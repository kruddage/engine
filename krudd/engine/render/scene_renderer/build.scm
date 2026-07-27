; SPDX-License-Identifier: GPL-2.0-or-later
;;! The web module is the only thing that links this library, so it is
;;! wasm-only. scene_renderer.c is plain C and carries a native (#else) service
;;! path — the native test below compiles it straight in rather than linking the
;;! library, the way every other GPU-free test here does.
((wasm-only
  (library "scene_renderer"
    (sources "scene_renderer.c")
    (private "." (raw "${generated}") (root "render/frame_graph")
             (root "render/particles/include")
             (root "core/include") (root "abi") (root "world/entity/include") (root "base/math/include") (root "world/asset/include")
             (raw "../third_party"))
    (link "math" "mesh_script" "texture_script" "frame_graph" "particles"
          "log" "memory" "subsystem" "subsystem_manager" "script"
          "m")))
 (native-only
  (executable "scene_renderer_test"
              (sources "scene_renderer_test.c" "scene_renderer.c"
                       (root "render/particles/particles.c")
                       (root "base/math/math.c")
                       (root "base/math/camera.c")
                       (raw "${generated}/math_gen.c"))
              (private "." (raw "${generated}")
                       (root "render/null")
                       (root "render/frame_graph") (root "world/asset/include")
                       (root "render/particles/include")
                       (root "core/include")
                       (root "abi") (root "world/entity/include") (root "base/math/include") (raw "../third_party"))
              (link "mesh_script" "texture_script" "frame_graph" "renderer_null"
                    "log" "memory" "subsystem_manager" "script" "m"))
  (test "scene_renderer" "scene_renderer_test")))
