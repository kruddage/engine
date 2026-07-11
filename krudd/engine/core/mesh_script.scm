; SPDX-License-Identifier: GPL-2.0-or-later

;;! mesh-script — the (mesh NAME (generate () ...)) form, its geometry helper
;;! vocabulary, and the mesh generator. Every mesh in the engine — including
;;! the built-in cube/sphere/plane/pyramid/grid (see builtin_mesh_scripts.h)
;;! — is one of these forms; there is no hardcoded C mesh generator anymore.
;;! Mirrors entity_script.scm's (script NAME ...) dispatcher.
;;!
;;! A mesh script is one (mesh NAME [(params ...)] (generate () EXPR ...))
;;! S-expression. Its generate clause takes no arguments and must evaluate to
;;! (cons VERTS INDICES): VERTS a list of 8-element (px py pz nx ny nz u v)
;;! lists, INDICES a flat list of vertex-index integers, a multiple of 3 (one
;;! triangle per triple).
;;!
;;! An optional (params (NAME TYPE [(edit ...)] [(default ...)]) ...) clause —
;;! the same shape a (script ...) or shader material block declares — makes the
;;! geometry a function of authored inputs the generate body reads through
;;! (param NAME). Two entities can then share one mesh asset yet draw at
;;! different sizes, exactly as they share a material asset yet draw in
;;! different colors: the per-entity override lives in the world, the shape math
;;! lives here.
;;!
;;! The engine's mesh-script driver (asset/mesh_script.c) calls
;;! mesh-script-generate with the source text of a bound ASSET_TYPE_MESH asset
;;! and that entity's resolved params, and marshals the (VERTS . INDICES) result
;;! into a mesh_blob. A param-less mesh is a pure function of nothing — no clock,
;;! no entity, no params — so it is parsed AND generated once per exact source
;;! text, then cached: a render upload and an editor hit-test both asking for the
;;! same built-in cost nothing after the first. A parameterized mesh depends on
;;! its params, so it regenerates on demand (the render layer caches the GPU
;;! buffers per (source, params); see mesh-script-generate).

;;! --- geometry helpers ------------------------------------------------
;;!
;;! A small shared vocabulary every mesh script's generate clause can call —
;;! quad emission and basic vector math — so authoring a shape rarely means
;;! hand-listing vertices. Loaded once into the image, available to every
;;! mesh script (built-in or authored) the same way krudd-log is.

(define (mesh-normalize v)
  (let* ((x (car v)) (y (cadr v)) (z (caddr v))
         (len (sqrt (+ (* x x) (* y y) (* z z)))))
    (if (> len 1.0e-6) (list (/ x len) (/ y len) (/ z len)) v)))

(define (mesh-cross a b)
  (list (- (* (cadr a) (caddr b)) (* (caddr a) (cadr b)))
        (- (* (caddr a) (car b)) (* (car a) (caddr b)))
        (- (* (car a) (cadr b)) (* (cadr a) (car b)))))

(define (mesh-sub a b)
  (list (- (car a) (car b)) (- (cadr a) (cadr b)) (- (caddr a) (caddr b))))

;;! Corner sign pattern for mesh-quad-verts, CCW: (-,-), (+,-), (+,+), (-,+).
(define mesh-quad-corners '((-1.0 -1.0) (1.0 -1.0) (1.0 1.0) (-1.0 1.0)))

;;! (mesh-quad-verts n ax ay center) -> a list of 4 vertices for one quad face:
;;! outward normal N, in-plane axes AX/AY (unit vectors), offset CENTER along
;;! N (0.5 for a cube face, 0 for a flat plane). Pair with mesh-quad-indices
;;! (base) for the two triangles.
(define (mesh-quad-verts n ax ay center)
  (map (lambda (corner)
         (let* ((sx (car corner)) (sy (cadr corner))
                (px (+ (* center (car n))   (* 0.5 sx (car ax))   (* 0.5 sy (car ay))))
                (py (+ (* center (cadr n))  (* 0.5 sx (cadr ax))  (* 0.5 sy (cadr ay))))
                (pz (+ (* center (caddr n)) (* 0.5 sx (caddr ax)) (* 0.5 sy (caddr ay)))))
           (list px py pz (car n) (cadr n) (caddr n)
                 (* (+ sx 1.0) 0.5) (* (+ sy 1.0) 0.5))))
       mesh-quad-corners))

;;! (mesh-quad-indices base) -> the two CCW triangles for a quad whose 4
;;! vertices (from mesh-quad-verts) start at index BASE.
(define (mesh-quad-indices base)
  (list base (+ base 1) (+ base 2) base (+ base 2) (+ base 3)))

;;! (mesh-sphere-verts rings sectors) -> a UV-sphere's vertex list, radius
;;! 0.5, position/normal collinear (a unit sphere's normal is its position
;;! doubled). Pair with mesh-sphere-indices (same rings/sectors).
(define (mesh-sphere-verts rings sectors)
  (let rloop ((r 0) (acc '()))
      (if (> r rings)
          (reverse acc)
          (let* ((theta (/ (* pi r) rings))
                 (st (sin theta)) (ct (cos theta)))
            (rloop (+ r 1)
                   (let sloop ((s 0) (a acc))
                     (if (> s sectors)
                         a
                         (let* ((phi (/ (* 2.0 pi s) sectors))
                                (x (* st (cos phi)))
                                (y ct)
                                (z (* st (sin phi))))
                           (sloop (+ s 1)
                                  (cons (list (* 0.5 x) (* 0.5 y) (* 0.5 z)
                                              x y z
                                              (exact->inexact (/ s sectors))
                                              (exact->inexact (/ r rings)))
                                        a))))))))))

;;! (mesh-sphere-indices rings sectors) -> the index list matching
;;! mesh-sphere-verts's ring/sector layout (poles drop one triangle/sector).
(define (mesh-sphere-indices rings sectors)
  (let rloop ((r 0) (acc '()))
      (if (= r rings)
          (reverse acc)
          (rloop (+ r 1)
                 (let sloop ((s 0) (a acc))
                   (if (= s sectors)
                       a
                       (let* ((k1 (+ (* r (+ sectors 1)) s))
                              (k2 (+ k1 sectors 1))
                              (a1 (if (not (= r 0))
                                      (cons (+ k1 1) (cons k2 (cons k1 a)))
                                      a))
                              (a2 (if (not (= r (- rings 1)))
                                      (cons (+ k2 1) (cons k2 (cons (+ k1 1) a1)))
                                      a1)))
                         (sloop (+ s 1) a2))))))))

;;! --- the (mesh ...) form and driver ------------------------------------

(define *mesh-scripts* (make-hash-table))

;;! The (mesh NAME [(params ...)] (generate () BODY ...)) form evaluates to a
;;! zero-argument thunk over the generate clause's BODY. NAME is a human-readable
;;! label only, exactly like the entity script DSL's NAME — the registry below
;;! keys on source text (and params), not NAME, so a renamed clone never
;;! collides with the original it was copied from. A (params ...) clause, when
;;! present, is authored data read separately (see mesh-script-params); it is not
;;! a lifecycle clause, so the thunk ignores every clause but generate. The
;;! generate body may read those params through (param NAME) — the same reader a
;;! (script ...) clause uses — so one mesh source can draw at many sizes.
(define-macro (mesh name . clauses)
  (let ((g (assq 'generate clauses)))
    `(lambda () ,@(cddr g))))

;;! (mesh-script-params src) -> (TOTAL-SIZE (NAME TYPE OFFSET SIZE COMPONENTS
;;! EDIT-KIND EDIT-MIN EDIT-MAX DEFAULT) ...): a mesh's authorable parameters,
;;! tight-packed, in the exact shape script-params / shader-material-params
;;! report, so the one C marshaller (script_mesh_params -> query_params) and one
;;! set of editor widgets serve meshes too. Reuses the entity dispatcher's
;;! script-params-form (loaded before this image) since a (params ...) clause is
;;! identical wherever it appears.
(define (mesh-script-params src)
  (script-params-form (with-input-from-string src (lambda () (read)))))

;;! Parse and run SRC's generate thunk with PARAMS (an ((name . value) ...)
;;! alist) bound as the current (param ...) scope, returning (VERTS . INDICES).
;;! A fault (a malformed form, or a generate body that errors) is caught and
;;! logged, never taking the frame down, and degrades to an empty mesh. *params*
;;! is restored on both the success and fault paths so a mesh fault never leaks
;;! its params into a later entity-script tick.
(define (mesh-script-run src params)
  (set! *params* params)
  (let ((result
          (catch #t
            (lambda ()
              (let ((thunk (eval (with-input-from-string src (lambda () (read)))
                                 (rootlet))))
                (thunk)))
            (lambda args
              (krudd-log 2 (string-append "mesh-script: fault: " src))
              (cons '() '())))))
    (set! *params* '())
    result))

;;! (mesh-script-generate src params) -> (VERTS . INDICES). A param-less mesh
;;! (PARAMS '()) is a pure function of nothing, so its result is cached per exact
;;! source text — a render upload and an editor hit-test asking for the same
;;! built-in cost nothing after the first, exactly as before params existed. A
;;! parameterized mesh depends on PARAMS too, so it is regenerated on demand
;;! rather than cached here: the render layer keys its GPU buffers on
;;! (source, params) and only reaches this on a genuine cache miss, so the
;;! unbounded (source x params) space never accumulates in the image.
(define (mesh-script-generate src params)
  (if (null? params)
      (or (hash-table-ref *mesh-scripts* src)
          (let ((result (mesh-script-run src '())))
            (hash-table-set! *mesh-scripts* src result)
            result))
      (mesh-script-run src params)))
