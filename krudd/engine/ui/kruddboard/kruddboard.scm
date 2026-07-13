; SPDX-License-Identifier: GPL-2.0-or-later

;;! kruddboard — Scheme-authored panels for the engine debug board.
;;!
;;! The C++ host (kruddboard.cpp) registers the imgui-* drawing primitives and
;;! the shared krudd-* accessors against the s7 interpreter, loads this image
;;! once, then calls the panel procedures from inside the ImGui frame. A
;;! primitive only records a draw call — it must run while a frame is open — so
;;! the host invokes these procedures at draw time, never at load time.
;;!
;;! This image is being strangled into kruddgui one tab at a time (see the
;;! "strangle kruddboard into Scheme" epic, #492):
;;!
;;!   - the KRUDD tab (frame stats, startup profile, subsystem table) and the
;;!     Log console were lifted into kruddgui's own quads (#491, #492); their
;;!     ImGui draw paths (kruddboard-draw-stats / -startup / -perf / -subsystems
;;!     / -krudd / -log here, and draw_tab_krudd / -stats / -subsystems in
;;!     kruddboard.cpp) are gone; and
;;!
;;!   - the World (Scene) tab — the entity list and the mutating inspector — was
;;!     lifted onto kruddgui's interactive widgets (kruddgui.scm's Scene console,
;;!     #492). Its ImGui draw path (kruddboard-draw-world & friends here, and
;;!     draw_tab_world in kruddboard.cpp) is gone; every entity edit now routes
;;!     through the same undo-recording krudd-entity-* accessors from there.
;;!
;;! The Assets tab is the last panel still drawn by ImGui through this image; it
;;! lives in tabs/Assets.scm (a separate image the host loads alongside this one).
;;! When it falls too, ImGui can be removed.
