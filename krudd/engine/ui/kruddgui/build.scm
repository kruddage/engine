; SPDX-License-Identifier: GPL-2.0-or-later
;;! The immediate-mode gui's Scheme half, embedded into the s7 image that
;;! kruddgui.cpp drives.
;;!
;;! kruddgui is the *canvas* layer, and that line is the one #902 rests on:
;;!
;;!   Chrome is DOM. Canvas is kruddgui.
;;!
;;! Chrome is everything around the viewport — menus, toolbar, docks, panels,
;;! status bar — and it is the page's, in HTML. Canvas is everything drawn over
;;! the game in play view, where there is no chrome and there must not be, and
;;! it is kruddgui's, in C and Scheme. When something is genuinely both, it is
;;! chrome, and kruddgui takes the smaller job.
;;!
;;! So kruddgui's permanent job is exactly: the GAME / EDITOR switch (the only
;;! way out of either mode, so it can never be gated on the mode it shows), the
;;! perf HUD (visible in a game's play view), gizmos and viewport overlays (the
;;! kruddgui_api seam, which runs before the panels while the pointer's
;;! one-frame click edge is still live), a game's own HUD, and the multi-touch
;;! pointer router in kgui_input.c — the only touch-capable input layer in the
;;! tree, and what viewport.c's click-to-pick stands on.
;;!
;;! What it is not is an editor. The scene tree, inspector, asset browser and
;;! log console are the DOM chrome's; the parked Scheme panels in kruddgui.scm
;;! that implement them are not coming back, and nothing new should be added to
;;! them. They and their tests are kept green rather than deleted (#902 Q6):
;;! they share this file's layout, widget and markdown code with the two panels
;;! that do ship, so deleting them is an untangling, not a `git rm`, and their
;;! tests are what hold that shared code up. Kept, not revived — the moment
;;! their shared code has moved out from under them, they go.
((embed "kruddgui.scm" "kruddgui_scm.h" "KRUDDGUI_SCM")

 (wasm-only
  (library "kruddgui"
    (wasm-flags "--std=c++17" "-fno-exceptions" "-fno-rtti")
    (sources "kruddgui.cpp" "kgui_batch.c" "kgui_input.c"
             "kgui_text_edit.c" "kgui_font.c" "kgui_stats.c")
    (private "." (raw "${generated}") (raw "../third_party"))
    (link "script" "log" "memory" "subsystem"
          "subsystem_manager")))

 (native-only
  ;;! kgui_batch links libm for the vector primitives' sinf/cosf/sqrtf.
  (library "kgui_batch"
    (sources "kgui_batch.c")
    (public ".")
    (link "m"))
  (executable "kgui_batch_test"
              (sources "kgui_batch_test.c")
              (link "kgui_batch"))
  (test "kgui_batch" "kgui_batch_test")

  (library "kgui_input"
    (sources "kgui_input.c")
    (public "."))
  (executable "kgui_input_test"
              (sources "kgui_input_test.c")
              (link "kgui_input"))
  (test "kgui_input" "kgui_input_test")

  (library "kgui_text_edit"
    (sources "kgui_text_edit.c")
    (public "."))
  (executable "kgui_text_edit_test"
              (sources "kgui_text_edit_test.c")
              (link "kgui_text_edit"))
  (test "kgui_text_edit" "kgui_text_edit_test")

  ;;! kgui_font bakes an SDF atlas with the vendored stb_truetype
  ;;! (../third_party), which pulls in libm (floor/sqrt/pow/...).
  (library "kgui_font"
    (sources "kgui_font.c")
    (public ".")
    (private (raw "../third_party"))
    (link "m"))
  (executable "kgui_font_test"
              (sources "kgui_font_test.c")
              (link "kgui_font" "kgui_batch"))
  (test "kgui_font" "kgui_font_test")

  (executable "kgui_scene_test"
              (sources "kgui_scene_test.c")
              (private (root "core/include") (raw "${generated}")
                       (raw "../third_party"))
              (link "script"))
  (test "kgui_scene" "kgui_scene_test")

  (executable "kgui_widgets_test"
              (sources "kgui_widgets_test.c")
              (private (root "core/include") (raw "${generated}")
                       (raw "../third_party"))
              (link "script"))
  (test "kgui_widgets" "kgui_widgets_test")

  (executable "kgui_assets_test"
              (sources "kgui_assets_test.c")
              (private (root "core/include") (raw "${generated}")
                       (raw "../third_party"))
              (link "script"))
  (test "kgui_assets" "kgui_assets_test")

  ;;! The (krudd-stats) accessor, split out of the wasm-only kruddgui.cpp so
  ;;! the perf HUD's one engine input can be linked — and therefore tested —
  ;;! natively. A stub is what let the real binding vanish for a release
  ;;! without a test noticing (#911); see kgui_stats.h.
  (library "kgui_stats"
    (sources "kgui_stats.c")
    (public ".")
    (private (root "abi") (raw "../third_party")))

  ;;! Links kgui_stats so the HUD is driven by the same binding the browser
  ;;! calls, steered through a test-owned struct stats_api rather than a
  ;;! lookalike s7 stub.
  (executable "kgui_perf_test"
              (sources "kgui_perf_test.c")
              (private (root "core/include") (root "abi")
                       (raw "${generated}") (raw "../third_party"))
              (link "script" "kgui_stats"))
  (test "kgui_perf" "kgui_perf_test")

  (executable "kgui_mode_test"
              (sources "kgui_mode_test.c")
              (private (root "core/include") (raw "${generated}")
                       (raw "../third_party"))
              (link "script" "m"))
  (test "kgui_mode" "kgui_mode_test")))
