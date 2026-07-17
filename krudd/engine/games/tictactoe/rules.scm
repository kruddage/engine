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

;;! Board state: nine cells, 0 = empty, 1 = X, 2 = O; whose turn it is (1/2); and
;;! the outcome, *ttt-over*: 0 while playing, 1 or 2 once that player has won, 3 on
;;! a draw. Once it is non-zero the board is frozen and the next click restarts.
(define *ttt-board* (make-vector 9 0))
(define *ttt-turn* 1)
(define *ttt-over* 0)

;;! (ttt-reset) clears the state only. (ttt-restart) also sweeps the placed marks
;;! off the board — it needs the world bound, so it runs from a click, not at load.
;;! ttt-reset is variadic because dispatch_scm always calls with one argument (a
;;! host reset command passes a throwaway), yet ttt-restart calls it with none.
(define (ttt-reset . ignored)
  (set! *ttt-board* (make-vector 9 0))
  (set! *ttt-turn* 1)
  (set! *ttt-over* 0))

(define (ttt-restart)
  (scene-destroy-named! "mark")
  (ttt-reset))

;;! (ttt-status ignored) -> *ttt-over*. A read hook the host (or a test) can poll
;;! through dispatch_scm, which calls with one integer argument.
(define (ttt-status ignored) *ttt-over*)

;;! The eight winning lines (three rows, three columns, two diagonals) as cell
;;! index triples; ttt-winner scans them after each move.
(define *ttt-lines*
  '((0 1 2) (3 4 5) (6 7 8)
    (0 3 6) (1 4 7) (2 5 8)
    (0 4 8) (2 4 6)))

;;! (ttt-winner) -> 1 or 2 if that player holds a full line, else 0.
(define (ttt-winner)
  (let loop ((ls *ttt-lines*))
    (if (null? ls)
        0
        (let* ((ln (car ls))
               (va (vector-ref *ttt-board* (car ln))))
          (if (and (not (= va 0))
                   (= va (vector-ref *ttt-board* (cadr ln)))
                   (= va (vector-ref *ttt-board* (caddr ln))))
              va
              (loop (cdr ls)))))))

;;! (ttt-full?) -> #t when no cell is empty (a draw once ttt-winner is 0).
(define (ttt-full?)
  (let loop ((i 0))
    (cond ((= i 9) #t)
          ((= 0 (vector-ref *ttt-board* i)) #f)
          (else (loop (+ i 1))))))

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

;;! (ttt-place-move cell kind) -> place the mark, record it, then resolve the
;;! board: a completed line ends the game for KIND, a full board is a draw, and
;;! otherwise the turn passes. Returns 1 (a move happened).
(define (ttt-place-move cell kind)
  (ttt-place cell kind)
  (vector-set! *ttt-board* cell kind)
  (let ((win (ttt-winner)))
    (cond ((not (= win 0))
           (set! *ttt-over* win)
           (krudd-log 0 (string-append "tictactoe: "
                         (if (= win 1) "X" "O") " wins")))
          ((ttt-full?)
           (set! *ttt-over* 3)
           (krudd-log 0 "tictactoe: draw"))
          (else
           (set! *ttt-turn* (if (= kind 1) 2 1)))))
  1)

;;! (ttt-on-selected id) -> 1 if the click did something, else 0. The plugin calls
;;! this with the world bound; a fault is caught so a bad click never takes the
;;! frame down. Once the game is over any click restarts. While playing, only an
;;! empty cell places — an occupied cell, a mark, or the board are all no-ops.
(define (ttt-on-selected id)
  (catch #t
    (lambda ()
      (if (not (= *ttt-over* 0))
          (begin (ttt-restart) 1)
          (let ((cell (ttt-cell-index (scene-entity-name id))))
            (if (and cell (< cell 9) (= 0 (vector-ref *ttt-board* cell)))
                (ttt-place-move cell *ttt-turn*)
                0))))
    (lambda args (krudd-log 2 "tictactoe: rule fault") 0)))
