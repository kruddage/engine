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

;; ------------------------------------------------------------------
;; Log console — the lifted kruddboard tab (#491)
;; ------------------------------------------------------------------

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
	   ((>= ry (+ y h)) #t)			   ; below the body: stop
	   ((<= (+ ry line-h) y)		   ; above the body: skip
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

;; ------------------------------------------------------------------
;; Board panel — the lifted KRUDD tab (#492): frame stats, startup, subsystems
;; ------------------------------------------------------------------

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

;;! (kruddgui-board-startup-rows) the one-time boot profile: init total, time to
;;! first frame, then a per-phase breakdown. (krudd-startup) -> (init-ms
;;! first-frame-ms (name . ms) ...) or #f when stats are absent.
(define (kruddgui-board-startup-rows)
  (let ((s (krudd-startup)))
    (if (not s)
	(list (list 'dim "(startup timings unavailable)"))
	(let ((init  (car s))
	      (first (cadr s))
	      (phases (cddr s)))
	  (append
	   (list (list 'kv "Init total" (format #f "~,1F ms" init))
		 (list 'kv "1st frame"  (format #f "~,1F ms" first)))
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
	   ((>= ry (+ y h)) #t)			   ; below the body: stop
	   ((<= (+ ry lh) y)			   ; above the body: skip
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

;;! (kruddgui-draw) the whole layer — the host's per-tick entry point. Draw the
;;! mode-bar (row or column by orientation), then the Log console and the board
;;! console (each expanded or collapsed). Four regions in all, each owning its
;;! own input; drawn in order, so a later panel wins any overlap.
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
	  (kruddgui-board-draw-handle vw vh)))))
