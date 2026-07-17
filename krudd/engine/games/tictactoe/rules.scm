; SPDX-License-Identifier: GPL-2.0-or-later

;;! tic-tac-toe rules — the game's logic, in Scheme, living in the shared image.
;;! This is the stateful, event-driven half a game needs and an entity script
;;! (a stateless per-frame animator) is not: it holds the board and whose turn it
;;! is across frames, and it runs only when a click arrives, not every tick.
;;!
;;! The tictactoe plugin loads this image and, each frame, hands the id of the
;;! entity under a fresh click to ttt-on-selected (via entity_api.dispatch_scm,
;;! which binds the live world so the scene-* primitives can spawn). A click on an
;;! empty "cell-N" pad places the current player's mark there — built with the
;;! same scene-entity-build the scene form uses — and passes the turn. Win/draw
;;! detection and reset are the next slice; this slice is placement + turns, 2p.

;;! Board state: nine cells, 0 = empty, 1 = X, 2 = O; and whose turn it is (1/2).
(define *ttt-board* (make-vector 9 0))
(define *ttt-turn* 1)

(define (ttt-reset)
  (set! *ttt-board* (make-vector 9 0))
  (set! *ttt-turn* 1))

;;! Cell layout: index i is row-major (i = row*3 + col), and the board spans
;;! [-1.5, 1.5], so cell centres fall on the unit grid x,z in {-1, 0, 1}.
(define (ttt-cell-x i) (- (modulo i 3) 1))
(define (ttt-cell-z i) (- (quotient i 3) 1))

;;! (ttt-cell-index name) -> the N of a "cell-N" pad, or #f for any other entity
;;! (a placed mark, the board, empty space) — the filter that turns "something was
;;! clicked" into "a specific empty square was chosen".
(define (ttt-cell-index name)
  (and (>= (string-length name) 6)
       (string=? (substring name 0 5) "cell-")
       (string->number (substring name 5))))

;;! Mark geometry, as (entity ...) forms for scene-entity-build — the same shapes
;;! the static scene authored, now emitted per placement at the chosen cell. An X
;;! is the two-bar composite (a mesh-less parent holding the crossed bars); an O
;;! is the flat torus ring.
(define (ttt-x-form x z)
  `(entity (name "mark") (at ,x 0.15 ,z)
     (children
       (entity (mesh "builtin://mesh/box") (material "builtin://material/pbr-plastic")
               (rotate 0 45 0) (scale 0.6 0.09 0.14))
       (entity (mesh "builtin://mesh/box") (material "builtin://material/pbr-plastic")
               (rotate 0 -45 0) (scale 0.6 0.09 0.14)))))

(define (ttt-o-form x z)
  `(entity (name "mark")
           (mesh "builtin://mesh/torus") (material "builtin://material/pbr-metal")
           (at ,x 0.15 ,z) (scale 0.38 0.38 0.38)))

(define (ttt-place i kind)
  (scene-entity-build
    (if (= kind 1) (ttt-x-form (ttt-cell-x i) (ttt-cell-z i))
                   (ttt-o-form (ttt-cell-x i) (ttt-cell-z i)))
    -1))

;;! (ttt-on-selected id) -> 1 if a mark was placed, else 0. The plugin calls this
;;! with the world bound; a fault is caught so a bad click never takes the frame
;;! down. Only an empty cell places: an occupied cell, a mark, or the board are
;;! all no-ops.
(define (ttt-on-selected id)
  (catch #t
    (lambda ()
      (let ((cell (ttt-cell-index (scene-entity-name id))))
        (if (and cell (< cell 9) (= 0 (vector-ref *ttt-board* cell)))
            (let ((kind *ttt-turn*))
              (ttt-place cell kind)
              (vector-set! *ttt-board* cell kind)
              (set! *ttt-turn* (if (= kind 1) 2 1))
              (krudd-log 0 (string-append "tictactoe: "
                            (if (= kind 1) "X" "O") " played cell "
                            (number->string cell)))
              1)
            0)))
    (lambda args (krudd-log 2 "tictactoe: rule fault") 0)))
