; SPDX-License-Identifier: GPL-2.0-or-later

;;! The tic-tac-toe scene — the board and its nine empty cells, floating over a
;;! grassy heightfield. As of Slice 2 the game is playable, so the scene starts
;;! empty: no pre-placed marks. Clicking a cell runs the rules
;;! (games/tictactoe/rules.scm), which spawns a mark there.
;;!
;;! The whole scene is cel-shaded: every material here binds builtin://shader/toon
;;! (the toon-* set in asset/asset_plugin.c), a flat, banded, ink-outlined cartoon
;;! shading model — deliberately nothing like the pbr look games/chess wears, so
;;! the two games read as visibly different worlds even though they walk the same
;;! scene pipeline (issue #644). Switching a material for a toon-* variant is the
;;! whole change; the layout below is untouched.
;;!
;;! Each cell is a flat pickable pad named "cell-N" (row-major, N = row*3 + col).
;;! The engine's existing ray pick (kruddboard) already turns a viewport click
;;! into the entity under it and selects it; the tictactoe plugin watches that
;;! selection and hands a freshly-clicked "cell-N" to the rules. So the pads carry
;;! two jobs: they are what the ray hits, and their name is the cell's identity.
;;! They are a calm cream toon-cell — one flat colour per pad reads as a game
;;! board, where nine busy checkered pads read cluttered up close. The board frame
;;! (toon-board) sits a shade darker so the grid stands out against it.
;;!
;;! The board plane's outward normal is +Y, so it lies flat in XZ; scaled by 3 it
;;! spans [-1.5, 1.5]. Cell centres sit on the unit grid (x,z in {-1, 0, 1}); the
;;! pads ride just above the board (y = 0.02) so a click hits a pad, not the slab.
;;! Both stay at y = 0 — the camera (below) and rules.scm's mark-placement
;;! heights are both tuned against that, so "the board floats above the ground"
;;! is achieved by sinking "ground" well below it instead, not by moving the
;;! board up.
;;!
;;! "ground" is a builtin://mesh/heightfield (a gently wavy parametric surface;
;;! see core/mesh_script.scm) wearing builtin://material/toon-grass — a flat
;;! cartoon green off the toon shader. The heightfield's own macro waviness gives
;;! it shape, and the toon banding paints that shape as a few hard zones of light
;;! and shade, so it reads as a stylised grassy hill rather than a photoreal lawn
;;! (the pbr-grass this replaced carried a bumped procedural micro-detail that
;;! belongs to the realistic look, not this one). Scaled wide (14 units) and sunk
;;! to y = -0.75 so its highest crest (default heightfield amp 0.15, scaled with
;;! it) still sits well clear of the board's underside, reading as solid ground
;;! the platform hovers over rather than a slab cutting through it.
;;!
;;! The "Camera" entity gives the scene its own eye: scene_renderer reads the
;;! world position of whatever entity is named "Camera" into the camera eye each
;;! frame (the view target stays the origin, where the board is centred). A flat
;;! board seen edge-on is invisible, so this parks the eye high and in front for
;;! a fixed 3/4 top-down view that reads the grid — no mesh, so it neither draws
;;! nor gets picked. It is authored here rather than inherited because switching
;;! to this game clears the world, taking the boot scene's orbit camera with it.

(scene tic-tac-toe
  (entity (name "Camera") (at 0 4 3.5))

  (entity (name "ground")
          (mesh     "builtin://mesh/heightfield")
          (material "builtin://material/toon-grass")
          (at 0 -0.75 0) (scale 14 1 14))

  (entity (name "board")
          (mesh     "builtin://mesh/plane")
          (material "builtin://material/toon-board")
          (at 0 0 0) (scale 3 3 3))

  (entity (name "cell-0") (mesh "builtin://mesh/plane")
          (material "builtin://material/toon-cell") (at -1 0.02 -1) (scale 0.9 0.9 0.9))
  (entity (name "cell-1") (mesh "builtin://mesh/plane")
          (material "builtin://material/toon-cell") (at 0 0.02 -1) (scale 0.9 0.9 0.9))
  (entity (name "cell-2") (mesh "builtin://mesh/plane")
          (material "builtin://material/toon-cell") (at 1 0.02 -1) (scale 0.9 0.9 0.9))
  (entity (name "cell-3") (mesh "builtin://mesh/plane")
          (material "builtin://material/toon-cell") (at -1 0.02 0) (scale 0.9 0.9 0.9))
  (entity (name "cell-4") (mesh "builtin://mesh/plane")
          (material "builtin://material/toon-cell") (at 0 0.02 0) (scale 0.9 0.9 0.9))
  (entity (name "cell-5") (mesh "builtin://mesh/plane")
          (material "builtin://material/toon-cell") (at 1 0.02 0) (scale 0.9 0.9 0.9))
  (entity (name "cell-6") (mesh "builtin://mesh/plane")
          (material "builtin://material/toon-cell") (at -1 0.02 1) (scale 0.9 0.9 0.9))
  (entity (name "cell-7") (mesh "builtin://mesh/plane")
          (material "builtin://material/toon-cell") (at 0 0.02 1) (scale 0.9 0.9 0.9))
  (entity (name "cell-8") (mesh "builtin://mesh/plane")
          (material "builtin://material/toon-cell") (at 1 0.02 1) (scale 0.9 0.9 0.9)))
