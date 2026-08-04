; SPDX-License-Identifier: GPL-2.0-or-later
;;! The game viewport bridge (viewport.c): camera-aspect sync and click-to-pick,
;;! the game-facing half of the removed kruddboard overlay (#661). wasm-only —
;;! native builds host no games, canvas or kruddgui pointer. The raycast itself
;;! lives in viewport_pick.c (#697); the math
;;! it picks with (ray_from_screen / ray_tri_intersect / mat4_*) and
;;! mesh_script_generate resolve at the final WASM link against the single copies
;;! the renderer and the mesh_script library already provide.
;;!
;;! include/ is new with #996 and holds the two headers this module now
;;! PUBLISHES rather than keeps: viewport_pick.h, because the raycast takes a
;;! world-space ray a caller above ui/ may already hold, and pointer3d.h,
;;! because such a caller publishes its pointers through it. Both are consumed
;;! by the xr module, which sits above ui/ in the tier order and so calls
;;! downward into them — nothing here links, includes or names xr, and the
;;! viewport_pointer3d_publish it calls resolves at the final WASM link the way
;;! world_mesh_params already does.
((wasm-only
  (library "viewport"
    (sources "viewport.c" "viewport_pick.c" "viewport_pointer3d.c")
    (public "include")
    (private "." (root "abi/include") (root "world/entity/include") (root "base/math/include") (root "core/include") (root "world/asset/include")
             (raw "${generated}"))
    (link "mesh_script" "script" "log" "memory"
          "subsystem" "subsystem_manager")))

 (native-only
  ;;! GPU-free unit test: boot the s7 image, pick the built-in box out of a
  ;;! one-entity world. entity.c (world_mesh_params) and the math are compiled
  ;;! straight in, the way entity_test and scene_renderer_test do. Only
  ;;! viewport_pick.c is built here — the rest of the module is the wasm-only
  ;;! bridge above, and the raycast is the part with no browser in it.
  (executable "viewport_pick_test"
              (sources "viewport_pick_test.c" "viewport_pick.c"
                       (root "world/entity/entity.c")
                       (root "base/math/math.c") (root "base/math/camera.c")
                       (raw "${generated}/math_gen.c"))
              (private "." "include" (root "abi/include") (root "world/entity/include") (root "base/math/include") (root "core/include")
                       (root "base/memory/include") (root "world/asset/include")
                       (raw "${generated}") (raw "../third_party"))
              (link "mesh_script" "script" "memory" "log" "m"))
  (test "viewport_pick" "viewport_pick_test")))
