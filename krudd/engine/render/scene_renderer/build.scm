; SPDX-License-Identifier: GPL-2.0-or-later
;;! The scene renderer library builds for BOTH targets. It was wasm-only when
;;! only the web module linked it; the native editor (krudd_qt, #675) needs it
;;! too, and scene_renderer.c already carries a native (#else) service path — it
;;! is plain C, so "wasm-only" was a packaging accident, not a real constraint.
((library "scene_renderer"
   (sources "scene_renderer.c"
            (root "math/math.c")
            (root "math/camera.c")
            (raw "${generated}/math_gen.c"))
   (private "." (raw "${generated}") (root "render/frame_graph")
            (root "render/particles")
            (root "core/include") (root "abi") (root "asset")
            (raw "../third_party"))
   (link "mesh_script" "texture_script" "frame_graph" "particles"
         "log" "memory" "subsystem" "subsystem_manager" "script"
         "m"))
 (native-only
  (executable "scene_renderer_test"
              (sources "scene_renderer_test.c" "scene_renderer.c"
                       (root "render/particles/particles.c")
                       (root "math/math.c")
                       (root "math/camera.c")
                       (raw "${generated}/math_gen.c"))
              (private "." (raw "${generated}")
                       (root "render/null")
                       (root "render/frame_graph") (root "asset")
                       (root "render/particles")
                       (root "core/include")
                       (root "abi") (raw "../third_party"))
              (link "mesh_script" "texture_script" "frame_graph" "renderer_null"
                    "log" "memory" "subsystem_manager" "script" "m"))
  (test "scene_renderer" "scene_renderer_test")))
