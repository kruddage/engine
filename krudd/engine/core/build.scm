; SPDX-License-Identifier: GPL-2.0-or-later
;;! version.h.in is the header every binary reports itself by, substituted from
;;! git. runtime.scm is the prelude script.c evaluates first into the s7 image —
;;! embedded rather than loaded from disk, because in the browser there is none.
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
                      (root "abi/include") (root "world/entity/include") (root "base/math/include")
                      (root "game/host/include") (root "game/project/include"))
             (link "subsystem" "subsystem_manager" "log" "memory" "script")
             ;;! No game module in the list any more: a game is a (project ...)
             ;;! source the project host evaluates, not a plugin to link, and
             ;;! the one this image ships staged rides in as the
             ;;! STAGED_PROJECT_SCM embed a directory under projects/ declares.
             ;;! Exactly one may, which resolve-check-staged enforces and
             ;;! argues (#1019) — engine.c includes that header unconditionally.
             ;;!
             ;;! xr is the one name below that is not a plugin. There is no
             ;;! xr_plugin_entry and engine.c calls nothing in it: the engine
             ;;! does not know a WebXR session exists, and the initiative's
             ;;! rule is that it never learns (#987, #993). It is listed
             ;;! because the PAGE reaches it — its krudd_xr_* entry points are
             ;;! in EXPORTED_FUNCTIONS (kruddmake/ninja.scm), and an exported
             ;;! symbol is both what pulls its archive member into the module
             ;;! and what fails the link loudly if this line is ever dropped.
             (wasm-modules "asset_plugin" "edit_plugin" "entity_plugin"
                           "renderer_webgl" "renderer_webgpu" "frame_graph" "scene_renderer"
                           "viewport" "kruddgui" "audio_scriptnode"
                           "project_host" "xr"))

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

  ;;! frame_pacing has no public surface — engine.c is its only real caller —
  ;;! but the resync-across-a-gap rule it encodes (#991) is exactly the kind
  ;;! of thing worth pinning down natively rather than only by eyeballing the
  ;;! browser, so it is split out as its own library the way kruddgui splits
  ;;! kgui_input out for the same reason.
  (library "frame_pacing"
    (sources "frame_pacing.c"))
  (executable "frame_pacing_test"
              (sources "frame_pacing_test.c")
              (link "frame_pacing"))
  (test "frame_pacing" "frame_pacing_test")))
