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
;;! same scene-entity-build the scene form uses — and passes the turn. Completing a
;;! line ends the round: a strike bar is laid over the three cells and the winner's
;;! running tally (the scoreboard the host reads through ttt-score) ticks up. A full
;;! board is a draw; either way the next click restarts, and the tally carries over.

;;! Board state: nine cells, 0 = empty, 1 = X, 2 = O; whose turn it is (1/2); and
;;! the outcome, *ttt-over*: 0 while playing, 1 or 2 once that player has won, 3 on
;;! a draw. Once it is non-zero the board is frozen and the next click restarts.
(define *ttt-board* (make-vector 9 0))
(define *ttt-turn* 1)
(define *ttt-over* 0)

;;! Match score: running win tallies for X (player 1) and O (player 2). Unlike the
;;! board, these survive a round restart — they are the scoreboard, so a fresh win
;;! adds to them and only (ttt-score-reset) — run when the game is (re)loaded from
;;! the launcher — clears them back to nil-nil.
(define *ttt-score-x* 0)
(define *ttt-score-o* 0)

;;! (ttt-reset) clears the round state only (board, turn, outcome) — NOT the score,
;;! since (ttt-restart) routes through here between rounds and the tally must carry
;;! over. (ttt-restart) also sweeps the placed marks (and the win strike, which is
;;! named "mark" too) off the board — it needs the world bound, so it runs from a
;;! click, not at load. ttt-reset is variadic because dispatch_scm always calls with
;;! one argument (a host reset command passes a throwaway), yet ttt-restart calls it
;;! with none.
(define (ttt-reset . ignored)
  (set! *ttt-board* (make-vector 9 0))
  (set! *ttt-turn* 1)
  (set! *ttt-over* 0))

(define (ttt-restart)
  (scene-destroy-named! "mark")
  (ttt-reset))

;;! (ttt-score-reset ignored) zeroes both tallies — a fresh match. Variadic-by-arg
;;! like ttt-status: the host dispatches it with one throwaway integer on game load.
(define (ttt-score-reset . ignored)
  (set! *ttt-score-x* 0)
  (set! *ttt-score-o* 0))

;;! (ttt-status ignored) -> *ttt-over*. A read hook the host (or a test) can poll
;;! through dispatch_scm, which calls with one integer argument.
(define (ttt-status ignored) *ttt-over*)

;;! (ttt-score ignored) -> both tallies packed into one integer, X*1000 + O, so the
;;! host can read the whole scoreboard in a single dispatch_scm poll and unpack it
;;! (x = n / 1000, o = n mod 1000). 1000 leaves ample room for any real session.
(define (ttt-score ignored)
  (+ (* *ttt-score-x* 1000) *ttt-score-o*))

;;! The eight winning lines (three rows, three columns, two diagonals) as cell
;;! index triples; ttt-winner scans them after each move.
(define *ttt-lines*
  '((0 1 2) (3 4 5) (6 7 8)
    (0 3 6) (1 4 7) (2 5 8)
    (0 4 8) (2 4 6)))

;;! (ttt-line-full? ln) -> #t when all three cells of LN hold the same non-empty
;;! mark — the shared test behind ttt-winner (which line owner) and ttt-winning-line
;;! (which triple), so the two never drift apart.
(define (ttt-line-full? ln)
  (let ((va (vector-ref *ttt-board* (car ln))))
    (and (not (= va 0))
         (= va (vector-ref *ttt-board* (cadr ln)))
         (= va (vector-ref *ttt-board* (caddr ln))))))

;;! (ttt-winner) -> 1 or 2 if that player holds a full line, else 0.
(define (ttt-winner)
  (let loop ((ls *ttt-lines*))
    (cond ((null? ls) 0)
          ((ttt-line-full? (car ls)) (vector-ref *ttt-board* (caar ls)))
          (else (loop (cdr ls))))))

;;! (ttt-winning-line) -> the completed (a b c) triple, or #f while none is. Feeds
;;! the strike-through: the line's two end cells give the bar its span and angle.
(define (ttt-winning-line)
  (let loop ((ls *ttt-lines*))
    (cond ((null? ls) #f)
          ((ttt-line-full? (car ls)) (car ls))
          (else (loop (cdr ls))))))

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
;;! is the two-bar composite (a mesh-less parent holding the crossed bars), in
;;! toon-x; an O is the flat torus ring, in toon-o. Both bind the cel shader the
;;! rest of the scene wears (see scene.scm), so a placed mark reads as part of the
;;! same cartoon world — a bold red X, a bold blue O, banded and ink-edged.
(define (ttt-x-form x z)
  `(entity (name "mark") (at ,x 0.15 ,z)
     (children
       (entity (mesh "builtin://mesh/box") (material "builtin://material/toon-x")
               (rotate 0 45 0) (scale 0.6 0.09 0.14))
       (entity (mesh "builtin://mesh/box") (material "builtin://material/toon-x")
               (rotate 0 -45 0) (scale 0.6 0.09 0.14)))))

(define (ttt-o-form x z)
  `(entity (name "mark")
           (mesh "builtin://mesh/torus") (material "builtin://material/toon-o")
           (at ,x 0.15 ,z) (scale 0.38 0.38 0.38)))

;;! (ttt-strike-material kind) -> the winning line's bar material: KIND's own
;;! toon tint, so an X win strikes through in red and an O win in blue rather
;;! than always the same colour regardless of who won.
(define (ttt-strike-material kind)
  (if (= kind 1) "builtin://material/toon-x" "builtin://material/toon-o"))

;;! Spark colour for a mark: X sparks red, O sparks blue — the same toon tints
;;! the marks and strike wear (toon-x / toon-o), so a burst reads as that
;;! player's colour.
(define (ttt-mark-rgb kind)
  (if (= kind 1) '(0.92 0.2 0.24) '(0.18 0.46 0.95)))

;;! (ttt-spark x z kind count) fires a cosmetic particle burst of COUNT particles
;;! at world (x, z) in KIND's colour, a little above the board. Guarded on
;;! particle-burst! being bound: the engine's render layer registers it (see
;;! scene_renderer's register_particle_script), but the headless rules test runs
;;! with no renderer, so there the primitive is absent and the effect is simply
;;! skipped — the rules never depend on it.
(define (ttt-spark x z kind count)
  (when (defined? 'particle-burst!)
    (let ((c (ttt-mark-rgb kind)))
      (particle-burst! x 0.3 z (car c) (cadr c) (caddr c) count))))

(define (ttt-place i kind)
  (scene-entity-build
    (if (= kind 1) (ttt-x-form (ttt-cell-x i) (ttt-cell-z i))
                   (ttt-o-form (ttt-cell-x i) (ttt-cell-z i)))
    -1))

;;! (ttt-strike kind ln) draws the win: a thin bar laid over the three cells of
;;! the winning line LN, spanning from its first cell's centre to its last, in
;;! KIND's own gem material (see ttt-strike-material) so the strike reads as
;;! that player's colour, not a fixed third colour. The bar is the unit box
;;! scaled long on X; a single Y rotation turns that +X bar to lie along the
;;! line. The four line orientations pin the angle exactly — a row is flat (0),
;;! a column is a quarter turn (90), and the two diagonals are ±45 — so no trig
;;! is needed; a box is symmetric under a half turn, so either diagonal sign
;;! reads the same. It rides above the marks (y = 0.32) and, being named "mark",
;;! ttt-restart sweeps it away with them. LN may be #f (a non-win), which is a
;;! no-op.
(define (ttt-strike kind ln)
  (when (pair? ln)
    (let* ((a  (car ln))         (c  (caddr ln))
           (ax (ttt-cell-x a))   (az (ttt-cell-z a))
           (cx (ttt-cell-x c))   (cz (ttt-cell-z c))
           (dx (- cx ax))        (dz (- cz az))
           (mx (/ (+ ax cx) 2))  (mz (/ (+ az cz) 2))
           (len (sqrt (+ (* dx dx) (* dz dz))))
           (ang (cond ((= dz 0) 0)
                      ((= dx 0) 90)
                      ((= dx dz) -45)
                      (else 45))))
      (scene-entity-build
        `(entity (name "mark")
                 (mesh "builtin://mesh/box") (material ,(ttt-strike-material kind))
                 (at ,mx 0.32 ,mz) (rotate 0 ,ang 0)
                 (scale ,(+ len 0.5) 0.08 0.14))
        -1)
      ;;! A bigger celebratory burst along the winning line's midpoint.
      (ttt-spark mx mz kind 90))))

;;! (ttt-award kind) credits the win to KIND's running tally — the scoreboard the
;;! host reads through ttt-score. A draw awards nothing, so this runs only on a win.
(define (ttt-award kind)
  (if (= kind 1)
      (set! *ttt-score-x* (+ *ttt-score-x* 1))
      (set! *ttt-score-o* (+ *ttt-score-o* 1))))

;;! (ttt-place-move cell kind) -> place the mark, record it, then resolve the
;;! board: a completed line ends the game for KIND — strike the line through and
;;! credit the win — a full board is a draw, and otherwise the turn passes. Returns
;;! 1 (a move happened).
(define (ttt-place-move cell kind)
  (ttt-place cell kind)
  (ttt-spark (ttt-cell-x cell) (ttt-cell-z cell) kind 34)
  (vector-set! *ttt-board* cell kind)
  (let ((win (ttt-winner)))
    (cond ((not (= win 0))
           (set! *ttt-over* win)
           (ttt-strike win (ttt-winning-line))
           (ttt-award win)
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
