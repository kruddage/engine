; SPDX-License-Identifier: GPL-2.0-or-later

;;! kruddboard — Scheme-authored panels for the engine debug board.
;;!
;;! The C++ host (kruddboard.cpp) registers the imgui-* drawing primitives and
;;! the krudd-stats accessor against the shared s7 interpreter, loads this image
;;! once, then calls the panel procedures below from inside the ImGui frame. A
;;! primitive only records a draw call — it must run while a frame is open — so
;;! the host invokes these procedures at draw time, never at load time.
;;!
;;! This is the first panel to move out of kruddboard.cpp; the remaining tabs
;;! follow the same shape (see the "strangle kruddboard into Scheme" epic). The
;;! C draw_tab_stats this replaces read three fields off the stats subsystem and
;;! laid them out with ImGui::Text; the labels and spacing are preserved here.

;;! (krudd-stats) hands back #f when the stats subsystem is absent, otherwise the
;;! list (fps frame-ms frame-count). The #f branch mirrors the old C null check.
(define (kruddboard-draw-stats)
  (let ((s (krudd-stats)))
    (if (not s)
	(imgui-text-disabled "(stats unavailable)")
	(begin
	  (imgui-text (format #f "FPS (avg): ~,1F" (car s)))
	  (imgui-text (format #f "Frame ms:  ~,2F" (cadr s)))
	  (imgui-text (format #f "Frame:     ~D"   (caddr s)))))))

;;! (kruddboard-draw-subsystem-row r) draws one subsystem: its name, then
;;! yes/- for whether it exposes an API and a tick, then the WASM size — or a
;;! dimmed "-" when the size is zero/unknown, as the old C did with
;;! TextDisabled. r is a (name api? tick? wasm-size) list.
(define (kruddboard-draw-subsystem-row r)
  (let ((name     (car r))
	(has-api  (cadr r))
	(has-tick (caddr r))
	(size     (cadddr r)))
    (imgui-table-next-row)
    (imgui-table-next-column)
    (imgui-text name)
    (imgui-table-next-column)
    (imgui-text (if has-api "yes" "-"))
    (imgui-table-next-column)
    (imgui-text (if has-tick "yes" "-"))
    (imgui-table-next-column)
    (if (> size 0)
	(imgui-text (number->string size))
	(imgui-text-disabled "-"))))

;;! (kruddboard-draw-subsystems) renders the subsystem manager's entries as the
;;! four-column Name / API / Tick / WASM Size table the C draw_tab_subsystems
;;! drew. (krudd-subsystems) returns one (name api? tick? wasm-size) row per
;;! subsystem in table order (static then dynamic), or #f when the manager is
;;! absent; the #f branch mirrors the old null check.
(define (kruddboard-draw-subsystems)
  (let ((rows (krudd-subsystems)))
    (if (not rows)
	(imgui-text-disabled "(subsystem manager unavailable)")
	(when (imgui-begin-table "##subsys" 4)
	  (imgui-table-setup-column "Name")
	  (imgui-table-setup-column "API")
	  (imgui-table-setup-column "Tick")
	  (imgui-table-setup-column "WASM Size")
	  (imgui-table-headers-row)
	  (for-each kruddboard-draw-subsystem-row rows)
	  (imgui-end-table)))))

;;! Log view state persists across frames the way the old C statics did: the
;;! active level filter (a log_level integer, DEBUG=0) and whether the view
;;! auto-scrolls to the newest line.
(define kruddboard-log-filter 0)
(define kruddboard-log-autoscroll #t)

;;! Per-level text colours indexed by log_level: DEBUG grey, INFO white, WARN
;;! yellow, ERROR red — the same RGBA the old level_colors table carried.
(define kruddboard-log-colors
  (vector (list 0.6 0.6 0.6 1.0)
	  (list 1.0 1.0 1.0 1.0)
	  (list 1.0 0.8 0.2 1.0)
	  (list 1.0 0.3 0.3 1.0)))

;;! (kruddboard-draw-log-line m) draws one history entry: skip it when its
;;! level is below the active filter, otherwise draw the text in the level's
;;! colour. m is a (level . text) pair.
(define (kruddboard-draw-log-line m)
  (let ((level (car m))
	(text  (cdr m)))
    (when (>= level kruddboard-log-filter)
      (let ((c (vector-ref kruddboard-log-colors level)))
	(imgui-text-colored (car c) (cadr c) (caddr c) (cadddr c) text)))))

;;! (kruddboard-draw-log) draws the level-filter buttons, the auto-scroll
;;! toggle, and the scrolling history child — the C draw_tab_log ported whole.
;;! (krudd-log-history) hands back a list of (level . text) pairs oldest-first,
;;! or #f when the log subsystem is absent; the #f branch mirrors the old null
;;! check. The scroll region caps at ~88% of the viewport height minus the
;;! controls, floored at 80px, as the C computed it.
(define (kruddboard-draw-log)
  (let ((hist (krudd-log-history)))
    (if (not hist)
	(imgui-text-disabled "(log unavailable)")
	(begin
	  (when (imgui-small-button "DEBUG") (set! kruddboard-log-filter 0))
	  (imgui-same-line)
	  (when (imgui-small-button "INFO")  (set! kruddboard-log-filter 1))
	  (imgui-same-line)
	  (when (imgui-small-button "WARN")  (set! kruddboard-log-filter 2))
	  (imgui-same-line)
	  (when (imgui-small-button "ERROR") (set! kruddboard-log-filter 3))
	  (imgui-same-line)
	  (set! kruddboard-log-autoscroll
		(imgui-checkbox "Auto-scroll" kruddboard-log-autoscroll))
	  (imgui-separator)
	  (let ((scroll-h (max 80.0
			       (- (* (imgui-viewport-work-height) 0.88)
				  120.0))))
	    (imgui-begin-child "##logscroll" 0.0 scroll-h)
	    (for-each kruddboard-draw-log-line hist)
	    (when kruddboard-log-autoscroll (imgui-set-scroll-here-y 1.0))
	    (imgui-end-child))))))

;;! (kruddboard-draw-krudd) is the whole KRUDD tab: frame stats, subsystems,
;;! and log, each under its own default-open collapsing header — the Scheme
;;! composition that replaces the C draw_tab_krudd.
(define (kruddboard-draw-krudd)
  (when (imgui-collapsing-header "Frame Stats") (kruddboard-draw-stats))
  (when (imgui-collapsing-header "Subsystems")  (kruddboard-draw-subsystems))
  (when (imgui-collapsing-header "Log")         (kruddboard-draw-log)))
