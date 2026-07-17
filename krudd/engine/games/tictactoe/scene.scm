; SPDX-License-Identifier: GPL-2.0-or-later

;;! The tic-tac-toe scene — the first built-in game, and the proof that a game is
;;! authored data (a (scene ...) form) on top of the engine's generic scene
;;! builder, not new engine C. See core/scene_script.scm for the clause grammar.
;;!
;;! Slice 0 is static: a 3x3 board (a plane wearing the checker material) with a
;;! few marks placed on it, so booting confirms a hand-authored .scm populates
;;! and renders the world. Later slices make the cells pickable and the marks a
;;! function of play — but the board and pieces stay exactly these assets.
;;!
;;! The board plane's outward normal is +Y (see PLANE_MESH_SCRIPT_SRC), so it lies
;;! flat in the XZ plane; scaled by 3 it spans [-1.5, 1.5], and the nine cell
;;! centres sit at x,z in {-1, 0, 1}. Marks ride just above the surface (y = 0.15).

(scene tic-tac-toe
  (entity (name "board")
          (mesh     "builtin://mesh/plane")
          (material "builtin://material/checker")
          (at 0 0 0) (scale 3 3 3))

  ;;! O — a ring lying flat (the revolved torus, axis +Y).
  (entity (name "o-a1")
          (mesh     "builtin://mesh/torus")
          (material "builtin://material/pbr-metal")
          (at -1 0.15 -1) (scale 0.4 0.4 0.4))
  (entity (name "o-c3")
          (mesh     "builtin://mesh/torus")
          (material "builtin://material/pbr-metal")
          (at 1 0.15 1) (scale 0.4 0.4 0.4))

  ;;! X — a small cube, tilted 45 degrees so it reads as a cross from above.
  (entity (name "x-b2")
          (mesh     "builtin://mesh/box")
          (material "builtin://material/pbr-plastic")
          (at 0 0.15 0) (rotate 0 45 0) (scale 0.5 0.15 0.5)))
