; SPDX-License-Identifier: GPL-2.0-or-later
((library "subsystem"
   (sources "subsystem.c")
   (public "include"))

 (library "subsystem_manager"
   (sources "subsystem_manager.c")
   (public "include"))

 (library "script"
   (sources "script.c")
   (public "include")
   (private (raw "../third_party") (raw "${generated}"))
   (link "log" "m"))

 (executable "index"
             (sources "engine.c")
             (private "include" (raw "${generated}")
                      (root "abi"))
             (link "subsystem" "subsystem_manager" "log" "memory" "script")
             (wasm-modules "asset_plugin" "edit_plugin" "entity_plugin"
                           "renderer_webgl" "renderer_webgpu" "frame_graph" "scene_renderer"
                           "viewport" "kruddgui" "audio_scriptnode"
                           "tictactoe_game" "chess_game"))

 (native-only
  ;;! The offscreen WebGPU harness. Needs native Dawn, so it is skipped
  ;;! entirely unless KRUDD_DAWN_PREFIX is set — see tools/dawn-smoke/README.md.
  ;;! No (test ...) edge: it needs a real GPU adapter, which a CI runner has
  ;;! no business assuming.
  (executable "krudd_native"
              (sources "engine_native.c")
              (private "include" (raw "${generated}"))
              (dawn)
              (link "subsystem" "subsystem_manager" "log" "memory" "script"
                    "renderer_webgpu"))

  ;;! The render cluster boots a non-empty scene against the recording null
  ;;! backend — the GPU-free proof of editor_boot_cluster() (and thus the whole
  ;;! windowed scene path up to the backend boundary). No Dawn, no GPU, so it
  ;;! runs in ordinary CI, unlike krudd_native/krudd_qt.
  (executable "editor_boot_test"
              (sources "editor_boot_test.c" "editor_boot.c")
              (private "include" (raw "${generated}")
                       (root "render/null") (root "abi"))
              (link "renderer_null" "scene_renderer"
                    "frame_graph" "entity_plugin" "asset_plugin" "edit_plugin"
                    "mesh_script" "texture_script" "particles"
                    "subsystem" "subsystem_manager" "log" "memory" "script" "m"))
  (test "editor_boot" "editor_boot_test")

  ;;! The spec -> layout walk (editor_layout.c) that the Qt shell renders from,
  ;;! exercised GPU- and Qt-free: it evaluates the embedded editor_layout.scm
  ;;! spec through the shared s7 image and asserts on the C tree krudd_qt.cpp
  ;;! would emit. No Qt, no window, no GPU, so it runs in ordinary CI (unlike
  ;;! krudd_qt). Needs ../third_party for s7.h and ${generated} for LAYOUT_SCM.
  (executable "editor_layout_test"
              (sources "editor_layout_test.c" "editor_layout.c")
              (private "include" (raw "${generated}") (raw "../third_party"))
              (link "script"))
  (test "editor_layout" "editor_layout_test")

  ;;! The Qt-hosted native editor. The engine's Vulkan backend presenting into a
  ;;! QWindow embedded in real Qt chrome (menu bar, toolbar, docks). Part of
  ;;! #675/#676 (the Qt editor shell), on native Vulkan (#705) rather than native
  ;;! Dawn. Carries (vulkan) and (qt), so it is skipped unless BOTH the Vulkan
  ;;! loader (KRUDD_VULKAN) and Qt (KRUDD_QT) are configured — `./krudd.sh
  ;;! editor` sets both. Needs render/vulkan on the include path for the platform
  ;;! host seam (vulkan_platform.h). No (test ...) edge: it opens a window and
  ;;! needs a real GPU, so it is a deliverable, not a CI test.
  (executable "krudd_qt"
              (sources "krudd_qt.cpp" "editor_boot.c" "editor_layout.c")
              (private "include" (raw "${generated}") (raw "../third_party")
                       (root "render/vulkan") (root "abi"))
              (vulkan)
              (qt)
              (link "renderer_vulkan" "scene_renderer"
                    "frame_graph" "entity_plugin" "asset_plugin" "edit_plugin"
                    "mesh_script" "texture_script" "particles" "viewport_pick"
                    "subsystem" "subsystem_manager" "log" "memory" "script" "m"))

  (executable "subsystem_test"
              (sources "subsystem_test.c")
              (link "subsystem"))
  (test "subsystem" "subsystem_test")

  (executable "subsystem_manager_test"
              (sources "subsystem_manager_test.c")
              (link "subsystem_manager"))
  (test "subsystem_manager" "subsystem_manager_test")

  (executable "script_test"
              (sources "script_test.c")
              (link "script"))
  (test "script" "script_test")

  (executable "shader_transpile_test"
              (sources "shader_transpile_test.c")
              (link "script"))
  (test "shader_transpile" "shader_transpile_test")))
