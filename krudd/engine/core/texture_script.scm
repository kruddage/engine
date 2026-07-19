; SPDX-License-Identifier: GPL-2.0-or-later

;;! texture-script — the (texture NAME (shade (u v) ...)) form and the texture
;;! generator. Every texture in the engine is one of these forms; there is no
;;! hardcoded C texture generator, exactly as there is no hardcoded C mesh
;;! generator (see mesh_script.scm). Mirrors the (mesh ...) / (script ...)
;;! dispatchers.
;;!
;;! A texture script is one (texture NAME [(params ...)] (shade (u v) EXPR ...))
;;! S-expression. Its shade clause is a *pure function of the normalized texel
;;! coordinate* u,v in [0,1) and must evaluate to a pixel (list R G B A), each
;;! channel a real in [0,1] (a short list defaults the missing channels — RGB to
;;! 0, A to 1). Because shade never sees a resolution, the same script bakes at
;;! 256x256 or 2Kx2K: the host loop over x,y is the only thing that changes.
;;!
;;! The engine's texture-script driver (asset/texture_script.c) calls
;;! texture-script-generate with the source text of a bound ASSET_TYPE_TEXTURE
;;! asset, its resolved params, and a width/height, and marshals the returned
;;! byte-vector (width*height RGBA8 texels) into a texture_blob. Unlike a mesh
;;! (cheap, cached in the image) a texture bakes into a large buffer whose size
;;! is one of its inputs, so this layer does not cache: the render layer keys its
;;! GPU textures on (source, params, width, height) and only reaches here on a
;;! genuine miss, so the unbounded (source x params x size) space never
;;! accumulates in the image.

;;! --- pixel helpers ---------------------------------------------------
;;!
;;! A small shared vocabulary every shade clause can call, so authoring a
;;! pattern rarely means spelling out clamping and quantization by hand.

;;! Clamp a channel to the [0,1] the wire format stores.
(define (tex-clamp01 x)
  (if (real? x) (max 0.0 (min 1.0 x)) 0.0))

;;! Exact floor — shade clauses key checker/grid patterns on (tex-ifloor (* u n)),
;;! and even?/modulo need an integer, not the real (floor ...) returns.
(define (tex-ifloor x)
  (inexact->exact (floor x)))

;;! Linear blend of two channels (or two same-length pixel lists) by t in [0,1].
(define (tex-mix a b t)
  (if (pair? a)
      (map (lambda (x y) (tex-mix x y t)) a b)
      (+ (* a (- 1.0 t)) (* b t))))

;;! --- the (texture ...) form and driver -------------------------------

;;! The (texture NAME [(params ...)] (shade (u v) BODY ...)) form evaluates to
;;! the shade procedure — a two-argument (lambda (u v) BODY ...). NAME is a
;;! human-readable label only (like the mesh/entity DSLs); the render layer keys
;;! on source text, params, and size, not NAME. A (params ...) clause, when
;;! present, is authored data read separately (see texture-script-params); it is
;;! not a lifecycle clause, so the form ignores every clause but shade. The shade
;;! body reads its params through (param NAME) — the same reader a (mesh ...) or
;;! (script ...) clause uses.
(define-macro (texture name . clauses)
  (let ((s (assq 'shade clauses)))
    `(lambda ,(cadr s) ,@(cddr s))))

;;! (texture-script-params src) -> (TOTAL-SIZE (NAME TYPE OFFSET SIZE COMPONENTS
;;! EDIT-KIND EDIT-MIN EDIT-MAX DEFAULT) ...): a texture's authorable parameters,
;;! tight-packed, in the exact shape script-params / mesh-script-params report, so
;;! the one C marshaller (script_texture_params -> query_params) and one set of
;;! editor widgets serve textures too. Reuses the entity dispatcher's
;;! script-params-form since a (params ...) clause is identical wherever it
;;! appears.
(define (texture-script-params src)
  (catch #t
         (lambda ()
           (script-params-form (with-input-from-string src (lambda () (read)))))
         (lambda args (cons 0 '()))))

;;! Read pixel channel K (a real in [0,1]) from a shade result, or FALLBACK when
;;! the list is short or the channel is non-numeric — so a malformed pixel
;;! degrades to a sane colour rather than faulting the whole bake.
(define (tex-channel px k fallback)
  (if (and (pair? px) (< k (length px)))
      (let ((c (list-ref px k)))
        (if (real? c) c fallback))
      fallback))

;;! Quantize a [0,1] channel to a 0..255 byte (round-to-nearest).
(define (tex-quantize c)
  (inexact->exact (floor (+ 0.5 (* 255.0 (tex-clamp01 c))))))

;;! Parse and run SRC's shade procedure over a WIDTH x HEIGHT grid with PARAMS
;;! (an ((name . value) ...) alist) bound as the current (param ...) scope,
;;! returning a byte-vector of WIDTH*HEIGHT RGBA8 texels (row-major, top-left
;;! origin). shade is sampled at each texel *centre* (u = (x+0.5)/width), so the
;;! result is symmetric and independent of resolution. A fault (a malformed form,
;;! or a shade body that errors) is caught and logged, never taking the frame
;;! down, and degrades to #f — which the host marshals to "no texture". *params*
;;! is restored on both paths so a texture fault never leaks its params into a
;;! later mesh or entity-script call.
(define (texture-script-run src params width height)
  (set! *params* params)
  (let ((result
         (catch #t
                (lambda ()
                  (let ((shade (eval (with-input-from-string src
                                       (lambda () (read)))
                                     (rootlet)))
                        (buf (make-byte-vector (* width height 4) 0)))
                    (do ((y 0 (+ y 1)))
                        ((= y height) buf)
                      (do ((x 0 (+ x 1)))
                          ((= x width))
                        (let* ((u  (/ (+ x 0.5) width))
                               (v  (/ (+ y 0.5) height))
                               (px (shade u v))
                               (i  (* (+ (* y width) x) 4)))
                          (byte-vector-set! buf i       (tex-quantize (tex-channel px 0 0.0)))
                          (byte-vector-set! buf (+ i 1) (tex-quantize (tex-channel px 1 0.0)))
                          (byte-vector-set! buf (+ i 2) (tex-quantize (tex-channel px 2 0.0)))
                          (byte-vector-set! buf (+ i 3) (tex-quantize (tex-channel px 3 1.0))))))))
                (lambda args
                  (krudd-log 2 (string-append "texture-script: fault: " src))
                  #f))))
    (set! *params* '())
    result))

;;! (texture-script-generate src params width height) -> a byte-vector of
;;! width*height RGBA8 texels, or #f on fault / non-positive size. The one entry
;;! point the C driver calls; see texture-script-run for the sampling contract.
(define (texture-script-generate src params width height)
  (if (and (integer? width) (integer? height) (> width 0) (> height 0))
      (texture-script-run src params width height)
      #f))
