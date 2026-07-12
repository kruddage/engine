; SPDX-License-Identifier: GPL-2.0-or-later

;;! kruddgui — the Scheme-authored panels for krudd's own UI layer.
;;!
;;! The C++ host (kruddgui.cpp) registers the kgui-* primitives against the
;;! shared s7 interpreter, loads this image once, then calls (kruddgui-draw)
;;! each tick after ImGui has rendered. A primitive only appends to the frame's
;;! quad batch or hit-tests the trapped pointer, so these procedures must run at
;;! draw time — never at load time.
;;!
;;! v0 (#490) is a single panel: a finger-sized MOVE / ROTATE / SCALE mode-bar
;;! wired to the shared gizmo tool via (krudd-gizmo-mode) / (krudd-set-gizmo-mode).
;;! It is drawn entirely with kruddgui's own quads and font-atlas text — no ImGui
;;! widgets — and it reflows between a bottom-centre row (landscape) and a
;;! bottom-right column (portrait).

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
;;! both registers the rect as part of the bar's hit footprint (so the host
;;! consumes taps there instead of forwarding them to ImGui) and reports a tap.
(define (kruddgui-button x y w h idx label)
  (let ((active (= (krudd-gizmo-mode) idx)))
    (kruddgui-rect* (list x y w h)
		    (if active kruddgui-active-bg kruddgui-idle-bg))
    (kruddgui-label x y w h label
		    (if active kruddgui-active-fg kruddgui-idle-fg))
    (when (kgui-button x y w h)
      (krudd-set-gizmo-mode idx))))

;;! (kruddgui-backing x y w h) draw the translucent panel behind the bar,
;;! inset by one gap on every side so the chips sit on a common ground.
(define (kruddgui-backing x y w h)
  (let ((p kruddgui-gap))
    (kruddgui-rect* (list (- x p) (- y p) (+ w (* 2 p)) (+ h (* 2 p)))
		    kruddgui-panel-bg)))

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
	 (y    (- vh m h)))
    (kruddgui-backing x0 y tot h)
    (let loop ((ms kruddgui-modes) (i 0))
      (when (pair? ms)
	(let ((x (+ x0 (* i (+ w g)))))
	  (kruddgui-button x y w h (caar ms) (cdar ms)))
	(loop (cdr ms) (+ i 1))))))

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
	 (y0  (- vh m tot)))
    (kruddgui-backing x y0 w tot)
    (let loop ((ms kruddgui-modes) (i 0))
      (when (pair? ms)
	(let ((y (+ y0 (* i (+ h g)))))
	  (kruddgui-button x y w h (caar ms) (cdar ms)))
	(loop (cdr ms) (+ i 1))))))

;;! (kruddgui-draw) the whole panel — the host's per-tick entry point. Pick the
;;! layout from the viewport's orientation and draw the mode-bar.
(define (kruddgui-draw)
  (let* ((vp (kgui-viewport-size))
	 (vw (car vp))
	 (vh (cadr vp)))
    (when (and (> vw 0) (> vh 0))
      (if (>= vw vh)
	  (kruddgui-draw-row vw vh)
	  (kruddgui-draw-col vw vh)))))
