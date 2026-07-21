; SPDX-License-Identifier: GPL-2.0-or-later
;;! The game viewport bridge (viewport.c): camera-aspect sync and click-to-pick,
;;! the game-facing half of the removed kruddboard overlay (#661). wasm-only —
;;! native builds host no games, canvas or kruddgui pointer. The raycast itself
;;! lives in viewport_pick.c (#697), shared with the native Qt editor; the math
;;! it picks with (ray_from_screen / ray_tri_intersect / mat4_*) and
;;! mesh_script_generate resolve at the final WASM link against the single copies
;;! the renderer and the mesh_script library already provide.
((wasm-only
  (library "viewport"
    (sources "viewport.c" "viewport_pick.c")
    (private "." (root "abi") (root "core/include") (root "asset")
             (raw "${generated}"))
    (link "mesh_script" "script" "log" "memory"
          "subsystem" "subsystem_manager")))

 (native-only
  ;;! The shared click-to-pick raycast, native side. krudd_qt links this; the
  ;;! math (ray_* / mat4_*) and world_mesh_params it references resolve at the
  ;;! krudd_qt link against scene_renderer and entity_plugin, which it already
  ;;! links — the "provided by the final link" arrangement the wasm viewport
  ;;! library above relies on, here for a native executable.
  (library "viewport_pick"
    (sources "viewport_pick.c")
    (public "." (root "abi"))
    (private (root "core/include") (root "asset") (raw "${generated}"))
    (link "mesh_script"))

  ;;! GPU-free unit test: boot the s7 image, pick the built-in box out of a
  ;;! one-entity world. entity.c (world_mesh_params) and the math are compiled
  ;;! straight in, the way entity_test and scene_renderer_test do.
  (executable "viewport_pick_test"
              (sources "viewport_pick_test.c" "viewport_pick.c"
                       (root "entity/entity.c")
                       (root "math/math.c") (root "math/camera.c")
                       (raw "${generated}/math_gen.c"))
              (private "." (root "abi") (root "core/include")
                       (root "memory/include") (root "asset")
                       (raw "${generated}") (raw "../third_party"))
              (link "mesh_script" "script" "memory" "log" "m"))
  (test "viewport_pick" "viewport_pick_test")))
