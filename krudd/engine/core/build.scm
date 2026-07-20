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

  ;;! The Qt-hosted WebGPU harness — the native editor. The same backend as
  ;;! krudd_native, presenting into a QWindow embedded in real Qt chrome
  ;;! (menu bar, toolbar, docks) instead of reading a frame back offscreen.
  ;;! Part of #675/#676 (the Qt editor shell). Carries (dawn) and (qt), so it is
  ;;! skipped unless BOTH native Dawn (KRUDD_DAWN_PREFIX) and Qt (KRUDD_QT) are
  ;;! configured — `./krudd.sh editor` sets the latter. Needs render/webgpu on
  ;;! the include path for the platform host seam (webgpu_platform.h). No
  ;;! (test ...) edge: it opens a window and needs a real GPU adapter, so it is
  ;;! a deliverable, not a CI test.
  (executable "krudd_qt"
              (sources "krudd_qt.cpp")
              (private "include" (raw "${generated}")
                       (root "render/webgpu"))
              (dawn)
              (qt)
              (link "subsystem" "subsystem_manager" "log" "memory" "script"
                    "renderer_webgpu"))

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
