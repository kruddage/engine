; SPDX-License-Identifier: GPL-2.0-or-later
;;! version.h.in is the header every binary reports itself by, substituted from
;;! git. runtime.scm is the prelude script.c evaluates first into the s7 image —
;;! embedded rather than loaded from disk, because in the browser there is none.
;;! editor_layout.scm is the editor chrome's spec, embedded the same way and for
;;! the same reason: script.c evaluates it to serialize the tree for the web
;;! shell. It lives here rather than beside the shell because `script` is a
;;! library every module may link, so it may not reach into a shell for its own
;;! generated header (#786 lists shell/ last precisely so nothing reaches into
;;! it). A shell reaching the other way, into core's generated header, is
;;! allowed.
((configure-file "version.h.in" "version.h")
 (embed "runtime.scm" "runtime_scm.h" "RUNTIME_SCM")
 (embed "editor_layout.scm" "editor_layout_scm.h" "LAYOUT_SCM")

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
                           "viewport" "kruddgui" "audio_scriptnode"
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
  (test "shader_transpile" "shader_transpile_test")

  ;;! The s7->JS layout serialization primitive (script_json /
  ;;! script_layout_json in script.c), exercised browser- and GPU-free: it
  ;;! serializes hand-built s7 values and the embedded editor_layout.scm spec and
  ;;! asserts on the JSON the web chrome (#706 part C) will JSON.parse. Needs
  ;;! ../third_party for s7.h to build the test values; the primitive itself
  ;;! rides in the linked script library. Part B of #706 / #723.
  (executable "script_layout_json_test"
              (sources "script_layout_json_test.c")
              (private "include" (raw "../third_party"))
              (link "script"))
  (test "script_layout_json" "script_layout_json_test")))
