; SPDX-License-Identifier: GPL-2.0-or-later
;;! The web module is the only thing that links this library, so it is
;;! wasm-only. scene_renderer.c is plain C and carries a native (#else) service
;;! path — the native test below compiles it straight in rather than linking the
;;! library, the way every other GPU-free test here does.
;;! include/scene_renderer/scene_view.h is the module's public surface: the
;;! per-frame view list a frame is drawn from (#989). It is public rather than a
;;! private header beside the .c because its intended caller is a sibling module
;;! — the one that has N views to supply — and nothing else here is reachable
;;! from outside; the renderer itself is still reached through the subsystems it
;;! registers.
((wasm-only
  (library "scene_renderer"
    (sources "scene_renderer.c")
    (public "include")
    (private "." (raw "${generated}") (root "render/frame_graph/include")
             (root "render/particles/include")
             (root "core/include") (root "abi/include") (root "world/entity/include") (root "base/math/include") (root "world/asset/include")
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
              (private "." "include" (raw "${generated}")
                       (root "render/null/include")
                       (root "render/frame_graph/include") (root "world/asset/include")
                       (root "render/particles/include")
                       (root "core/include")
                       (root "abi/include") (root "world/entity/include") (root "base/math/include") (raw "../third_party"))
              (link "mesh_script" "texture_script" "frame_graph" "renderer_null"
                    "log" "memory" "subsystem_manager" "script" "m"))
  (test "scene_renderer" "scene_renderer_test")))
