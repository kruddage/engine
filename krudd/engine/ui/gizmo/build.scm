; SPDX-License-Identifier: GPL-2.0-or-later
;;! The transform gizmo (#949): the handles the editor drags a selection by,
;;! plus the ground grid and the axis indicator, drawn on kruddgui's overlay
;;! batch. #944's Q4 put the editor's gizmos in the canvas and named
;;! kruddgui_api's overlay seam as the place they go; ui/viewport was its only
;;! tenant until this.
;;!
;;! Split the way viewport/ and bridge/ are, and for the same payoff: gizmo.c is
;;! pure arithmetic — projection, hit-testing and the three drag solves, with no
;;! world, no camera and no pointer in it — so the native test drives the whole
;;! of the maths with a compiler and nothing else. The issue asks for exactly
;;! that, and it is right to: a wrong gizmo still looks like a gizmo, so testing
;;! it through the UI is testing it by eye.
;;!
;;! gizmo_plugin.c is the half that cannot be tested that way — the subsystem,
;;! the vtable the bridge drives, and the drawing. It builds on both targets so
;;! the bridge's contract does not change shape between them; only the drawing
;;! is fenced under __EMSCRIPTEN__.
((library "gizmo"
   (sources "gizmo.c" "gizmo_plugin.c")
   (private "include" (root "abi") (root "world/entity/include")
            (root "base/math/include") (root "core/include"))
   (link "subsystem" "subsystem_manager" "log" "m"))

 (native-only
  ;;! The arithmetic, against a synthetic camera: a hand-built view·projection,
  ;;! a pivot, and a pointer position in, a transform out. No GPU, no world, no
  ;;! subsystem manager. base/math comes in compiled straight, the way
  ;;! viewport_pick_test takes it, because the projection this asserts on has to
  ;;! be the one the renderer draws with rather than a second copy.
  (executable "gizmo_test"
              (sources "gizmo_test.c" "gizmo.c"
                       (root "base/math/math.c") (root "base/math/camera.c")
                       (raw "${generated}/math_gen.c"))
              (private "include" (root "abi") (root "base/math/include")
                       (root "core/include")
                       (raw "${generated}"))
              (link "m"))
  (test "gizmo" "gizmo_test")))
