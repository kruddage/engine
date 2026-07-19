; SPDX-License-Identifier: GPL-2.0-or-later
;;! The game viewport bridge (viewport.c): camera-aspect sync and click-to-pick,
;;! the game-facing half of the removed kruddboard overlay (#661). wasm-only —
;;! native builds host no games, canvas or kruddgui pointer. The math it picks
;;! with (ray_from_screen / ray_tri_intersect / mat4_*) and mesh_script_generate
;;! resolve at the final WASM link against the single copies the renderer and the
;;! mesh_script library already provide.
((wasm-only
  (library "viewport"
    (sources "viewport.c")
    (private "." (root "abi") (root "core/include") (root "asset")
             (raw "${generated}"))
    (link "mesh_script" "script" "log" "memory"
          "subsystem" "subsystem_manager"))))
