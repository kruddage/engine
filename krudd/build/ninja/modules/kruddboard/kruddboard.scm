; SPDX-License-Identifier: GPL-2.0-or-later
;
; kruddboard — Scheme-authored panels for the engine debug board.
;
; The C++ host (kruddboard.cpp) registers the imgui-* drawing primitives and
; the krudd-stats accessor against the shared s7 interpreter, loads this image
; once, then calls the panel procedures below from inside the ImGui frame. A
; primitive only records a draw call — it must run while a frame is open — so
; the host invokes these procedures at draw time, never at load time.
;
; This is the first panel to move out of kruddboard.cpp; the remaining tabs
; follow the same shape (see the "strangle kruddboard into Scheme" epic). The
; C draw_tab_stats this replaces read three fields off the stats subsystem and
; laid them out with ImGui::Text; the labels and spacing are preserved here.

; (krudd-stats) hands back #f when the stats subsystem is absent, otherwise the
; list (fps frame-ms frame-count). The #f branch mirrors the old C null check.
(define (kruddboard-draw-stats)
  (let ((s (krudd-stats)))
    (if (not s)
	(imgui-text-disabled "(stats unavailable)")
	(begin
	  (imgui-text (format #f "FPS (avg): ~,1F" (car s)))
	  (imgui-text (format #f "Frame ms:  ~,2F" (cadr s)))
	  (imgui-text (format #f "Frame:     ~D"   (caddr s)))))))
