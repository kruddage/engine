; SPDX-License-Identifier: GPL-2.0-or-later

;;! The tic-tac-toe scene — the board and its nine empty cells. As of Slice 2 the
;;! game is playable, so the scene starts empty: no pre-placed marks. Clicking a
;;! cell runs the rules (games/tictactoe/rules.scm), which spawns a mark there.
;;!
;;! Each cell is a flat pickable pad named "cell-N" (row-major, N = row*3 + col).
;;! The engine's existing ray pick (kruddboard) already turns a viewport click
;;! into the entity under it and selects it; the tictactoe plugin watches that
;;! selection and hands a freshly-clicked "cell-N" to the rules. So the pads carry
;;! two jobs: they are what the ray hits, and their name is the cell's identity.
;;!
;;! The board plane's outward normal is +Y, so it lies flat in XZ; scaled by 3 it
;;! spans [-1.5, 1.5]. Cell centres sit on the unit grid (x,z in {-1, 0, 1}); the
;;! pads ride just above the board (y = 0.02) so a click hits a pad, not the slab.

(scene tic-tac-toe
  (entity (name "board")
          (mesh     "builtin://mesh/plane")
          (material "builtin://material/pbr-metal")
          (at 0 0 0) (scale 3 3 3))

  (entity (name "cell-0") (mesh "builtin://mesh/plane")
          (material "builtin://material/checker") (at -1 0.02 -1) (scale 0.9 0.9 0.9))
  (entity (name "cell-1") (mesh "builtin://mesh/plane")
          (material "builtin://material/checker") (at 0 0.02 -1) (scale 0.9 0.9 0.9))
  (entity (name "cell-2") (mesh "builtin://mesh/plane")
          (material "builtin://material/checker") (at 1 0.02 -1) (scale 0.9 0.9 0.9))
  (entity (name "cell-3") (mesh "builtin://mesh/plane")
          (material "builtin://material/checker") (at -1 0.02 0) (scale 0.9 0.9 0.9))
  (entity (name "cell-4") (mesh "builtin://mesh/plane")
          (material "builtin://material/checker") (at 0 0.02 0) (scale 0.9 0.9 0.9))
  (entity (name "cell-5") (mesh "builtin://mesh/plane")
          (material "builtin://material/checker") (at 1 0.02 0) (scale 0.9 0.9 0.9))
  (entity (name "cell-6") (mesh "builtin://mesh/plane")
          (material "builtin://material/checker") (at -1 0.02 1) (scale 0.9 0.9 0.9))
  (entity (name "cell-7") (mesh "builtin://mesh/plane")
          (material "builtin://material/checker") (at 0 0.02 1) (scale 0.9 0.9 0.9))
  (entity (name "cell-8") (mesh "builtin://mesh/plane")
          (material "builtin://material/checker") (at 1 0.02 1) (scale 0.9 0.9 0.9)))
