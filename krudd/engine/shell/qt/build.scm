; SPDX-License-Identifier: GPL-2.0-or-later
((native-only
  ;;! The render cluster boots a non-empty scene against the recording null
  ;;! backend — the GPU-free proof of editor_boot_cluster() (and thus the whole
  ;;! windowed scene path up to the backend boundary). No Dawn, no GPU, so it
  ;;! runs in ordinary CI, unlike krudd_native/krudd_qt.
  (executable "editor_boot_test"
              (sources "editor_boot_test.c" "editor_boot.c")
              (private "." (raw "${generated}")
                       (root "core/include")
                       (root "render/null") (root "abi")
                       (root "world/entity/include")
                       (root "base/math/include"))
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
              (private "." (raw "${generated}")
                       (root "core/include") (raw "../third_party"))
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
              (private "." (raw "${generated}") (raw "../third_party")
                       (root "core/include")
                       (root "render/vulkan") (root "abi")
                       (root "world/entity/include")
                       (root "base/math/include"))
              (vulkan)
              (qt)
              (link "renderer_vulkan" "scene_renderer"
                    "frame_graph" "entity_plugin" "asset_plugin" "edit_plugin"
                    "mesh_script" "texture_script" "particles" "viewport_pick"
                    "subsystem" "subsystem_manager" "log" "memory" "script" "m"))))
