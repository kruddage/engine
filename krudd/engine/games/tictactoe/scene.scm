; SPDX-License-Identifier: GPL-2.0-or-later

;;! The tic-tac-toe scene — the first built-in game, and the proof that a game is
;;! authored data (a (scene ...) form) on top of the engine's generic scene
;;! builder, not new engine C. See core/scene_script.scm for the clause grammar.
;;!
;;! Slice 1 is still static, but it is a real board now: a 3x3 plane wearing the
;;! checker material, with proper marks. An O is the built-in torus (a ring lying
;;! flat). An X is a composite — a mesh-less parent at the cell holding two
;;! crossed box "bars" as children, so the two diagonals move and scale as one
;;! piece. Later slices make the cells pickable and the marks a function of play;
;;! the board and pieces stay exactly these assets.
;;!
;;! The board plane's outward normal is +Y (see PLANE_MESH_SCRIPT_SRC), so it lies
;;! flat in the XZ plane; scaled by 3 it spans [-1.5, 1.5], and the nine cell
;;! centres sit at x,z in {-1, 0, 1}. Marks ride just above the surface (y = 0.15).
;;! The sample position drawn here is
;;!     O . X
;;!     . X .
;;!     X . O

(scene tic-tac-toe
  (entity (name "board")
          (mesh     "builtin://mesh/plane")
          (material "builtin://material/checker")
          (at 0 0 0) (scale 3 3 3))

  ;;! O — the built-in torus, standing in as a ring lying flat (axis +Y).
  (entity (name "o-a1")
          (mesh     "builtin://mesh/torus")
          (material "builtin://material/pbr-metal")
          (at -1 0.15 -1) (scale 0.38 0.38 0.38))
  (entity (name "o-c3")
          (mesh     "builtin://mesh/torus")
          (material "builtin://material/pbr-metal")
          (at 1 0.15 1) (scale 0.38 0.38 0.38))

  ;;! X — a mesh-less parent at the cell holding two crossed bars. Each bar is the
  ;;! unit box scaled long-and-flat and turned +/-45 degrees about Y; the parent
  ;;! carries the cell position, so the pair reads as one X from above.
  (entity (name "x-c1") (at 1 0.15 -1)
          (children
            (entity (mesh "builtin://mesh/box")
                    (material "builtin://material/pbr-plastic")
                    (rotate 0 45 0) (scale 0.6 0.09 0.14))
            (entity (mesh "builtin://mesh/box")
                    (material "builtin://material/pbr-plastic")
                    (rotate 0 -45 0) (scale 0.6 0.09 0.14))))
  (entity (name "x-b2") (at 0 0.15 0)
          (children
            (entity (mesh "builtin://mesh/box")
                    (material "builtin://material/pbr-plastic")
                    (rotate 0 45 0) (scale 0.6 0.09 0.14))
            (entity (mesh "builtin://mesh/box")
                    (material "builtin://material/pbr-plastic")
                    (rotate 0 -45 0) (scale 0.6 0.09 0.14))))
  (entity (name "x-a3") (at -1 0.15 1)
          (children
            (entity (mesh "builtin://mesh/box")
                    (material "builtin://material/pbr-plastic")
                    (rotate 0 45 0) (scale 0.6 0.09 0.14))
            (entity (mesh "builtin://mesh/box")
                    (material "builtin://material/pbr-plastic")
                    (rotate 0 -45 0) (scale 0.6 0.09 0.14)))))
