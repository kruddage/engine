; SPDX-License-Identifier: GPL-2.0-or-later

;;! kruddgui — the Scheme-authored panels for krudd's own UI layer.
;;!
;;! The C++ host (kruddgui.cpp) registers the kgui-* primitives against the
;;! shared s7 interpreter, loads this image once, then calls (kruddgui-draw)
;;! each tick after ImGui has rendered. A primitive only appends to the frame's
;;! quad batch or queries the pointer router for the panel it is drawing, so
;;! these procedures must run at draw time — never at load time.
;;!
;;! Independent panels, each owning its own input region (#489, #491, #492):
;;!
;;!   - the MOVE / ROTATE / SCALE mode-bar (#490), wired to the shared gizmo
;;!     tool via (krudd-gizmo-mode) / (krudd-set-gizmo-mode);
;;!
;;!   - the Log console — the first kruddboard tab lifted out of ImGui. It is
;;!     drawn entirely with kruddgui's own quads and font-atlas text over the
;;!     engine log (krudd-log-history), with tap-to-filter level chips and a
;;!     drag/wheel-scrolled, scissor-clipped body. Its ImGui draw path is gone; and
;;!
;;!   - the board console — the KRUDD tab (frame stats, startup profile, and the
;;!     subsystem table) lifted out of ImGui (#492), a collapsible read view over
;;!     the same krudd-stats / krudd-startup / krudd-subsystems accessors. Its
;;!     ImGui draw path (draw_tab_krudd and the Scene tab's Perf roll-up) is gone.
;;!
;;! Each panel is bracketed by (kgui-panel-begin name x y w h) / (kgui-panel-end):
;;! the named region captures any gesture whose down lands inside it, so taps
;;! route to the right panel and a finger on one never disturbs the other or the
;;! ImGui editor underneath. Panels are declared in draw order; a later one wins
;;! the overlap. Everything the router does not claim is forwarded to ImGui.

;;! Touch-target and spacing constants. The button's short side is 56px, safely
;;! over the 44px minimum finger target; the rest key off the viewport.
(define kruddgui-margin 16)
(define kruddgui-gap 10)
(define kruddgui-btn 56)
(define kruddgui-btn-min-w 96)
(define kruddgui-col-w 128)

;;! The three tools, in gizmo-mode order (0 move, 1 rotate, 2 scale).
(define kruddgui-modes '((0 . "MOVE") (1 . "ROTATE") (2 . "SCALE")))

;;! Palette. The active tool reads as a bright accent with dark text; the others
;;! are dark chips with light text, over a translucent backing panel.
(define kruddgui-active-bg  '(0.95 0.55 0.15 0.95))
(define kruddgui-active-fg  '(0.10 0.08 0.05 1.0))
(define kruddgui-idle-bg    '(0.17 0.18 0.21 0.92))
(define kruddgui-idle-fg    '(0.86 0.88 0.92 1.0))
(define kruddgui-panel-bg   '(0.05 0.05 0.07 0.55))

;;! (kruddgui-rect* r c) draw a filled rect r=(x y w h) in colour c=(r g b a).
(define (kruddgui-rect* r c)
  (kgui-rect (car r) (cadr r) (caddr r) (cadddr r)
	     (car c) (cadr c) (caddr c) (cadddr c)))

;;! (kruddgui-label x y w h label c) draw LABEL centred in the (x y w h) box in
;;! colour c. Centring uses (kgui-text-metrics), which reports the run's pixel
;;! width and height at the current font size.
(define (kruddgui-label x y w h label c)
  (let* ((m  (kgui-text-metrics label))
	 (tw (car m))
	 (th (cadr m))
	 (tx (+ x (/ (- w tw) 2)))
	 (ty (+ y (/ (- h th) 2))))
    (kgui-text tx ty label (car c) (cadr c) (caddr c) (cadddr c))))

;;! (kruddgui-button x y w h idx label) draw one mode chip and, on tap, switch
;;! the shared gizmo tool to IDX. The active tool is highlighted. (kgui-button)
;;! reports a tap trapped by the enclosing mode-bar region and consumes it, so
;;! a single tap fires exactly one chip.
(define (kruddgui-button x y w h idx label)
  (let ((active (= (krudd-gizmo-mode) idx)))
    (kruddgui-rect* (list x y w h)
		    (if active kruddgui-active-bg kruddgui-idle-bg))
    (kruddgui-label x y w h label
		    (if active kruddgui-active-fg kruddgui-idle-fg))
    (when (kgui-button x y w h)
      (krudd-set-gizmo-mode idx))))

;;! (kruddgui-modebar-frame x y w h) the mode-bar's backing rect: the chip
;;! footprint (x y w h) inset outward by one gap on every side. It is both the
;;! translucent panel drawn behind the chips and the input region declared for
;;! the bar, so a down anywhere on the bar (chips or the gaps between) is
;;! captured by kruddgui rather than leaking to ImGui.
(define (kruddgui-modebar-frame x y w h)
  (let ((p kruddgui-gap))
    (list (- x p) (- y p) (+ w (* 2 p)) (+ h (* 2 p)))))

;;! Landscape (wide) layout: a horizontal row of three chips centred along the
;;! bottom. Chip width grows to fill but is clamped so the row always fits.
(define (kruddgui-draw-row vw vh)
  (let* ((n    (length kruddgui-modes))
	 (m    kruddgui-margin)
	 (g    kruddgui-gap)
	 (h    kruddgui-btn)
	 (maxw (/ (- vw (* 2 m) (* (- n 1) g)) n))
	 (w    (max kruddgui-btn-min-w (min 140 maxw)))
	 (tot  (+ (* n w) (* (- n 1) g)))
	 (x0   (/ (- vw tot) 2))
	 (y    (- vh m h))
	 (fr   (kruddgui-modebar-frame x0 y tot h)))
    (kgui-panel-begin "kgui-modebar" (car fr) (cadr fr) (caddr fr) (cadddr fr))
    (kruddgui-rect* fr kruddgui-panel-bg)
    (let loop ((ms kruddgui-modes) (i 0))
      (when (pair? ms)
	(let ((x (+ x0 (* i (+ w g)))))
	  (kruddgui-button x y w h (caar ms) (cdar ms)))
	(loop (cdr ms) (+ i 1))))
    (kgui-panel-end)))

;;! Portrait (tall) layout: a vertical column of three chips anchored bottom-
;;! right, within thumb reach. Column width is clamped to the viewport.
(define (kruddgui-draw-col vw vh)
  (let* ((n   (length kruddgui-modes))
	 (m   kruddgui-margin)
	 (g   kruddgui-gap)
	 (h   kruddgui-btn)
	 (w   (min kruddgui-col-w (- vw (* 2 m))))
	 (x   (- vw m w))
	 (tot (+ (* n h) (* (- n 1) g)))
	 (y0  (- vh m tot))
	 (fr  (kruddgui-modebar-frame x y0 w tot)))
    (kgui-panel-begin "kgui-modebar" (car fr) (cadr fr) (caddr fr) (cadddr fr))
    (kruddgui-rect* fr kruddgui-panel-bg)
    (let loop ((ms kruddgui-modes) (i 0))
      (when (pair? ms)
	(let ((y (+ y0 (* i (+ h g)))))
	  (kruddgui-button x y w h (caar ms) (cdar ms)))
	(loop (cdr ms) (+ i 1))))
    (kgui-panel-end)))

;;! ------------------------------------------------------------------
;;! Log console — the lifted kruddboard tab (#491)
;;! ------------------------------------------------------------------

;;! Console geometry. The panel anchors to the top-right so it never overlaps
;;! the bottom mode-bar; the header carries the title, level-filter chips and a
;;! close box, and the body is a scissor-clipped scroll of coloured log lines.
(define kruddgui-log-margin 12)
(define kruddgui-log-line-pad 4)
(define kruddgui-log-header-h 36)
(define kruddgui-log-chip-h 24)
(define kruddgui-log-chip-w 40)
(define kruddgui-log-handle-w 76)
(define kruddgui-log-handle-h 30)
(define kruddgui-log-max-w 360)

(define kruddgui-log-panel-bg '(0.08 0.09 0.11 0.95))
(define kruddgui-log-body-bg  '(0.04 0.05 0.06 0.96))

;;! Per-level line colours indexed by log_level (DEBUG INFO WARN ERROR).
(define kruddgui-log-colors
  (vector (list 0.60 0.62 0.66 1.0)
	  (list 0.86 0.88 0.92 1.0)
	  (list 0.98 0.80 0.30 1.0)
	  (list 1.00 0.42 0.42 1.0)))

;;! The level-filter chips: label and the minimum log_level each admits.
(define kruddgui-log-chips '(("ALL" . 0) ("INF" . 1) ("WRN" . 2) ("ERR" . 3)))

;;! Console state, held across frames the way the old C statics were: whether
;;! the console is expanded, the active minimum level, and the scroll distance
;;! up from the newest line (0 keeps it pinned to the bottom, so a fresh line
;;! auto-scrolls into view). The scroll value is clamped against the content
;;! each frame in kruddgui-log-draw-body.
(define kruddgui-log-open #f)
(define kruddgui-log-filter 0)
(define kruddgui-log-scroll 0.0)

;;! One text line's height at the current font size, plus a little leading.
(define (kruddgui-log-line-height)
  (+ kruddgui-log-line-pad (cadr (kgui-text-metrics "Ag"))))

;;! (kruddgui-log-filter-rows hist) keeps only the (level . text) rows at or
;;! above the active filter, preserving oldest-first order.
(define (kruddgui-log-filter-rows hist)
  (let loop ((h hist) (acc '()))
    (if (null? h)
	(reverse acc)
	(loop (cdr h)
	      (if (>= (caar h) kruddgui-log-filter)
		  (cons (car h) acc)
		  acc)))))

;;! (kruddgui-log-draw-header x y w hdr) draws the title, the four level chips
;;! and the close box across the header, wiring each chip to the filter and the
;;! close box to collapse the console. Chip and close taps are trapped by the
;;! console region, so they never reach ImGui.
(define (kruddgui-log-draw-header x y w hdr)
  (let* ((cw   kruddgui-log-chip-w)
	 (ch   kruddgui-log-chip-h)
	 (cy   (+ y (/ (- hdr ch) 2)))
	 (cx0  (+ x 52))
	 (bs   26)
	 (bx   (- (+ x w) 8 bs))
	 (by   (+ y (/ (- hdr bs) 2))))
    (kruddgui-label x y 52 hdr "LOG" kruddgui-idle-fg)
    (kruddgui-rect* (list bx by bs bs) kruddgui-idle-bg)
    (kruddgui-label bx by bs bs "x" kruddgui-idle-fg)
    (when (kgui-button bx by bs bs)
      (set! kruddgui-log-open #f))
    (let loop ((cs kruddgui-log-chips) (i 0))
      (when (pair? cs)
	(let* ((lbl    (caar cs))
	       (lvl    (cdar cs))
	       (cx     (+ cx0 (* i (+ cw 4))))
	       (active (= kruddgui-log-filter lvl)))
	  (kruddgui-rect* (list cx cy cw ch)
			  (if active kruddgui-active-bg kruddgui-idle-bg))
	  (kruddgui-label cx cy cw ch lbl
			  (if active kruddgui-active-fg kruddgui-idle-fg))
	  (when (kgui-button cx cy cw ch)
	    (set! kruddgui-log-filter lvl)))
	(loop (cdr cs) (+ i 1))))))

;;! (kruddgui-log-draw-body x y w h rows line-h) draws the scrolling list. The
;;! content is anchored to the bottom: with kruddgui-log-scroll 0 the newest
;;! line sits at the body's bottom edge, and dragging (or the wheel) walks up
;;! into history. The scroll is clamped to the content here and written back.
;;! Only the rows overlapping the viewport are emitted, and kgui-clip scissors
;;! the body so a partial row at either edge is cut cleanly.
(define (kruddgui-log-draw-body x y w h rows line-h)
  (let* ((n          (length rows))
	 (total      (* n line-h))
	 (max-scr    (max 0.0 (- total h)))
	 (scr        (min max-scr (max 0.0 kruddgui-log-scroll)))
	 (scroll-top (- max-scr scr)))
    (set! kruddgui-log-scroll scr)
    (kruddgui-rect* (list x y w h) kruddgui-log-body-bg)
    (kgui-clip x y w h)
    (let loop ((rs rows) (i 0))
      (when (pair? rs)
	(let ((ry (+ y (- (* i line-h) scroll-top))))
	  (cond
	   ;;! below the body: stop
	   ((>= ry (+ y h)) #t)
	   ;;! above the body: skip
	   ((<= (+ ry line-h) y)
	    (loop (cdr rs) (+ i 1)))
	   (else
	    (let* ((row (car rs))
		   (lvl (car row))
		   (txt (cdr row))
		   (c   (vector-ref kruddgui-log-colors lvl)))
	      (kgui-text (+ x 6) ry txt
			 (car c) (cadr c) (caddr c) (cadddr c)))
	    (loop (cdr rs) (+ i 1)))))))
    (kgui-clip-none)))

;;! (kruddgui-modebar-reserve vw vh) the height the mode-bar occupies at the
;;! bottom of the viewport, so the Log console can size itself to clear it: the
;;! tall three-chip column in portrait, the single-chip row in landscape.
(define (kruddgui-modebar-reserve vw vh)
  (let ((n (length kruddgui-modes)))
    (if (>= vw vh)
	(+ kruddgui-margin kruddgui-btn kruddgui-gap)
	(+ kruddgui-margin
	   (+ (* n kruddgui-btn) (* (- n 1) kruddgui-gap))
	   kruddgui-gap))))

;;! (kruddgui-log-draw-panel vw vh) the expanded console. It anchors top-left
;;! and stops short of the mode-bar's reserved band at the bottom, so the two
;;! kruddgui panels never overlap; while open it does cover the ImGui board
;;! beneath (a deliberate, dismissable read view), and its region traps every
;;! tap so nothing leaks to the editor under it. The body is fed by
;;! (krudd-log-history) — (level . text) pairs oldest-first, or #f when the log
;;! subsystem is absent (the #f branch mirrors the old C null check). Drag and
;;! wheel accumulated on the region this frame move the scroll before redraw.
(define (kruddgui-log-draw-panel vw vh)
  (let* ((m      kruddgui-log-margin)
	 (avail  (- vh m (kruddgui-modebar-reserve vw vh) kruddgui-gap))
	 (w      (min kruddgui-log-max-w (- vw (* 2 m))))
	 (h      (max 120.0 (min (* vh 0.5) avail)))
	 (x      m)
	 (y      m)
	 (hdr    kruddgui-log-header-h)
	 (body-y (+ y hdr))
	 (body-h (- h hdr)))
    (kgui-panel-begin "kgui-log" x y w h)
    (kruddgui-rect* (list x y w h) kruddgui-log-panel-bg)
    (kruddgui-log-draw-header x y w hdr)
    (let ((hist (krudd-log-history)))
      (if (not hist)
	  (kruddgui-label x body-y w body-h "(log unavailable)"
			  kruddgui-idle-fg)
	  (begin
	    (set! kruddgui-log-scroll
		  (+ kruddgui-log-scroll
		     (cadr (kgui-region-drag))
		     (- (kgui-region-wheel))))
	    (kruddgui-log-draw-body x body-y w body-h
				    (kruddgui-log-filter-rows hist)
				    (kruddgui-log-line-height)))))
    (kgui-panel-end)))

;;! (kruddgui-log-draw-handle vw vh) the collapsed console: a small pill in the
;;! bottom-left corner — a zone clear of both the ImGui board's header controls
;;! (so the editor's Show-KB toggle is never covered) and the mode-bar at the
;;! opposite bottom corner — that expands the console on tap, its own captured
;;! input region.
(define (kruddgui-log-draw-handle vw vh)
  (let* ((m  kruddgui-log-margin)
	 (hw kruddgui-log-handle-w)
	 (hh kruddgui-log-handle-h)
	 (x  m)
	 (y  (- vh m hh)))
    (kgui-panel-begin "kgui-log" x y hw hh)
    (kruddgui-rect* (list x y hw hh) kruddgui-log-panel-bg)
    (kruddgui-label x y hw hh "LOG" kruddgui-idle-fg)
    (when (kgui-button x y hw hh)
      (set! kruddgui-log-open #t))
    (kgui-panel-end)))

;;! ------------------------------------------------------------------
;;! Board panel — the lifted KRUDD tab (#492): frame stats, startup, subsystems
;;! ------------------------------------------------------------------

;;! The KRUDD tab's three read-only sections — live frame stats, the one-time
;;! startup profile, and the subsystem manager table — re-authored as a single
;;! kruddgui console. It reads the same shared accessors the ImGui tab did
;;! (krudd-stats / krudd-startup / krudd-subsystems, registered by kruddboard on
;;! the shared s7), draws them with kruddgui's own quads and font-atlas text, and
;;! owns its own input region — so its ImGui draw path (draw_tab_krudd and the
;;! Scene tab's Perf roll-up) is gone. The panel anchors top-right so it never
;;! overlaps the top-left Log console; both are dismissable read views over the
;;! editor. Geometry mirrors the Log console so the two consoles read as a set.
(define kruddgui-board-max-w 380)
(define kruddgui-board-header-h 36)
(define kruddgui-board-row-pad 6)
(define kruddgui-board-col-pad 8)

(define kruddgui-board-panel-bg '(0.08 0.09 0.11 0.95))
(define kruddgui-board-body-bg  '(0.04 0.05 0.06 0.96))
(define kruddgui-board-head-fg  '(0.62 0.80 0.98 1.0))
(define kruddgui-board-rule     '(0.24 0.26 0.30 1.0))

;;! Console state held across frames: whether it is expanded, and the content
;;! scroll offset. The offset is <= 0 — the pixels the list is shifted up, so 0
;;! pins the first row to the body's top and a drag/wheel walks down into the
;;! rest. It is clamped against the content each frame in kruddgui-board-draw-body.
(define kruddgui-board-open #f)
(define kruddgui-board-scroll 0.0)

;;! One list row's height at the current font size, plus a little leading.
(define (kruddgui-board-line-height)
  (+ kruddgui-board-row-pad (cadr (kgui-text-metrics "Ag"))))

;;! (kruddgui-board-cell x y lh str c) draws STR left-aligned at x, vertically
;;! centred in a row of height LH, in colour c.
(define (kruddgui-board-cell x y lh str c)
  (let* ((m  (kgui-text-metrics str))
	 (th (cadr m))
	 (ty (+ y (/ (- lh th) 2))))
    (kgui-text x ty str (car c) (cadr c) (caddr c) (cadddr c))))

;;! Row descriptors are little tagged lists so the layout is data, not control
;;! flow: (head TITLE) a section header with an underline rule; (dim TEXT) a
;;! greyed "unavailable" line; (kv LABEL VALUE) a two-column label/value line;
;;! (cols A B C) a three-column table row. kruddgui-board-draw-row renders one at
;;! the given (rx ry rw lh).
(define (kruddgui-board-draw-row row rx ry rw lh)
  (let ((kind (car row))
	(x0   (+ rx kruddgui-board-col-pad)))
    (cond
     ((eq? kind 'head)
      (kruddgui-board-cell x0 ry lh (cadr row) kruddgui-board-head-fg)
      (kruddgui-rect* (list rx (+ ry lh -2) rw 1) kruddgui-board-rule))
     ((eq? kind 'dim)
      (kruddgui-board-cell x0 ry lh (cadr row) kruddgui-idle-fg))
     ((eq? kind 'kv)
      (kruddgui-board-cell x0 ry lh (cadr row) kruddgui-idle-fg)
      (kruddgui-board-cell (+ rx (* rw 0.46)) ry lh (caddr row)
			   kruddgui-idle-fg))
     ((eq? kind 'cols)
      (kruddgui-board-cell x0 ry lh (cadr row) kruddgui-idle-fg)
      (kruddgui-board-cell (+ rx (* rw 0.60)) ry lh (caddr row)
			   kruddgui-idle-fg)
      (kruddgui-board-cell (+ rx (* rw 0.80)) ry lh (cadddr row)
			   kruddgui-idle-fg)))))

;;! (kruddgui-board-stat-rows) the live frame stats, or a dimmed line when the
;;! stats subsystem is absent — the #f branch mirrors the old C null check.
;;! (krudd-stats) -> (fps frame-ms frame-count).
(define (kruddgui-board-stat-rows)
  (let ((s (krudd-stats)))
    (if (not s)
	(list (list 'dim "(stats unavailable)"))
	(list (list 'kv "FPS (avg)" (format #f "~,1F" (car s)))
	      (list 'kv "Frame ms"  (format #f "~,2F" (cadr s)))
	      (list 'kv "Frame"     (format #f "~D"   (caddr s)))))))

;;! (kruddgui-board-startup-rows) the one-time boot profile: the page-load wall
;;! clock, init total, time to first frame, then a per-phase breakdown.
;;! (krudd-startup) -> (init-ms first-frame-ms page-first-ms (name . ms) ...) or
;;! #f when stats are absent. "Page->1st frame" is the honest black-screen time
;;! (download + WASM compile included); "Init total" and "1st frame" are measured
;;! from engine_init entry and so miss everything before main().
(define (kruddgui-board-startup-rows)
  (let ((s (krudd-startup)))
    (if (not s)
	(list (list 'dim "(startup timings unavailable)"))
	(let ((init   (car s))
	      (first  (cadr s))
	      (page   (caddr s))
	      (phases (cdddr s)))
	  (append
	   (list (list 'kv "Page->1st frame" (format #f "~,1F ms" page))
		 (list 'kv "Init total"      (format #f "~,1F ms" init))
		 (list 'kv "1st frame"       (format #f "~,1F ms" first)))
	   (map (lambda (p)
		  (list 'kv (car p) (format #f "~,2F ms" (cdr p))))
		phases))))))

;;! (kruddgui-board-subsystem-rows) the subsystem manager's entries as Name / API
;;! / Tick rows under a header row, or a dimmed line when the manager is absent.
;;! (krudd-subsystems) -> ((name api? tick? size) ...) or #f; size is unused here.
(define (kruddgui-board-subsystem-rows)
  (let ((rows (krudd-subsystems)))
    (if (not rows)
	(list (list 'dim "(subsystem manager unavailable)"))
	(cons (list 'cols "Name" "API" "Tick")
	      (map (lambda (r)
		     (list 'cols (car r)
			   (if (cadr r) "yes" "-")
			   (if (caddr r) "yes" "-")))
		   rows)))))

;;! (kruddgui-board-rows) the whole panel body as one flat row list: the three
;;! sections, each under its own header row.
(define (kruddgui-board-rows)
  (append (list (list 'head "FRAME STATS")) (kruddgui-board-stat-rows)
	  (list (list 'head "STARTUP"))     (kruddgui-board-startup-rows)
	  (list (list 'head "SUBSYSTEMS"))  (kruddgui-board-subsystem-rows)))

;;! (kruddgui-board-draw-header x y w hdr) draws the title and the close box.
;;! The close tap is trapped by the console region, so it never reaches ImGui.
(define (kruddgui-board-draw-header x y w hdr)
  (let* ((bs 26)
	 (bx (- (+ x w) 8 bs))
	 (by (+ y (/ (- hdr bs) 2))))
    (kruddgui-label x y 96 hdr "ENGINE" kruddgui-idle-fg)
    (kruddgui-rect* (list bx by bs bs) kruddgui-idle-bg)
    (kruddgui-label bx by bs bs "x" kruddgui-idle-fg)
    (when (kgui-button bx by bs bs)
      (set! kruddgui-board-open #f))))

;;! (kruddgui-board-draw-body x y w h rows lh) draws the scrolling list, anchored
;;! to the top: at scroll 0 the first row sits at the body's top edge and a drag
;;! (or wheel) walks down into the rest. The offset is clamped to the content
;;! here and written back. Only rows overlapping the viewport are emitted, and
;;! kgui-clip scissors the body so a partial row at either edge is cut cleanly.
(define (kruddgui-board-draw-body x y w h rows lh)
  (let* ((n       (length rows))
	 (total   (* n lh))
	 (min-off (min 0.0 (- h total)))
	 (off     (max min-off (min 0.0 kruddgui-board-scroll))))
    (set! kruddgui-board-scroll off)
    (kruddgui-rect* (list x y w h) kruddgui-board-body-bg)
    (kgui-clip x y w h)
    (let loop ((rs rows) (i 0))
      (when (pair? rs)
	(let ((ry (+ y off (* i lh))))
	  (cond
	   ;;! below the body: stop
	   ((>= ry (+ y h)) #t)
	   ;;! above the body: skip
	   ((<= (+ ry lh) y)
	    (loop (cdr rs) (+ i 1)))
	   (else
	    (kruddgui-board-draw-row (car rs) x ry w lh)
	    (loop (cdr rs) (+ i 1)))))))
    (kgui-clip-none)))

;;! (kruddgui-board-draw-panel vw vh) the expanded console. It anchors top-right
;;! and stops short of the mode-bar's reserved band at the bottom, so it never
;;! overlaps the bottom mode-bar or the top-left Log console; while open it does
;;! cover the ImGui board's right-hand controls (Show KB) beneath — a deliberate,
;;! dismissable read view — and its region traps every tap so nothing leaks to
;;! the editor under it. A drag/wheel accumulated on the region this frame scrolls
;;! the body before it is redrawn (then re-clamped there).
(define (kruddgui-board-draw-panel vw vh)
  (let* ((m      kruddgui-log-margin)
	 (avail  (- vh m (kruddgui-modebar-reserve vw vh) kruddgui-gap))
	 (w      (min kruddgui-board-max-w (- vw (* 2 m))))
	 (h      (max 140.0 (min (* vh 0.6) avail)))
	 (x      (- vw m w))
	 (y      m)
	 (hdr    kruddgui-board-header-h)
	 (body-y (+ y hdr))
	 (body-h (- h hdr)))
    (kgui-panel-begin "kgui-board" x y w h)
    (kruddgui-rect* (list x y w h) kruddgui-board-panel-bg)
    (kruddgui-board-draw-header x y w hdr)
    (set! kruddgui-board-scroll
	  (+ kruddgui-board-scroll
	     (cadr (kgui-region-drag))
	     (kgui-region-wheel)))
    (kruddgui-board-draw-body x body-y w body-h
			      (kruddgui-board-rows)
			      (kruddgui-board-line-height))
    (kgui-panel-end)))

;;! (kruddgui-board-draw-handle vw vh) the collapsed console: a small pill in the
;;! bottom-left corner, stacked directly above the Log handle so the two consoles
;;! share a tidy corner clear of both the ImGui board header (top) and the mode-
;;! bar (bottom-right / centre). Tap expands the console; its own captured region.
(define (kruddgui-board-draw-handle vw vh)
  (let* ((m  kruddgui-log-margin)
	 (hw kruddgui-log-handle-w)
	 (hh kruddgui-log-handle-h)
	 (x  m)
	 (y  (- vh m hh hh kruddgui-gap)))
    (kgui-panel-begin "kgui-board" x y hw hh)
    (kruddgui-rect* (list x y hw hh) kruddgui-board-panel-bg)
    (kruddgui-label x y hw hh "STATS" kruddgui-idle-fg)
    (when (kgui-button x y hw hh)
      (set! kruddgui-board-open #t))
    (kgui-panel-end)))

;;! ------------------------------------------------------------------
;;! Scene inspector — the World tab lifted onto kruddgui's own widgets (#492)
;;! ------------------------------------------------------------------

;;! The World (Scene) tab — the entity list and the drill-in inspector — was the
;;! ImGui board's first MUTATING surface. It is re-authored here on kruddgui's
;;! own interactive widgets: the soft-keyboard text field (kgui-field), numeric
;;! fields built on it, and tap-driven combos. Every edit still routes through
;;! the shared undo-recording krudd-entity-* accessors kruddboard registers, so
;;! rename / transform / rebind record undo steps exactly as the ImGui path did;
;;! its ImGui draw path (draw_tab_world + kruddboard-draw-world & friends) is
;;! gone. Like the Log and board consoles this is a dismissable overlay with its
;;! own input region and a bottom-left handle, stacked above the STATS handle.

(define kruddgui-scene-max-w 400)
(define kruddgui-scene-header-h 36)
(define kruddgui-scene-pad 10)
;;! Interactive row height (>= the 40px minimum finger target); label / read-
;;! only lines are shorter.
(define kruddgui-scene-row-h 40)
(define kruddgui-scene-line 26)
(define kruddgui-scene-gap 6)

(define kruddgui-scene-panel-bg  '(0.08 0.09 0.11 0.97))
(define kruddgui-scene-body-bg   '(0.05 0.06 0.08 0.98))
(define kruddgui-scene-field-bg  '(0.14 0.15 0.18 1.0))
(define kruddgui-scene-field-act '(0.20 0.25 0.32 1.0))
(define kruddgui-scene-caret-fg  '(0.95 0.96 0.99 1.0))
(define kruddgui-scene-label-fg  '(0.62 0.80 0.98 1.0))
(define kruddgui-scene-head-fg   '(0.62 0.80 0.98 1.0))
(define kruddgui-scene-rule-fg   '(0.24 0.26 0.30 1.0))

;;! Overlay state, held across frames the way the ImGui statics were: whether the
;;! console is expanded, which entity's inspector is drilled into (-1 = the entity
;;! list, matching the engine's "no selection" sentinel since entity id 0 is
;;! live), the body scroll offset (<= 0), the id of the one open combo (or #f),
;;! and the content height measured last frame (to clamp the scroll).
(define kruddgui-scene-open #f)
(define kruddgui-scene-sel -1)
(define kruddgui-scene-scroll 0.0)
(define kruddgui-scene-open-combo #f)
(define kruddgui-scene-total 0.0)

;;! A layout cursor: a mutable running y plus the body's clip band and the row
;;! origin/width, so each widget places itself, culls when off the body, and
;;! advances the cursor. Data, not control flow — the immediate-mode twin of the
;;! board console's row list, but the rows here are interactive.
(define (kruddgui-lay cy y0 y1 x w) (vector cy y0 y1 x w))
(define (kruddgui-lay-cy L) (vector-ref L 0))
(define (kruddgui-lay-y0 L) (vector-ref L 1))
(define (kruddgui-lay-y1 L) (vector-ref L 2))
(define (kruddgui-lay-x  L) (vector-ref L 3))
(define (kruddgui-lay-w  L) (vector-ref L 4))
(define (kruddgui-lay-adv! L dh) (vector-set! L 0 (+ (vector-ref L 0) dh)))
;;! Retarget the cursor's row origin/width — for a nested subtree that indents
;;! its rows (folder depth) while keeping the running y and clip band. The
;;! browser's folder tree narrows x/w around each level and restores them after.
(define (kruddgui-lay-set-x! L x) (vector-set! L 3 x))
(define (kruddgui-lay-set-w! L w) (vector-set! L 4 w))

;;! A row at the cursor is visible when it overlaps the body's clip band; an
;;! off-body row is culled (no draw, no input — it cannot be tapped anyway).
(define (kruddgui-lay-vis? L h)
  (let ((cy (kruddgui-lay-cy L)))
    (and (> (+ cy h) (kruddgui-lay-y0 L)) (< cy (kruddgui-lay-y1 L)))))

;;! (kruddgui-scene-field-draw x y w h disp active caret) paints one field cell:
;;! a filled box (brighter while focused), the text, and — while focused — a caret
;;! bar at the reported pixel offset. kgui-field owns the editing; this only draws.
(define (kruddgui-scene-field-draw x y w h disp active caret)
  (let* ((m  (kgui-text-metrics disp))
	 (th (cadr m))
	 (ty (+ y (/ (- h th) 2))))
    (kruddgui-rect* (list x y w h)
		    (if active kruddgui-scene-field-act kruddgui-scene-field-bg))
    (kgui-text (+ x 6) ty disp
	       (car kruddgui-idle-fg) (cadr kruddgui-idle-fg)
	       (caddr kruddgui-idle-fg) (cadddr kruddgui-idle-fg))
    (when active
      (kruddgui-rect* (list (+ x 6 caret) (+ y 6) 2 (- h 12))
		      kruddgui-scene-caret-fg))))

;;! (kruddgui-scene-field L id text mode) a full-width field row. Returns the
;;! kgui-field result (display active? committed? caret-px) so the caller can
;;! write the value back on commit. Culled rows still return a benign result.
(define (kruddgui-scene-field L id text mode)
  (let* ((x  (kruddgui-lay-x L))
	 (w  (kruddgui-lay-w L))
	 (y  (kruddgui-lay-cy L))
	 (h  kruddgui-scene-row-h)
	 (r  (if (kruddgui-lay-vis? L h)
		 (let ((res (kgui-field id x y w h text mode)))
		   (kruddgui-scene-field-draw x y w h (car res) (cadr res)
					      (cadddr res))
		   res)
		 (list text #f #f 0.0))))
    (kruddgui-lay-adv! L (+ h kruddgui-scene-gap))
    r))

;;! (kruddgui-scene-label L str) a section label line in accent blue.
(define (kruddgui-scene-label L str)
  (let ((x (kruddgui-lay-x L))
	(y (kruddgui-lay-cy L))
	(h kruddgui-scene-line))
    (when (kruddgui-lay-vis? L h)
      (kruddgui-board-cell (+ x 2) y h str kruddgui-scene-label-fg))
    (kruddgui-lay-adv! L h)))

;;! (kruddgui-scene-kv L label value) a read-only "label: value" line.
(define (kruddgui-scene-kv L label value)
  (let ((x (kruddgui-lay-x L))
	(w (kruddgui-lay-w L))
	(y (kruddgui-lay-cy L))
	(h kruddgui-scene-line))
    (when (kruddgui-lay-vis? L h)
      (kruddgui-board-cell (+ x 2) y h label kruddgui-idle-fg)
      (kruddgui-board-cell (+ x (* w 0.42)) y h value kruddgui-idle-fg))
    (kruddgui-lay-adv! L h)))

;;! (kruddgui-scene-rule L) a thin divider spanning the row width.
(define (kruddgui-scene-rule L)
  (let ((x (kruddgui-lay-x L))
	(w (kruddgui-lay-w L))
	(y (kruddgui-lay-cy L)))
    (when (kruddgui-lay-vis? L 8)
      (kruddgui-rect* (list x (+ y 3) w 1) kruddgui-scene-rule-fg))
    (kruddgui-lay-adv! L 8)))

;;! (kruddgui-scene-btn L label enabled?) a full-width tap button row. Returns #t
;;! on tap (only when enabled and visible).
(define (kruddgui-scene-btn L label enabled?)
  (let* ((x (kruddgui-lay-x L))
	 (w (kruddgui-lay-w L))
	 (y (kruddgui-lay-cy L))
	 (h kruddgui-scene-row-h)
	 (hit #f))
    (when (kruddgui-lay-vis? L h)
      (kruddgui-rect* (list x y w h)
		      (if enabled? kruddgui-idle-bg kruddgui-panel-bg))
      (kruddgui-label x y w h label kruddgui-idle-fg)
      (when (and enabled? (kgui-button x y w h))
	(set! hit #t)))
    (kruddgui-lay-adv! L (+ h kruddgui-scene-gap))
    hit))

;;! (kruddgui-scene-numeric x y w id cur) one numeric field cell at an explicit
;;! rect (used to pack several across a transform row). Returns (value .
;;! committed?): the parsed value, falling back to CUR while the buffer is empty
;;! or mid-edit (e.g. "-" or "1."), so an unparseable partial never writes.
(define (kruddgui-scene-numeric x y w id cur)
  (let* ((fmt (format #f "~,3F" cur))
	 (res (kgui-field id x y w kruddgui-scene-row-h fmt 1))
	 (disp (car res))
	 (val  (string->number disp)))
    (kruddgui-scene-field-draw x y w kruddgui-scene-row-h disp (cadr res)
			       (cadddr res))
    (cons (if (real? val) val cur) (caddr res))))

;;! (kruddgui-scene-vec-row L label id-base vec) a labelled row of one numeric
;;! field per component, packed across the row width. Returns (new-vec .
;;! changed?): the per-component values and whether any component committed.
(define (kruddgui-scene-vec-row L label id-base vec)
  (kruddgui-scene-label L label)
  (let* ((x (kruddgui-lay-x L))
	 (w (kruddgui-lay-w L))
	 (y (kruddgui-lay-cy L))
	 (n (length vec))
	 (g kruddgui-scene-gap)
	 (fw (/ (- w (* (- n 1) g)) n)))
    (let ((res
	   (if (kruddgui-lay-vis? L kruddgui-scene-row-h)
	       (let loop ((i 0) (vs vec) (acc '()))
		 (if (null? vs)
		     (reverse acc)
		     (let ((fx (+ x (* i (+ fw g)))))
		       (loop (+ i 1) (cdr vs)
			     (cons (kruddgui-scene-numeric
				    fx y fw (format #f "~A~D" id-base i) (car vs))
				   acc)))))
	       (map (lambda (v) (cons v #f)) vec))))
      (kruddgui-lay-adv! L (+ kruddgui-scene-row-h kruddgui-scene-gap))
      (cons (map car res)
	    (let anyc ((r res)) (cond ((null? r) #f)
				      ((cdr (car r)) #t)
				      (else (anyc (cdr r)))))))))

;;! (kruddgui-scene-combo L id label preview options ref can-edit setter e) an
;;! inline combo: a labelled preview button that toggles the option list open (one
;;! combo open at a time, keyed by ID). While open it lists a "(none)" unbind
;;! entry then one row per (aid . path) option; a tap selects through SETTER and
;;! closes. Disabled combos still draw but do not open.
(define (kruddgui-scene-combo L id label preview options ref can-edit setter e)
  (kruddgui-scene-label L label)
  (let ((opened (equal? kruddgui-scene-open-combo id)))
    (when (kruddgui-scene-btn L preview can-edit)
      (set! kruddgui-scene-open-combo (if opened #f id)))
    (when (and can-edit (equal? kruddgui-scene-open-combo id))
      (when (kruddgui-scene-btn L "  (none)" #t)
	(setter e 0)
	(set! kruddgui-scene-open-combo #f))
      (for-each
       (lambda (a)
	 (let ((aid (car a))
	       (path (cdr a)))
	   (when (kruddgui-scene-btn L (string-append "  " path) #t)
	     (setter e aid)
	     (set! kruddgui-scene-open-combo #f))))
       options))))

;;! (kruddgui-scene-param-menu L title e params values save-fn) a collapsing-free
;;! param editor: a label then one numeric field per component of each param, with
;;! a colour swatch ahead of a 3/4-component colour param. Collects every widget's
;;! (values . changed?) and, if any committed, packs the new values and hands them
;;! to SAVE-FN — the entity's undo-recording per-param setter — so the override
;;! lands immediately, no explicit Save, exactly as the ImGui menu did.
(define (kruddgui-scene-param-menu L title e id-tag params values save-fn)
  (unless (null? params)
    (kruddgui-scene-rule L)
    (kruddgui-scene-label L title)
    (let* ((results
	    (map (lambda (p v pidx)
		   (kruddgui-scene-param-widget L e
		       (format #f "~A-~D" id-tag pidx) p v))
		 params values (kruddgui-scene-iota (length params))))
	   (new-vals (map car results))
	   (changed  (let anyc ((r results))
		       (cond ((null? r) #f)
			     ((cdr (car r)) #t)
			     (else (anyc (cdr r)))))))
      (when changed (save-fn e new-vals)))))

;;! (kruddgui-scene-iota n) -> (0 1 ... n-1), for stable per-param id suffixes.
(define (kruddgui-scene-iota n)
  (let loop ((i (- n 1)) (acc '()))
    (if (< i 0) acc (loop (- i 1) (cons i acc)))))

;;! (kruddgui-scene-param-widget L e id param value) draws one param as component
;;! numeric fields (a colour param gets a live swatch first) and returns (value .
;;! changed?), the value staying a list so the save path packs it uniformly.
(define (kruddgui-scene-param-widget L e id param value)
  (let ((comps (list-ref param 4))
	(kind  (list-ref param 5)))
    (kruddgui-scene-label L (list-ref param 0))
    (let* ((x (kruddgui-lay-x L))
	   (w (kruddgui-lay-w L))
	   (color? (and (string=? kind "color") (>= comps 3)))
	   (sw (if color? 34 0))
	   (fx0 (+ x sw))
	   (fw-tot (- w sw)))
      (when (and color? (kruddgui-lay-vis? L kruddgui-scene-row-h))
	(kruddgui-rect* (list x (kruddgui-lay-cy L)
			      (- sw kruddgui-scene-gap) kruddgui-scene-row-h)
			(kruddgui-scene-color-of value comps)))
      (let* ((y (kruddgui-lay-cy L))
	     (g kruddgui-scene-gap)
	     (fw (if (> comps 0) (/ (- fw-tot (* (- comps 1) g)) comps) fw-tot))
	     (res
	      (if (and (> comps 0) (kruddgui-lay-vis? L kruddgui-scene-row-h))
		  (let loop ((i 0) (vs value) (acc '()))
		    (if (or (null? vs) (>= i comps))
			(reverse acc)
			(let ((cx (+ fx0 (* i (+ fw g)))))
			  (loop (+ i 1) (cdr vs)
				(cons (kruddgui-scene-numeric
				       cx y fw (format #f "~A~D" id i) (car vs))
				      acc)))))
		  (map (lambda (v) (cons v #f)) value))))
	(kruddgui-lay-adv! L (+ kruddgui-scene-row-h kruddgui-scene-gap))
	(cons (map car res)
	      (let anyc ((r res)) (cond ((null? r) #f)
				       ((cdr (car r)) #t)
				       (else (anyc (cdr r))))))))))

;;! (kruddgui-scene-color-of value comps) an (r g b a) swatch colour from a param
;;! value list, opaque when the param carries no alpha component.
(define (kruddgui-scene-color-of value comps)
  (let ((r (if (pair? value) (car value) 0.0))
	(g (if (>= (length value) 2) (cadr value) 0.0))
	(b (if (>= (length value) 3) (caddr value) 0.0))
	(a (if (>= comps 4) (list-ref value 3) 1.0)))
    (list r g b a)))

;;! (kruddgui-scene-bindings L e info can-bind) the Mesh / Material / Script combos
;;! plus each bound asset's param menu, mirroring kruddboard-draw-inspector-body's
;;! Bindings section but on kruddgui widgets.
(define (kruddgui-scene-bindings L e info can-bind)
  (let ((has-render   (list-ref info 6))
	(has-material (list-ref info 7))
	(render-ref   (list-ref info 8))
	(material-ref (list-ref info 9))
	(has-script   (list-ref info 10))
	(script-ref   (list-ref info 11)))
    (kruddgui-scene-combo L "mesh" "Mesh"
	(kruddgui-binding-label has-render render-ref) (krudd-mesh-assets)
	render-ref can-bind krudd-entity-set-render-ref e)
    (when has-render
      (kruddgui-scene-param-menu L "Mesh Parameters" e "mshp"
	  (krudd-mesh-params render-ref)
	  (krudd-entity-mesh-values e render-ref)
	  (lambda (e2 nv) (krudd-entity-save-mesh-params e2 render-ref nv))))
    (kruddgui-scene-combo L "material" "Material"
	(kruddgui-binding-label has-material material-ref)
	(krudd-material-assets) material-ref can-bind
	krudd-entity-set-material-ref e)
    (when has-material
      (kruddgui-scene-material-params L e material-ref))
    (kruddgui-scene-combo L "script" "Script"
	(kruddgui-binding-label has-script script-ref) (krudd-script-assets)
	script-ref can-bind krudd-entity-set-script-ref e)
    (when has-script
      (kruddgui-scene-param-menu L "Script Parameters" e "sp"
	  (krudd-script-params script-ref)
	  (krudd-entity-script-values e script-ref)
	  (lambda (e2 nv)
	    (krudd-entity-save-script-params e2 script-ref nv))))))

;;! (kruddgui-binding-label bound? ref) the combo preview: "(none)" when unbound,
;;! the asset's path when it resolves, else "(missing #ref)" — the World twin of
;;! kruddboard-binding-label.
(define (kruddgui-binding-label bound? ref)
  (if (or (not bound?) (= ref 0))
      "(none)"
      (let ((path (krudd-asset-find ref)))
	(if (string? path) path (format #f "(missing #~D)" ref)))))

;;! (kruddgui-scene-material-params L e material-ref) the bound material's shader
;;! params and, when its texture slot declares params, that texture's — the
;;! per-entity override layer, saved immediately through the undo-recording setter.
(define (kruddgui-scene-material-params L e material-ref)
  (let ((shader-ref (krudd-asset-shader-ref material-ref)))
    (unless (= shader-ref 0)
      (kruddgui-scene-param-menu L "Material Parameters" e "mp"
	  (krudd-shader-material-params shader-ref)
	  (krudd-entity-material-values e material-ref shader-ref)
	  (lambda (e2 nv)
	    (krudd-entity-save-material-params e2 shader-ref nv))))
    (let ((slot (krudd-material-texture material-ref)))
      (when (pair? slot)
	(kruddgui-scene-param-menu L "Texture Parameters" e "texp"
	    (krudd-texture-params (car slot))
	    (krudd-entity-texture-values e (car slot))
	    (lambda (e2 nv)
	      (krudd-entity-save-texture-params e2 (car slot) nv)))))))

;;! (kruddgui-scene-inspector L e info can-entity can-asset) the drilled-in
;;! inspector body: the name field, the Position / Rotation / Scale rows, a read-
;;! only Info block, and the Bindings section. Each edit writes back through the
;;! matching krudd-entity-* setter, which records an undo step.
(define (kruddgui-scene-inspector L e info can-entity can-asset)
  (let ((name (list-ref info 0))
	(pos  (list-ref info 1))
	(rot  (list-ref info 2))
	(scl  (list-ref info 3)))
    (kruddgui-scene-label L "Name")
    (let ((nf (kruddgui-scene-field L "ename" name 0)))
      (when (and can-entity (caddr nf))
	(krudd-entity-set-name e (car nf))))
    (kruddgui-scene-rule L)
    (let ((pr (kruddgui-scene-vec-row L "Position" "pos" pos))
	  (rr (kruddgui-scene-vec-row L "Rotation" "rot" rot))
	  (sr (kruddgui-scene-vec-row L "Scale" "scl" scl)))
      (when (and can-entity (or (cdr pr) (cdr rr) (cdr sr)))
	(krudd-entity-set-transform e (car pr) (car rr) (car sr))))
    (kruddgui-scene-rule L)
    (kruddgui-scene-label L "Info")
    (kruddgui-scene-kv L "Entity ID" (format #f "~D" e))
    (kruddgui-scene-kv L "Parent" (kruddboard-parent-label (list-ref info 4)))
    (kruddgui-scene-rule L)
    (kruddgui-scene-label L "Bindings")
    (kruddgui-scene-bindings L e info (and can-entity can-asset))))

;;! (kruddboard-parent-label parent) formats the Parent line: "(root)", "name
;;! (#id)", or "entity id" — moved here with the inspector it serves.
(define (kruddboard-parent-label parent)
  (if (not parent)
      "(root)"
      (let ((pid (car parent))
	    (pname (cdr parent)))
	(if (string? pname)
	    (format #f "~A (#~D)" pname pid)
	    (format #f "entity ~D" pid)))))

;;! (kruddgui-scene-list L caps) the entity list screen: a "+ Entity" button then
;;! one row per entity — a tap drills into that entity's inspector (and drives the
;;! viewport selection so the gizmo tracks it), the right-edge x destroys it. The
;;! list is materialised so destroying a row mid-frame leaves iteration intact.
(define (kruddgui-scene-list L caps)
  (let ((has-api (car caps)))
    (when (and (kruddgui-scene-btn L "+ Entity" has-api))
      (kruddgui-scene-create))
    (let ((ents (krudd-world-entities)))
      (if (or (not ents) (null? ents))
	  (kruddgui-scene-label L "(no entities)")
	  (for-each (lambda (row) (kruddgui-scene-list-row L row has-api))
		    ents)))))

;;! (kruddgui-scene-create) appends an entity, names it, selects it and drills in
;;! — the "+ Entity" action, the twin of kruddboard-world-create.
(define (kruddgui-scene-create)
  (let ((id (krudd-entity-create)))
    (when (>= id 0)
      (krudd-entity-set-name id "Entity")
      (krudd-entity-select id)
      (set! kruddgui-scene-sel id))))

;;! (kruddgui-scene-list-row L row has-api) one entity row: the name area drills
;;! in (checked after the x so a tap on the x deletes without also drilling), the
;;! right-edge x destroys the entity when the scene api is present.
(define (kruddgui-scene-list-row L row has-api)
  (let* ((id   (car row))
	 (disp (if (string? (cdr row)) (cdr row) (format #f "entity ~D" id)))
	 (x    (kruddgui-lay-x L))
	 (w    (kruddgui-lay-w L))
	 (y    (kruddgui-lay-cy L))
	 (h    kruddgui-scene-row-h)
	 (bs   28)
	 (bx   (- (+ x w) bs 4)))
    (when (kruddgui-lay-vis? L h)
      (kruddgui-rect* (list x y w h)
		      (if (= id (krudd-selected))
			  kruddgui-active-bg kruddgui-idle-bg))
      (kruddgui-board-cell (+ x 8) y h disp
			   (if (= id (krudd-selected))
			       kruddgui-active-fg kruddgui-idle-fg))
      (when has-api
	(kruddgui-rect* (list bx (+ y 6) bs (- h 12)) kruddgui-panel-bg)
	(kruddgui-label bx (+ y 6) bs (- h 12) "x" kruddgui-idle-fg))
      (cond
       ((and has-api (kgui-button bx (+ y 6) bs (- h 12)))
	(krudd-entity-destroy id))
       ((kgui-button x y w h)
	(krudd-entity-select id)
	(set! kruddgui-scene-sel id))))
    (kruddgui-lay-adv! L (+ h kruddgui-scene-gap))))

;;! (kruddgui-scene-body x y w h caps) the scrolling inspector/list body. It
;;! clamps the scroll against last frame's measured content height, draws every
;;! row through the layout cursor (culling off-body rows), records the height for
;;! next frame, and — when drilled in but the entity no longer resolves — falls
;;! back to the list, the stale-guard the ImGui inspector used.
(define (kruddgui-scene-body x y w h caps)
  (let* ((min-off (min 0.0 (- h kruddgui-scene-total)))
	 (off     (max min-off (min 0.0 kruddgui-scene-scroll)))
	 (ix      (+ x kruddgui-scene-pad))
	 (iw      (- w (* 2 kruddgui-scene-pad)))
	 (L       (kruddgui-lay (+ y off) y (+ y h) ix iw)))
    (set! kruddgui-scene-scroll off)
    (kruddgui-rect* (list x y w h) kruddgui-scene-body-bg)
    (kgui-clip x y w h)
    (if (= kruddgui-scene-sel -1)
	(kruddgui-scene-list L caps)
	(let ((info (krudd-entity-inspect kruddgui-scene-sel)))
	  (if (not info)
	      (set! kruddgui-scene-sel -1)
	      (kruddgui-scene-inspector L kruddgui-scene-sel info
					(car caps) (cadr caps)))))
    (kgui-clip-none)
    (set! kruddgui-scene-total
	  (- (kruddgui-lay-cy L) (+ y off)))))

;;! (kruddgui-scene-draw-header x y w hdr) the title, a "< Back" affordance when
;;! drilled in, and the close box. Back returns to the list; close collapses the
;;! console. Both taps are trapped by the console region.
(define (kruddgui-scene-draw-header x y w hdr)
  (let* ((bs 26)
	 (bx (- (+ x w) 8 bs))
	 (by (+ y (/ (- hdr bs) 2))))
    (if (= kruddgui-scene-sel -1)
	(kruddgui-label x y 120 hdr "SCENE" kruddgui-idle-fg)
	(begin
	  (kruddgui-rect* (list (+ x 6) by 64 bs) kruddgui-idle-bg)
	  (kruddgui-label (+ x 6) by 64 bs "< Back" kruddgui-idle-fg)
	  (when (kgui-button (+ x 6) by 64 bs)
	    (set! kruddgui-scene-sel -1)
	    (set! kruddgui-scene-open-combo #f))))
    (kruddgui-rect* (list bx by bs bs) kruddgui-idle-bg)
    (kruddgui-label bx by bs bs "x" kruddgui-idle-fg)
    (when (kgui-button bx by bs bs)
      (set! kruddgui-scene-open #f))))

;;! (kruddgui-scene-draw-panel vw vh) the expanded console: a top-left overlay
;;! (drawn last, so it wins any overlap with the Log/board consoles) that stops
;;! short of the mode-bar's reserved band. Drag/wheel on the region scrolls the
;;! body before it is redrawn (then re-clamped there).
(define (kruddgui-scene-draw-panel vw vh)
  (let* ((m      kruddgui-log-margin)
	 (avail  (- vh m (kruddgui-modebar-reserve vw vh) kruddgui-gap))
	 (w      (min kruddgui-scene-max-w (- vw (* 2 m))))
	 (h      (max 160.0 (min (* vh 0.72) avail)))
	 (x      m)
	 (y      m)
	 (hdr    kruddgui-scene-header-h)
	 (body-y (+ y hdr))
	 (body-h (- h hdr)))
    (kgui-panel-begin "kgui-scene" x y w h)
    (kruddgui-rect* (list x y w h) kruddgui-scene-panel-bg)
    (kruddgui-scene-draw-header x y w hdr)
    (set! kruddgui-scene-scroll
	  (+ kruddgui-scene-scroll
	     (cadr (kgui-region-drag))
	     (kgui-region-wheel)))
    (kruddgui-scene-body x body-y w body-h (krudd-world-caps))
    (kgui-panel-end)))

;;! (kruddgui-scene-draw-handle vw vh) the collapsed console: a pill in the bottom-
;;! left stack, above the STATS handle (STATS above LOG). Tap expands the console.
(define (kruddgui-scene-draw-handle vw vh)
  (let* ((m  kruddgui-log-margin)
	 (hw kruddgui-log-handle-w)
	 (hh kruddgui-log-handle-h)
	 (x  m)
	 (y  (- vh m hh hh hh (* 2 kruddgui-gap))))
    (kgui-panel-begin "kgui-scene" x y hw hh)
    (kruddgui-rect* (list x y hw hh) kruddgui-scene-panel-bg)
    (kruddgui-label x y hw hh "SCENE" kruddgui-idle-fg)
    (when (kgui-button x y hw hh)
      (set! kruddgui-scene-open #t))
    (kgui-panel-end)))

;;! ------------------------------------------------------------------
;;! Widget foundations — the draggable slider and 2D colour picker (#492, PR6a)
;;! ------------------------------------------------------------------

;;! The Assets tab (the heaviest ImGui consumer left) leans on two widgets the
;;! read-only consoles never needed: a draggable slider (its material / texture
;;! params are numeric fields today) and a colour picker (its colour params).
;;! Both are built here on the per-widget drag-capture seam (kgui-region): each
;;! declares a small input region at its own rect, on TOP of the enclosing scroll
;;! body, so a down inside it is captured and mapped to a value while a down
;;! elsewhere still scrolls the body. The material / texture editors the Assets
;;! browser drills into (6c) bind these to real params; until then they are covered
;;! by the widget host test (kgui_widgets_test) driving them directly. Per touch
;;! convention a drag that starts on a slider does not also start a scroll (the
;;! slider's region ate the down); note it.

(define kruddgui-slider-track-h 8)
(define kruddgui-slider-knob-w 14)
(define kruddgui-slider-fill     '(0.95 0.55 0.15 0.95))
(define kruddgui-slider-track-bg '(0.20 0.22 0.26 1.0))
(define kruddgui-slider-knob     '(0.92 0.94 0.98 1.0))

;;! (kruddgui-slider L id label value lo hi) a labelled draggable track. The label
;;! and the live value sit on one line; the track fills the next row with a fill
;;! bar and a knob. Its region spans the track, so a down anywhere on it (tap or
;;! drag) jumps the value: the captured pointer's x maps across [lo hi]. Returns
;;! (value . changed?) — changed? #t on any frame the drag moved the value — so
;;! the caller writes the new value back through its setter, the drag-native twin
;;! of the numeric field's commit. Culled off-body rows declare no region.
(define (kruddgui-slider L id label value lo hi)
  (let* ((x    (kruddgui-lay-x L))
	 (w    (kruddgui-lay-w L))
	 (ly   (kruddgui-lay-cy L))
	 (lh   kruddgui-scene-line)
	 (th   kruddgui-scene-row-h)
	 (ty   (+ ly lh))
	 (span (- hi lo))
	 (vis  (kruddgui-lay-vis? L (+ lh th)))
	 (st   (if vis (kgui-region id x ty w th) (list #f 0.0 0.0)))
	 (nv   (if (and (car st) (> span 0))
		   (+ lo (* (max 0.0 (min 1.0 (/ (- (cadr st) x) w))) span))
		   value))
	 (changed (not (= nv value))))
    (when vis
      (kruddgui-board-cell (+ x 2) ly lh label kruddgui-scene-label-fg)
      (let* ((vstr (format #f "~,3F" nv))
	     (vw   (car (kgui-text-metrics vstr))))
	(kruddgui-board-cell (- (+ x w) vw 2) ly lh vstr kruddgui-idle-fg))
      (let* ((trk-y (+ ty (/ (- th kruddgui-slider-track-h) 2)))
	     (kw    kruddgui-slider-knob-w)
	     (t     (if (> span 0) (max 0.0 (min 1.0 (/ (- nv lo) span))) 0.0))
	     (run   (* t (- w kw))))
	(kruddgui-rect* (list x trk-y w kruddgui-slider-track-h)
			kruddgui-slider-track-bg)
	(kruddgui-rect* (list x trk-y (+ run (/ kw 2)) kruddgui-slider-track-h)
			kruddgui-slider-fill)
	(kruddgui-rect* (list (+ x run) ty kw th) kruddgui-slider-knob)))
    (kruddgui-lay-adv! L (+ lh th kruddgui-scene-gap))
    (cons nv changed)))

;;! HSV<->RGB, all channels in 0..1, hue wrapped to [0,1). The colour picker works
;;! in HSV — an SV square at a fixed hue plus a hue strip — and stores RGB, so it
;;! round-trips through these each frame.
(define (kruddgui-hsv->rgb h s v)
  (let* ((h6 (* (- h (floor h)) 6.0))
	 (i  (modulo (inexact->exact (floor h6)) 6))
	 (f  (- h6 (floor h6)))
	 (p  (* v (- 1.0 s)))
	 (q  (* v (- 1.0 (* s f))))
	 (u  (* v (- 1.0 (* s (- 1.0 f))))))
    (cond ((= i 0) (list v u p))
	  ((= i 1) (list q v p))
	  ((= i 2) (list p v u))
	  ((= i 3) (list p q v))
	  ((= i 4) (list u p v))
	  (else    (list v p q)))))

(define (kruddgui-rgb->hsv r g b)
  (let* ((mx (max r g b))
	 (mn (min r g b))
	 (d  (- mx mn))
	 (v  mx)
	 (s  (if (> mx 0.0) (/ d mx) 0.0))
	 (h6 (cond ((= d 0.0) 0.0)
		   ((= mx r) (/ (- g b) d))
		   ((= mx g) (+ 2.0 (/ (- b r) d)))
		   (else     (+ 4.0 (/ (- r g) d))))))
    (list (/ (if (< h6 0.0) (+ h6 6.0) h6) 6.0) s v)))

;;! Picker geometry: the SV square is a fixed side; the hue strip a slim bar to
;;! its right. Both are drawn as a grid / stack of flat cells since kgui-rect only
;;! fills — a cheap raster of the gradient, fine at touch scale.
(define kruddgui-picker-side 132)
(define kruddgui-picker-hue-w 22)
(define kruddgui-picker-sv-nx 12)
(define kruddgui-picker-sv-ny 10)
(define kruddgui-picker-hue-n 12)
(define kruddgui-picker-cursor '(0.98 0.99 1.0 1.0))

;;! The id of the one colour picker currently open (or #f), so tapping a second
;;! swatch closes the first — the same one-open-at-a-time discipline as the combos.
(define kruddgui-open-picker #f)

;;! (kruddgui-picker-sv L id h s v svx svy side) the saturation/value square at a
;;! fixed hue: x is saturation, y is value (top = 1). A grid of hsv->rgb cells with
;;! the current (s v) marked by a cursor box; its region maps a captured pointer to
;;! (s . v). Returns (s . v), unchanged when not pressed.
(define (kruddgui-picker-sv id h s v svx svy side)
  (let* ((nx kruddgui-picker-sv-nx)
	 (ny kruddgui-picker-sv-ny)
	 (cw (/ side nx))
	 (ch (/ side ny)))
    (let yloop ((j 0))
      (when (< j ny)
	(let xloop ((i 0))
	  (when (< i nx)
	    (let* ((cs (/ (+ i 0.5) nx))
		   (cv (- 1.0 (/ (+ j 0.5) ny)))
		   (c  (kruddgui-hsv->rgb h cs cv)))
	      (kruddgui-rect* (list (+ svx (* i cw)) (+ svy (* j ch)) cw ch)
			      (append c (list 1.0))))
	    (xloop (+ i 1))))
	(yloop (+ j 1))))
    (let* ((st  (kgui-region id svx svy side side))
	   (prs (car st))
	   (ns  s)
	   (nv  v))
      (when prs
	(set! ns (max 0.0 (min 1.0 (/ (- (cadr st) svx) side))))
	(set! nv (max 0.0 (min 1.0 (- 1.0 (/ (- (caddr st) svy) side))))))
      (let ((cx (+ svx (* ns side)))
	    (cy (+ svy (* (- 1.0 nv) side))))
	(kruddgui-rect* (list (- cx 4) (- cy 1) 8 2) kruddgui-picker-cursor)
	(kruddgui-rect* (list (- cx 1) (- cy 4) 2 8) kruddgui-picker-cursor))
      (cons ns nv))))

;;! (kruddgui-picker-hue id h hx hy side) the hue strip: a vertical stack of full-
;;! saturation bands with the current hue marked. Its region maps a captured
;;! pointer's y to a hue in [0,1). Returns the (possibly new) hue.
(define (kruddgui-picker-hue id h hx hy side)
  (let* ((n kruddgui-picker-hue-n)
	 (bh (/ side n)))
    (let loop ((i 0))
      (when (< i n)
	(let ((c (kruddgui-hsv->rgb (/ (+ i 0.5) n) 1.0 1.0)))
	  (kruddgui-rect* (list hx (+ hy (* i bh)) kruddgui-picker-hue-w bh)
			  (append c (list 1.0))))
	(loop (+ i 1))))
    (let* ((st  (kgui-region id hx hy kruddgui-picker-hue-w side))
	   (prs (car st))
	   (nh  h))
      (when prs
	(set! nh (max 0.0 (min 0.999 (/ (- (caddr st) hy) side)))))
      (let ((cy (+ hy (* nh side))))
	(kruddgui-rect* (list (- hx 2) (- cy 1) (+ kruddgui-picker-hue-w 4) 3)
			kruddgui-picker-cursor))
      nh)))

;;! (kruddgui-color-swatch L id label rgb) a labelled colour field: a swatch row
;;! showing the current colour, which tapping opens into an SV square + hue strip
;;! (one picker open at a time, keyed by ID). Drag on either recomposes the RGB
;;! through HSV. Returns (rgb . changed?), the rgb a fresh 3-list; any 4th (alpha)
;;! component of the input is preserved. Culled rows draw and declare nothing.
(define (kruddgui-color-swatch L id label rgb)
  (kruddgui-scene-label L label)
  (let* ((x     (kruddgui-lay-x L))
	 (w     (kruddgui-lay-w L))
	 (y     (kruddgui-lay-cy L))
	 (h     kruddgui-scene-row-h)
	 (r     (if (pair? rgb) (car rgb) 0.0))
	 (g     (if (>= (length rgb) 2) (cadr rgb) 0.0))
	 (b     (if (>= (length rgb) 3) (caddr rgb) 0.0))
	 (alpha (if (>= (length rgb) 4) (list (list-ref rgb 3)) '()))
	 (nrgb  (list r g b))
	 (changed #f))
    (when (kruddgui-lay-vis? L h)
      (kruddgui-rect* (list x y w h) (list r g b 1.0))
      (kruddgui-rect* (list x y w 2) kruddgui-scene-rule-fg)
      (when (kgui-button x y w h)
	(set! kruddgui-open-picker
	      (if (equal? kruddgui-open-picker id) #f id))))
    (kruddgui-lay-adv! L (+ h kruddgui-scene-gap))
    (when (equal? kruddgui-open-picker id)
      (let* ((side kruddgui-picker-side)
	     (px   (kruddgui-lay-x L))
	     (py   (kruddgui-lay-cy L)))
	(when (kruddgui-lay-vis? L side)
	  (let* ((hsv (kruddgui-rgb->hsv r g b))
		 (hx  (+ px side kruddgui-scene-gap))
		 (sv  (kruddgui-picker-sv (string-append id "-sv")
					  (car hsv) (cadr hsv) (caddr hsv)
					  px py side))
		 (nh  (kruddgui-picker-hue (string-append id "-hue")
					   (car hsv) hx py side))
		 (out (kruddgui-hsv->rgb nh (car sv) (cdr sv))))
	    (set! nrgb out)
	    (set! changed (not (equal? out (list r g b))))))
	(kruddgui-lay-adv! L (+ side kruddgui-scene-gap))))
    (cons (append nrgb alpha) changed)))

;;! ------------------------------------------------------------------
;;! Shared layout vocabulary — a fold header and a button row (#492, PR6b)
;;! ------------------------------------------------------------------
;;! The layout gaps the Assets lift needs that the Scene tab never grew: ImGui's
;;! collapsing-header and its same-line button strips. Both build on the layout
;;! cursor and the tap-button/one-open-keyed-by-id shapes above, so the Assets
;;! port (and any later tab) shares one vocabulary instead of re-deriving it.

;;! Open fold ids as an assoc list (id . open?). Folds toggle independently —
;;! unlike the one-at-a-time combos and colour picker — so this is a set, not a
;;! single id. A fold absent from the list takes the caller's default.
(define kruddgui-fold-state '())

(define (kruddgui-fold-open? id default)
  (let ((p (assoc id kruddgui-fold-state)))
    (if p (cdr p) default)))

(define (kruddgui-fold-set! id open?)
  (let loop ((l kruddgui-fold-state) (acc '()))
    (cond ((null? l) (set! kruddgui-fold-state (cons (cons id open?) acc)))
	  ((equal? (caar l) id) (loop (cdr l) acc))
	  (else (loop (cdr l) (cons (car l) acc))))))

;;! (kruddgui-fold L id label default) a full-width collapsing header: a marker
;;! (v open / > closed — the atlas is ASCII, so a letter stands in for the
;;! triangle) and the label, tapping toggles the id's open flag. Returns #t when
;;! open, so the caller draws the section body only then. Culled headers keep the
;;! stored state and return it, so an off-body fold still gates its body.
(define (kruddgui-fold L id label default)
  (let* ((x    (kruddgui-lay-x L))
	 (w    (kruddgui-lay-w L))
	 (y    (kruddgui-lay-cy L))
	 (h    kruddgui-scene-row-h)
	 (open (kruddgui-fold-open? id default)))
    (when (kruddgui-lay-vis? L h)
      (kruddgui-rect* (list x y w h) kruddgui-idle-bg)
      (kruddgui-board-cell (+ x 8) y h (if open "v" ">") kruddgui-scene-label-fg)
      (kruddgui-board-cell (+ x 26) y h label kruddgui-scene-label-fg)
      (when (kgui-button x y w h)
	(set! open (not open))
	(kruddgui-fold-set! id open)))
    (kruddgui-lay-adv! L (+ h kruddgui-scene-gap))
    open))

;;! (kruddgui-button-row L labels) lay one tap button per label across the row
;;! width — the kruddgui answer to ImGui's same-line button strips (the cursor
;;! has no implicit x, so the cells are packed here). Returns the tapped label,
;;! or #f when none was tapped this frame. Culled rows draw nothing, return #f.
(define (kruddgui-button-row L labels)
  (let* ((x   (kruddgui-lay-x L))
	 (w   (kruddgui-lay-w L))
	 (y   (kruddgui-lay-cy L))
	 (h   kruddgui-scene-row-h)
	 (n   (length labels))
	 (g   kruddgui-scene-gap)
	 (cw  (if (> n 0) (/ (- w (* (- n 1) g)) n) w))
	 (hit #f))
    (when (and (> n 0) (kruddgui-lay-vis? L h))
      (let loop ((ls labels) (i 0))
	(when (pair? ls)
	  (let ((cx (+ x (* i (+ cw g)))))
	    (kruddgui-rect* (list cx y cw h) kruddgui-idle-bg)
	    (kruddgui-label cx y cw h (car ls) kruddgui-idle-fg)
	    (when (kgui-button cx y cw h)
	      (set! hit (car ls))))
	  (loop (cdr ls) (+ i 1)))))
    (kruddgui-lay-adv! L (+ h kruddgui-scene-gap))
    hit))

;;! ------------------------------------------------------------------
;;! Assets console — the Assets tab lifted onto kruddgui (#492, PR6b)
;;! ------------------------------------------------------------------

;;! The Assets tab — the heaviest ImGui board consumer — lifts onto kruddgui's own
;;! widgets here, matching the Log / board / Scene consoles: a top-left console, a
;;! bottom-left handle, its own input region. PR6a landed the foundations (slider,
;;! colour picker) and the shared vocabulary (fold, button row); PR6b (this) ports
;;! the asset *browser* — the package sections, the folder tree and the leaf grid,
;;! plus the New Asset form — over that vocabulary, replacing PR6a's demo body. The
;;! per-type material / texture / source editors the browser drills into land in
;;! 6c; until then a tapped asset shows a read-only catalog view. The still-live
;;! ImGui Assets tab (kruddboard-draw-assets over Assets.scm) coexists until it is
;;! retired. State is held across frames like the other consoles.
(define kruddgui-assets-open #f)
(define kruddgui-assets-scroll 0.0)
(define kruddgui-assets-total 0.0)
(define kruddgui-assets-max-w 400)
(define kruddgui-assets-header-h 36)

;;! Browser selection: 0 = the package browser, else the drilled-in asset id.
;;! Asset ids are >= 1, so 0 is a safe "no selection" sentinel (the same
;;! convention Assets.scm's kruddboard-assets-sel uses).
(define kruddgui-assets-sel 0)

;;! New Asset form state: whether the name/type editor is showing, the name
;;! field's live text, and the selected type index (0 Text 1 Shader 2 Material
;;! 3 Script 4 Mesh — the order Assets.scm's type combo uses).
(define kruddgui-assets-naming #f)
(define kruddgui-assets-new-name "")
(define kruddgui-assets-new-type 0)
(define kruddgui-assets-new-types '("Text" "Shader" "Material" "Script" "Mesh"))

;;! ------------------------------------------------------------------
;;! Asset browser — path-tree helpers (lifted from Assets.scm)
;;! ------------------------------------------------------------------
;;! The browser groups rows into a folder tree by splitting each asset path on
;;! "/", the virtual-filesystem convention builtin:// paths already use
;;! ("builtin://shader/scene-textured" -> folder "shader", leaf "scene-textured").
;;! These are Assets.scm's pure tree-builder helpers, lifted here under kruddgui-
;;! names (both images load into the one shared s7, so the names must not clash) so
;;! the kruddgui browser can build the same tree without reaching into kruddboard's
;;! module. This s7 carries no core `filter`, so the splits are spelled by hand.

;;! (kruddgui-string-split str ch) splits STR at each CH, dropping empty pieces —
;;! so a leading "/" or a doubled "//" makes no blank segment.
(define (kruddgui-string-split str ch)
  (let loop ((start 0) (acc '()))
    (let ((pos (char-position ch str start)))
      (if pos
	  (loop (+ pos 1)
		(let ((seg (substring str start pos)))
		  (if (string=? seg "") acc (cons seg acc))))
	  (let ((seg (substring str start (string-length str))))
	    (reverse (if (string=? seg "") acc (cons seg acc))))))))

;;! (kruddgui-strip-builtin-prefix path) drops a leading "builtin://" scheme so
;;! "shader"/"material" read as folders, not URI noise.
(define (kruddgui-strip-builtin-prefix path)
  (let ((prefix "builtin://"))
    (if (and (>= (string-length path) (string-length prefix))
	     (string=? (substring path 0 (string-length prefix)) prefix))
	(substring path (string-length prefix) (string-length path))
	path)))

;;! (kruddgui-asset-path-segments path) the folder segments the tree groups by:
;;! the scheme prefix dropped, then a split on "/"; a path with no "/" comes back
;;! as its own single-element list (a top-level leaf).
(define (kruddgui-asset-path-segments path)
  (let ((segs (kruddgui-string-split (kruddgui-strip-builtin-prefix path) #\/)))
    (if (null? segs) (list path) segs)))

;;! (kruddgui-asset-rows->entries rows) turns a flat ROWS list (one group as
;;! krudd-assets returns it) into (segments row) entries, the shape the tree
;;! groups and recurses on.
(define (kruddgui-asset-rows->entries rows)
  (map (lambda (row)
	 (list (kruddgui-asset-path-segments (list-ref row 1)) row))
       rows))

;;! (kruddgui-asset-entries-at-depth entries want-folders?) splits ENTRIES by
;;! whether their segment list still nests (> 1 element, under a folder) or has
;;! bottomed out at one (a leaf); WANT-FOLDERS? selects which half comes back.
(define (kruddgui-asset-entries-at-depth entries want-folders?)
  (cond ((null? entries) '())
	(else
	 (let* ((e (car entries))
		(segs (list-ref e 0))
		(is-folder (pair? (cdr segs)))
		(rest (kruddgui-asset-entries-at-depth
		       (cdr entries) want-folders?)))
	   (if (eq? is-folder want-folders?) (cons e rest) rest)))))

;;! (kruddgui-asset-entries-with-head entries head) the ENTRIES whose next path
;;! segment is HEAD.
(define (kruddgui-asset-entries-with-head entries head)
  (cond ((null? entries) '())
	(else
	 (let* ((e (car entries))
		(segs (list-ref e 0))
		(rest (kruddgui-asset-entries-with-head (cdr entries) head)))
	   (if (string=? (car segs) head) (cons e rest) rest)))))

;;! (kruddgui-uniq lst) LST's elements in first-appearance order, later
;;! duplicates dropped (compared with equal?, via member).
(define (kruddgui-uniq lst)
  (let loop ((lst lst) (seen '()))
    (cond ((null? lst) (reverse seen))
	  ((member (car lst) seen) (loop (cdr lst) seen))
	  (else (loop (cdr lst) (cons (car lst) seen))))))

;;! (kruddgui-asset-group-by-head entries) partitions ENTRIES — all segment lists
;;! length > 1 — into one (head . child-entries) bucket per distinct first
;;! segment, in first-appearance order, each child stripped of the head segment,
;;! ready to recurse one level deeper.
(define (kruddgui-asset-group-by-head entries)
  (map (lambda (head)
	 (cons head
	       (map (lambda (e) (list (cdr (list-ref e 0)) (list-ref e 1)))
		    (kruddgui-asset-entries-with-head entries head))))
       (kruddgui-uniq (map (lambda (e) (car (list-ref e 0))) entries))))

;;! Asset type / kind / state labels — the integer codes mirror asset_api.h, the
;;! same raw-int-from-C convention Assets.scm's label helpers use.
(define (kruddgui-asset-type-label t)
  (cond ((= t 1) "Mesh") ((= t 2) "Texture") ((= t 3) "Material")
	((= t 4) "Shader") ((= t 5) "Font") ((= t 6) "Scene")
	((= t 7) "Text") ((= t 8) "Script") (else "?")))

(define (kruddgui-asset-kind-label k) (if (= k 1) "Primitive" "Normal"))

(define (kruddgui-asset-state-label s)
  (cond ((= s 0) "Pending") ((= s 1) "Loaded") (else "Error")))

;;! ------------------------------------------------------------------
;;! Asset browser — the package sections, folder tree and leaf grid
;;! ------------------------------------------------------------------

;;! Grid columns: the Type and State cells are pinned to the row's right edge at a
;;! fixed width, the Name cell (indented by folder depth) takes the rest. The
;;! ImGui table's Kind, Size and Flags columns drop to the inspector — the package
;;! section already names the lock state, so a per-row Flags column would only
;;! repeat it — keeping the touch grid to three columns that fit the panel width.
(define kruddgui-asset-col-type 64)
(define kruddgui-asset-col-state 60)
(define kruddgui-asset-indent 16)

;;! (kruddgui-asset-grid-header L) the column-label row drawn once under an open
;;! package, its Type/State labels pinned to the same right edge as the leaf rows.
(define (kruddgui-asset-grid-header L)
  (let* ((x  (kruddgui-lay-x L))
	 (w  (kruddgui-lay-w L))
	 (y  (kruddgui-lay-cy L))
	 (h  kruddgui-scene-line)
	 (rx (+ x w))
	 (sx (- rx kruddgui-asset-col-state))
	 (tx (- sx kruddgui-asset-col-type)))
    (when (kruddgui-lay-vis? L h)
      (kruddgui-board-cell (+ x 8) y h "Name" kruddgui-scene-label-fg)
      (kruddgui-board-cell tx y h "Type" kruddgui-scene-label-fg)
      (kruddgui-board-cell sx y h "State" kruddgui-scene-label-fg))
    (kruddgui-lay-adv! L h)))

;;! (kruddgui-asset-row L row name) one leaf row: the Name / Type / State cells
;;! over a tappable full-width background; a tap drills into the asset's inspector.
;;! ROW is (id path type kind state size refs); NAME is the leaf path segment (the
;;! ancestor folders are already drawn as the enclosing folds). Only a tap drives
;;! it — no per-row drag region — so a package of many rows never nears
;;! KGUI_MAX_REGIONS.
(define (kruddgui-asset-row L row name)
  (let* ((id    (list-ref row 0))
	 (type  (list-ref row 2))
	 (state (list-ref row 4))
	 (x  (kruddgui-lay-x L))
	 (w  (kruddgui-lay-w L))
	 (y  (kruddgui-lay-cy L))
	 (h  kruddgui-scene-row-h)
	 (rx (+ x w))
	 (sx (- rx kruddgui-asset-col-state))
	 (tx (- sx kruddgui-asset-col-type)))
    (when (kruddgui-lay-vis? L h)
      (kruddgui-rect* (list x y w h) kruddgui-idle-bg)
      (kruddgui-board-cell (+ x 8) y h name kruddgui-idle-fg)
      (kruddgui-board-cell tx y h (kruddgui-asset-type-label type)
			   kruddgui-idle-fg)
      (kruddgui-board-cell sx y h (kruddgui-asset-state-label state)
			   kruddgui-idle-fg)
      (when (kgui-button x y w h)
	(set! kruddgui-assets-sel id)))
    (kruddgui-lay-adv! L (+ h kruddgui-scene-gap))))

;;! (kruddgui-asset-tree L entries prefix) draws one tree level: a nested
;;! kruddgui-fold per distinct first path segment (recursing into it when open),
;;! then a leaf row per entry already down to one segment — folders before leaves,
;;! VS-Code-Explorer-style. PREFIX threads the ancestor id so folder folds keyed by
;;! the same name under different parents stay distinct. Children draw indented by
;;! narrowing the cursor's x/w for the subtree — the right edge stays put, so the
;;! Type/State columns stay aligned across depths — restored after.
(define (kruddgui-asset-tree L entries prefix)
  (let ((folders (kruddgui-asset-group-by-head
		  (kruddgui-asset-entries-at-depth entries #t)))
	(leaves  (kruddgui-asset-entries-at-depth entries #f))
	(x       (kruddgui-lay-x L))
	(w       (kruddgui-lay-w L)))
    (for-each
     (lambda (bucket)
       (let* ((name     (car bucket))
	      (children (cdr bucket))
	      (id       (string-append prefix "/" name)))
	 (when (kruddgui-fold L id name #f)
	   (kruddgui-lay-set-x! L (+ x kruddgui-asset-indent))
	   (kruddgui-lay-set-w! L (- w kruddgui-asset-indent))
	   (kruddgui-asset-tree L children id)
	   (kruddgui-lay-set-x! L x)
	   (kruddgui-lay-set-w! L w))))
     folders)
    (for-each
     (lambda (e)
       (kruddgui-asset-row L (list-ref e 1) (car (list-ref e 0))))
     leaves)))

;;! (kruddgui-asset-package L name origin locked prefix rows open-default) one
;;! collapsible package section: a fold naming the package, its origin, lock state
;;! and (dynamic) asset count, over the folder tree its rows live in. Engine
;;! built-ins start collapsed (reference, not your working set), the project
;;! package open. The fold id pins to the prefix so the open state survives the
;;! changing count in the label; open-default only seeds the first frame.
(define (kruddgui-asset-package L name origin locked prefix rows open-default)
  (unless (null? rows)
    (when (kruddgui-fold L (string-append "pkg-" prefix)
			 (format #f "~A  (~A~A, ~D)" name origin
				 (if locked ", locked" "") (length rows))
			 open-default)
      (kruddgui-asset-grid-header L)
      (kruddgui-asset-tree L (kruddgui-asset-rows->entries rows) prefix))))

;;! (kruddgui-asset-type-picker L types sel) a segmented type-chip row: one chip
;;! per label, the selected index highlighted; returns the tapped index (a number,
;;! so 0 is a live result), or #f when none. The ref-combo kruddgui-scene-combo
;;! binds an asset id through a setter and always offers a "(none)" row — neither
;;! fits a fixed 0..N type choice — so the New Asset form gets this instead.
(define (kruddgui-asset-type-picker L types sel)
  (let* ((x   (kruddgui-lay-x L))
	 (w   (kruddgui-lay-w L))
	 (y   (kruddgui-lay-cy L))
	 (h   kruddgui-scene-row-h)
	 (n   (length types))
	 (g   kruddgui-scene-gap)
	 (cw  (if (> n 0) (/ (- w (* (- n 1) g)) n) w))
	 (hit #f))
    (when (and (> n 0) (kruddgui-lay-vis? L h))
      (let loop ((ls types) (i 0))
	(when (pair? ls)
	  (let ((cx     (+ x (* i (+ cw g))))
		(active (= i sel)))
	    (kruddgui-rect* (list cx y cw h)
			    (if active kruddgui-active-bg kruddgui-idle-bg))
	    (kruddgui-label cx y cw h (car ls)
			    (if active kruddgui-active-fg kruddgui-idle-fg))
	    (when (kgui-button cx y cw h)
	      (set! hit i)))
	  (loop (cdr ls) (+ i 1)))))
    (kruddgui-lay-adv! L (+ h kruddgui-scene-gap))
    hit))

;;! (kruddgui-asset-create-of-type type name) dispatch to the typed create
;;! primitive the New Asset type index selected — the Assets.scm twin.
(define (kruddgui-asset-create-of-type type name)
  (cond ((= type 1) (krudd-asset-create-shader name))
	((= type 2) (krudd-asset-create-material name))
	((= type 3) (krudd-asset-create-script name))
	((= type 4) (krudd-asset-create-mesh name))
	(else (krudd-asset-create-text name))))

;;! (kruddgui-asset-new-form L) the New Asset control: a "+ New Asset" button that
;;! opens into a name field, a type picker and a Create/Cancel row. Create (with a
;;! non-empty name) makes the asset through the typed primitive and drills into it;
;;! Cancel closes the form. Only drawn when the mutation api is present.
(define (kruddgui-asset-new-form L)
  (if (not kruddgui-assets-naming)
      (when (kruddgui-scene-btn L "+ New Asset" #t)
	(set! kruddgui-assets-naming #t)
	(set! kruddgui-assets-new-name "")
	(set! kruddgui-assets-new-type 0))
      (begin
	(kruddgui-scene-label L "Name")
	(let ((nf (kruddgui-scene-field L "asset-new-name"
					kruddgui-assets-new-name 0)))
	  (set! kruddgui-assets-new-name (car nf)))
	(kruddgui-scene-label L "Type")
	(let ((pick (kruddgui-asset-type-picker L kruddgui-assets-new-types
						kruddgui-assets-new-type)))
	  (when pick (set! kruddgui-assets-new-type pick)))
	(let ((act (kruddgui-button-row L (list "Create" "Cancel"))))
	  (cond
	   ((equal? act "Cancel")
	    (set! kruddgui-assets-naming #f))
	   ((and (equal? act "Create")
		 (not (string=? kruddgui-assets-new-name "")))
	    (let ((nid (kruddgui-asset-create-of-type
			kruddgui-assets-new-type kruddgui-assets-new-name)))
	      (unless (= nid 0) (set! kruddgui-assets-sel nid))
	      (set! kruddgui-assets-naming #f))))))))

;;! ------------------------------------------------------------------
;;! Asset inspector — per-type editors: the material editor (#492, PR6c)
;;! ------------------------------------------------------------------
;;! PR6b landed the browser and a read-only landing view for a tapped asset; 6c
;;! gives the inspector a per-type seam (kruddgui-asset-body, mirroring Assets.scm's
;;! kruddboard-draw-asset-body) and fills in the first real editor: the material
;;! editor. A material carries no schema of its own — it names a shader, and that
;;! shader's Material block (krudd-shader-material-params) is the parameter set the
;;! editor draws — so the editor is a shader picker, the derived parameters as
;;! sliders / swatches / numeric fields, an optional texture binding, and Save /
;;! Delete (or, for a read-only built-in, Clone). The source editors, markdown
;;! preview and image-baked previews stay on the ImGui Assets tab until their own
;;! primitives land (a multiline field, an md_draw->kgui_batch port, an image
;;! primitive); every other asset type still falls through to the generic view.

;;! Material editor model — the kruddgui twins of Assets.scm's material statics,
;;! keyed by the material id they were loaded for. A material has no fields of its
;;! own: -shader is the shader it names, -params the shader's Material descriptor
;;! list, -values the current value per field (in order). -texture / -tex-res are
;;! its optional texture binding (0 = none) and the square bake edge.
(define kruddgui-assets-mat-id 0)
(define kruddgui-assets-mat-shader 0)
(define kruddgui-assets-mat-params '())
(define kruddgui-assets-mat-values '())
(define kruddgui-assets-mat-texture 0)
(define kruddgui-assets-mat-tex-res 256)

;;! The bake resolutions the texture resolution combo offers — square, power-of-two
;;! (Assets.scm's kruddboard-assets-tex-resolutions).
(define kruddgui-assets-tex-resolutions '(64 128 256 512 1024 2048))

;;! Built-in Clone form state: the source id the default name was seeded from, the
;;! proposed name, and whether the last Clone hit a duplicate path. Mirrors
;;! Assets.scm's clone-src / clone-name / clone-conflict — only one inspector is
;;! open at a time, so a single set serves whichever built-in is being cloned.
(define kruddgui-assets-clone-src 0)
(define kruddgui-assets-clone-name "")
(define kruddgui-assets-clone-conflict #f)

;;! (kruddgui-assets-refresh-material) re-derives the parameter descriptors and
;;! current values for the loaded material against its selected shader — run on
;;! load and whenever the shader changes (krudd-material-values returns the shader's
;;! defaults when the material doesn't yet target it). The twin of Assets.scm's
;;! kruddboard-assets-refresh-material.
(define (kruddgui-assets-refresh-material)
  (set! kruddgui-assets-mat-params
	(krudd-shader-material-params kruddgui-assets-mat-shader))
  (set! kruddgui-assets-mat-values
	(krudd-material-values kruddgui-assets-mat-id
			       kruddgui-assets-mat-shader)))

;;! (kruddgui-assets-maybe-reload-material id) loads the material model when the
;;! selection changes: its stored shader-ref and texture binding, then the derived
;;! params/values. Keyed by id so an in-progress edit is never clobbered mid-frame,
;;! the twin of Assets.scm's kruddboard-assets-maybe-reload-material.
(define (kruddgui-assets-maybe-reload-material id)
  (unless (= kruddgui-assets-mat-id id)
    (set! kruddgui-assets-mat-id id)
    (set! kruddgui-assets-mat-shader (krudd-asset-shader-ref id))
    (let ((tx (krudd-material-texture id)))
      (if (pair? tx)
	  (begin (set! kruddgui-assets-mat-texture (car tx))
		 (set! kruddgui-assets-mat-tex-res (cadr tx)))
	  (begin (set! kruddgui-assets-mat-texture 0)
		 (set! kruddgui-assets-mat-tex-res 256))))
    (kruddgui-assets-refresh-material)))

;;! (kruddgui-assets-do-delete id) deletes the asset, clears the material model
;;! keyed to it, and returns to the browser — the twin of Assets.scm's
;;! kruddboard-assets-do-delete.
(define (kruddgui-assets-do-delete id)
  (krudd-asset-delete id)
  (set! kruddgui-assets-mat-id 0)
  (set! kruddgui-assets-sel 0))

;;! (kruddgui-assets-numeric-row L id label comps value) a labelled row of COMPS
;;! numeric fields packed across the row width — the fallback branch of the Assets
;;! param widget, for a field with neither a colour nor a range hint. Returns
;;! (values . changed?): the per-component values and whether any component
;;! committed. Culled rows return the values unchanged.
(define (kruddgui-assets-numeric-row L id label comps value)
  (kruddgui-scene-label L label)
  (let* ((x  (kruddgui-lay-x L))
	 (w  (kruddgui-lay-w L))
	 (y  (kruddgui-lay-cy L))
	 (g  kruddgui-scene-gap)
	 (fw (if (> comps 0) (/ (- w (* (- comps 1) g)) comps) w))
	 (res
	  (if (and (> comps 0) (kruddgui-lay-vis? L kruddgui-scene-row-h))
	      (let loop ((i 0) (vs value) (acc '()))
		(if (or (null? vs) (>= i comps))
		    (reverse acc)
		    (let ((cx (+ x (* i (+ fw g)))))
		      (loop (+ i 1) (cdr vs)
			    (cons (kruddgui-scene-numeric
				   cx y fw (format #f "~A~D" id i) (car vs))
				  acc)))))
	      (map (lambda (v) (cons v #f)) value))))
    (kruddgui-lay-adv! L (+ kruddgui-scene-row-h kruddgui-scene-gap))
    (cons (map car res)
	  (let anyc ((r res)) (cond ((null? r) #f)
				    ((cdr (car r)) #t)
				    (else (anyc (cdr r))))))))

;;! (kruddgui-assets-param-widget L id param value) draws one shader-derived
;;! parameter and returns its (values . changed?). Unlike the Scene tab's numeric-
;;! only kruddgui-scene-param-widget, this Assets variant routes on the field's edit
;;! hint the way Assets.scm's kruddboard-draw-material-param does: a "color" hint
;;! with three or more components becomes a draggable colour swatch, a "range" hint
;;! on a single component a draggable slider (its min / max are param fields 6 and
;;! 7), and anything else plain numeric fields. value stays a list so the save path
;;! packs every kind uniformly. param is (name type off size comps kind min max).
(define (kruddgui-assets-param-widget L id param value)
  (let ((name  (list-ref param 0))
	(comps (list-ref param 4))
	(kind  (list-ref param 5))
	(mn    (list-ref param 6))
	(mx    (list-ref param 7)))
    (cond
     ((and (string=? kind "color") (>= comps 3))
      (kruddgui-color-swatch L id name value))
     ((and (string=? kind "range") (= comps 1))
      (let ((r (kruddgui-slider L id name (car value) mn mx)))
	(cons (list (car r)) (cdr r))))
     (else
      (kruddgui-assets-numeric-row L id name comps value)))))

;;! (kruddgui-assets-material-params L) draws every material parameter widget in
;;! order and, if any committed this frame, writes the packed values back into the
;;! editor model. The write-back is what makes a slider drag or picker edit persist
;;! frame to frame; the asset itself is only touched by the explicit Save row below,
;;! unlike the Scene param menu's save-on-change (a material's shader / texture /
;;! resolution are coupled, so they batch behind one Save).
(define (kruddgui-assets-material-params L)
  (if (null? kruddgui-assets-mat-params)
      (kruddgui-scene-label L "(no material parameters)")
      (let* ((results
	      (map (lambda (p v pidx)
		     (kruddgui-assets-param-widget L
			 (format #f "mp-~D" pidx) p v))
		   kruddgui-assets-mat-params
		   kruddgui-assets-mat-values
		   (kruddgui-scene-iota (length kruddgui-assets-mat-params))))
	     (new-vals (map car results))
	     (changed  (let anyc ((r results))
			 (cond ((null? r) #f)
			       ((cdr (car r)) #t)
			       (else (anyc (cdr r)))))))
	(when changed
	  (set! kruddgui-assets-mat-values new-vals)))))

;;! (kruddgui-assets-material-texture L) the material's Texture binding: a texture-
;;! asset combo (with the shared combo's "(none)" unbind row) and, once a texture is
;;! bound, a square bake-resolution combo. Both edit the editor model the Save row
;;! packs into the material's texture trailer; ported from Assets.scm's
;;! kruddboard-draw-material-texture. The resolution combo reuses the same ref combo
;;! over (res . "N x N") options, its setter guarding the shared "(none)" row to a
;;! no-op since a bound texture always has a resolution.
(define (kruddgui-assets-material-texture L)
  (kruddgui-scene-combo L "mat-tex" "Texture"
      (kruddgui-binding-label #t kruddgui-assets-mat-texture)
      (krudd-texture-assets) kruddgui-assets-mat-texture #t
      (lambda (ignored aid) (set! kruddgui-assets-mat-texture aid))
      0)
  (when (> kruddgui-assets-mat-texture 0)
    (kruddgui-scene-combo L "mat-res" "Resolution"
	(format #f "~D x ~D" kruddgui-assets-mat-tex-res
		kruddgui-assets-mat-tex-res)
	(map (lambda (r) (cons r (format #f "~D x ~D" r r)))
	     kruddgui-assets-tex-resolutions)
	kruddgui-assets-mat-tex-res #t
	(lambda (ignored r)
	  (when (> r 0) (set! kruddgui-assets-mat-tex-res r)))
	0)))

;;! (kruddgui-asset-material-clone L id path) the read-only material's Clone row: a
;;! name field seeded "<path>_copy" on first view and a Clone button that packs the
;;! current shader / values / texture / resolution into a new authored material
;;! (krudd-asset-clone-material) and drills into it, or flags a name clash. Mirrors
;;! Assets.scm's kruddboard-draw-asset-material-clone.
(define (kruddgui-asset-material-clone L id path)
  (unless (= kruddgui-assets-clone-src id)
    (set! kruddgui-assets-clone-src id)
    (set! kruddgui-assets-clone-name
	  (string-append (kruddgui-strip-builtin-prefix path) "_copy"))
    (set! kruddgui-assets-clone-conflict #f))
  (kruddgui-scene-label L "Clone as")
  (let ((nf (kruddgui-scene-field L "mat-clone-name"
				  kruddgui-assets-clone-name 0)))
    (set! kruddgui-assets-clone-name (car nf)))
  (when (and (kruddgui-scene-btn L "Clone" #t)
	     (not (string=? kruddgui-assets-clone-name "")))
    (let ((nid (krudd-asset-clone-material
		kruddgui-assets-clone-name
		kruddgui-assets-mat-shader
		kruddgui-assets-mat-values
		kruddgui-assets-mat-texture
		kruddgui-assets-mat-tex-res
		kruddgui-assets-mat-tex-res)))
      (if (= nid 0)
	  (set! kruddgui-assets-clone-conflict #t)
	  (begin
	    (set! kruddgui-assets-clone-conflict #f)
	    (set! kruddgui-assets-sel nid)))))
  (when kruddgui-assets-clone-conflict
    (kruddgui-scene-label L
	(format #f "(\"~A\" already exists)" kruddgui-assets-clone-name))))

;;! (kruddgui-asset-material-editor L id path editable?) the material inspector: a
;;! Shader picker, the shader-derived Parameters, and the Texture binding — each
;;! under its own fold — then Save/Delete, or the built-in Clone row. Ported from
;;! Assets.scm's kruddboard-draw-asset-material-editor. kruddgui has no disable
;;! primitive, so a read-only built-in's shader / params / texture stay live for
;;! preview-editing; only Save is withheld (Clone captures whatever you set), where
;;! the ImGui editor greyed the controls out. Save is explicit — the coupled shader
;;! / texture / resolution batch behind one Save, not the Scene menu's save-on-change.
(define (kruddgui-asset-material-editor L id path editable?)
  (kruddgui-assets-maybe-reload-material id)
  (when (kruddgui-fold L "mat-shader-fold" "Shader" #t)
    (kruddgui-scene-combo L "mat-shader" "Shader"
	(kruddgui-binding-label #t kruddgui-assets-mat-shader)
	(krudd-shader-assets) kruddgui-assets-mat-shader #t
	(lambda (ignored aid)
	  (unless (= aid kruddgui-assets-mat-shader)
	    (set! kruddgui-assets-mat-shader aid)
	    (kruddgui-assets-refresh-material)))
	0))
  (when (kruddgui-fold L "mat-params-fold" "Parameters" #t)
    (kruddgui-assets-material-params L))
  (when (kruddgui-fold L "mat-tex-fold" "Texture" #t)
    (kruddgui-assets-material-texture L))
  (kruddgui-scene-rule L)
  (if editable?
      (let ((act (kruddgui-button-row L (list "Save" "Delete"))))
	(cond
	 ((equal? act "Save")
	  (krudd-asset-save-material id
	      kruddgui-assets-mat-shader kruddgui-assets-mat-values
	      kruddgui-assets-mat-texture kruddgui-assets-mat-tex-res
	      kruddgui-assets-mat-tex-res))
	 ((equal? act "Delete")
	  (kruddgui-assets-do-delete id))))
      (kruddgui-asset-material-clone L id path)))

;;! (kruddgui-asset-generic L info) the read-only catalog view — the browser's
;;! landing screen for any asset without a dedicated editor yet, and the tail of the
;;! type dispatch. The Type / Kind / State / Size / Refs / Read-only rows off
;;! (krudd-asset-info id).
(define (kruddgui-asset-generic L info)
  (kruddgui-scene-kv L "Type"
		     (kruddgui-asset-type-label (list-ref info 1)))
  (kruddgui-scene-kv L "Kind"
		     (kruddgui-asset-kind-label (list-ref info 2)))
  (kruddgui-scene-kv L "State"
		     (kruddgui-asset-state-label (list-ref info 3)))
  (kruddgui-scene-kv L "Size"
		     (let ((sz (list-ref info 4)))
		       (if (> sz 0) (number->string sz) "-")))
  (kruddgui-scene-kv L "Refs" (number->string (list-ref info 5)))
  (kruddgui-scene-kv L "Read-only"
		     (if (list-ref info 6) "yes" "no")))

;;! (kruddgui-asset-body L id info) dispatches to the per-type editor by asset type
;;! ahead of the generic catalog view — the twin of Assets.scm's
;;! kruddboard-draw-asset-body. ASSET_TYPE_MATERIAL = 3 (mirroring asset_api.h); 6c
;;! fills in the material branch, the remaining per-type editors slot in here as
;;! they land. read-only? (info field 6) drives the material editor's Save-vs-Clone
;;! row.
(define (kruddgui-asset-body L id info)
  (let ((type      (list-ref info 1))
	(read-only (list-ref info 6)))
    (cond
     ((= type 3)
      (kruddgui-asset-material-editor L id (list-ref info 0) (not read-only)))
     (else (kruddgui-asset-generic L info)))))

;;! (kruddgui-asset-inspector L id) the drilled-in inspector: a "< Back" row, the
;;! asset path, then the per-type body (kruddgui-asset-body) — the material editor
;;! for a material, the read-only catalog view otherwise. Back returns to the
;;! browser; a stale id (the asset deleted elsewhere) falls back to it too, the
;;! guard the ImGui inspector used. (krudd-asset-info id) -> (path type kind state
;;! size refs read-only? origin) or #f.
(define (kruddgui-asset-inspector L id)
  (let ((info (krudd-asset-info id)))
    (if (not info)
	(set! kruddgui-assets-sel 0)
	(begin
	  (when (kruddgui-scene-btn L "< Back" #t)
	    (set! kruddgui-assets-sel 0))
	  (kruddgui-scene-label L (list-ref info 0))
	  (kruddgui-scene-rule L)
	  (kruddgui-asset-body L id info)))))

;;! (kruddgui-asset-browser L) the package browser: the New Asset form (when the
;;! mutation api is present), then the engine and project package sections — or a
;;! placeholder when the asset api is absent or empty. (krudd-assets) ->
;;! (engine-rows project-rows) split by read_only, so the two groups already are
;;! the two packages.
(define (kruddgui-asset-browser L)
  (let ((groups (krudd-assets)))
    (if (not groups)
	(kruddgui-scene-label L "(assets unavailable)")
	(begin
	  (when (krudd-asset-mut?)
	    (kruddgui-asset-new-form L)
	    (kruddgui-scene-rule L))
	  (let ((engine  (car groups))
		(project (cadr groups)))
	    (if (and (null? engine) (null? project))
		(kruddgui-scene-label L "(no assets)")
		(begin
		  (kruddgui-asset-package L "krudd:engine" "engine" #t
					  "builtin" engine #f)
		  (kruddgui-asset-package L "pkg:project" "project" #f
					  "project" project #t))))))))

;;! (kruddgui-assets-body x y w h) the scrolling Assets body: the package browser,
;;! or the drilled-in asset inspector once one is selected. Scroll, clip and
;;! culling mirror the Scene body; the widget regions any 6c editor declares inside
;;! sit on top of this body's region.
(define (kruddgui-assets-body x y w h)
  (let* ((min-off (min 0.0 (- h kruddgui-assets-total)))
	 (off     (max min-off (min 0.0 kruddgui-assets-scroll)))
	 (ix      (+ x kruddgui-scene-pad))
	 (iw      (- w (* 2 kruddgui-scene-pad)))
	 (L       (kruddgui-lay (+ y off) y (+ y h) ix iw)))
    (set! kruddgui-assets-scroll off)
    (kruddgui-rect* (list x y w h) kruddgui-scene-body-bg)
    (kgui-clip x y w h)
    (if (= kruddgui-assets-sel 0)
	(kruddgui-asset-browser L)
	(kruddgui-asset-inspector L kruddgui-assets-sel))
    (kgui-clip-none)
    (set! kruddgui-assets-total (- (kruddgui-lay-cy L) (+ y off)))))

;;! (kruddgui-assets-draw-header x y w hdr) the title and the close box; the close
;;! tap is trapped by the console region so it never reaches ImGui.
(define (kruddgui-assets-draw-header x y w hdr)
  (let* ((bs 26)
	 (bx (- (+ x w) 8 bs))
	 (by (+ y (/ (- hdr bs) 2))))
    (kruddgui-label x y 120 hdr "ASSETS" kruddgui-idle-fg)
    (kruddgui-rect* (list bx by bs bs) kruddgui-idle-bg)
    (kruddgui-label bx by bs bs "x" kruddgui-idle-fg)
    (when (kgui-button bx by bs bs)
      (set! kruddgui-assets-open #f))))

;;! (kruddgui-assets-draw-panel vw vh) the expanded console: a top-left overlay
;;! stopping short of the mode-bar band, drawn last so it wins any overlap. A
;;! drag/wheel on the body region scrolls it (then re-clamped in the body).
(define (kruddgui-assets-draw-panel vw vh)
  (let* ((m      kruddgui-log-margin)
	 (avail  (- vh m (kruddgui-modebar-reserve vw vh) kruddgui-gap))
	 (w      (min kruddgui-assets-max-w (- vw (* 2 m))))
	 (h      (max 160.0 (min (* vh 0.72) avail)))
	 (x      m)
	 (y      m)
	 (hdr    kruddgui-assets-header-h)
	 (body-y (+ y hdr))
	 (body-h (- h hdr)))
    (kgui-panel-begin "kgui-assets" x y w h)
    (kruddgui-rect* (list x y w h) kruddgui-scene-panel-bg)
    (kruddgui-assets-draw-header x y w hdr)
    (set! kruddgui-assets-scroll
	  (+ kruddgui-assets-scroll
	     (cadr (kgui-region-drag))
	     (kgui-region-wheel)))
    (kruddgui-assets-body x body-y w body-h)
    (kgui-panel-end)))

;;! (kruddgui-assets-draw-handle vw vh) the collapsed console: a pill in the bottom-
;;! left stack, above the SCENE handle. Tap expands the console.
(define (kruddgui-assets-draw-handle vw vh)
  (let* ((m  kruddgui-log-margin)
	 (hw kruddgui-log-handle-w)
	 (hh kruddgui-log-handle-h)
	 (x  m)
	 (y  (- vh m (* 4 hh) (* 3 kruddgui-gap))))
    (kgui-panel-begin "kgui-assets" x y hw hh)
    (kruddgui-rect* (list x y hw hh) kruddgui-scene-panel-bg)
    (kruddgui-label x y hw hh "ASSETS" kruddgui-idle-fg)
    (when (kgui-button x y hw hh)
      (set! kruddgui-assets-open #t))
    (kgui-panel-end)))

;;! (kruddgui-draw) the whole layer — the host's per-tick entry point. Draw the
;;! mode-bar (row or column by orientation), then the Log console, the board
;;! console, the Scene inspector and the Assets console (each expanded or
;;! collapsed). Every panel owns its own input region; drawn in order, so a later
;;! panel wins any overlap.
(define (kruddgui-draw)
  (let* ((vp (kgui-viewport-size))
	 (vw (car vp))
	 (vh (cadr vp)))
    (when (and (> vw 0) (> vh 0))
      (when (>= (krudd-selected) 0)
	(if (>= vw vh)
	    (kruddgui-draw-row vw vh)
	    (kruddgui-draw-col vw vh)))
      (if kruddgui-log-open
	  (kruddgui-log-draw-panel vw vh)
	  (kruddgui-log-draw-handle vw vh))
      (if kruddgui-board-open
	  (kruddgui-board-draw-panel vw vh)
	  (kruddgui-board-draw-handle vw vh))
      (if kruddgui-scene-open
	  (kruddgui-scene-draw-panel vw vh)
	  (kruddgui-scene-draw-handle vw vh))
      (if kruddgui-assets-open
	  (kruddgui-assets-draw-panel vw vh)
	  (kruddgui-assets-draw-handle vw vh)))))
