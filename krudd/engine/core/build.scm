; SPDX-License-Identifier: GPL-2.0-or-later
;;! version.h.in is the header every binary reports itself by, substituted from
;;! git. runtime.scm is the prelude script.c evaluates first into the s7 image —
;;! embedded rather than loaded from disk, because in the browser there is none.
;;!
;;! editor_layout.scm was embedded here too, for the same reason, and #953
;;! retired it with the rest of the Scheme chrome: the editor is a TypeScript
;;! application now and reads its own dock list out of its own source. The
;;! mechanism was good — a menu added to the spec reached the page with no edit
;;! to the markup — and it lost to a substrate decision (#944 Q3), not to a
;;! flaw. It is recoverable from the commit before this one.
((configure-file "version.h.in" "version.h")
 (embed "runtime.scm" "runtime_scm.h" "RUNTIME_SCM")

 (library "subsystem"
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
                      (root "abi") (root "world/entity/include") (root "base/math/include") (root "game/host/include"))
             (link "subsystem" "subsystem_manager" "log" "memory" "script")
             (wasm-modules "asset_plugin" "edit_plugin" "entity_plugin"
                           "renderer_webgl" "renderer_webgpu" "frame_graph" "scene_renderer"
                           "viewport" "gizmo" "kruddgui" "bridge"
                           "audio_scriptnode"
                           "chess_game"))

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
