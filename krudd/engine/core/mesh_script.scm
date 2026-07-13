; SPDX-License-Identifier: GPL-2.0-or-later

;;! mesh-script — the (mesh NAME (generate () ...)) form, its geometry helper
;;! vocabulary, and the mesh generator. Every mesh in the engine — including
;;! the built-in box/sphere/plane/pyramid/grid, the revolved
;;! cylinder/cone/capsule/disc/torus, and the parametric superquadric/heightfield
;;! (see builtin_mesh_scripts.h) — is one of these forms; there is no hardcoded
;;! C mesh generator anymore.
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

;;! --- surfaces of revolution & parametric grids ----------------------
;;!
;;! Two shape "engines" that turn a compact description into (VERTS . INDICES),
;;! the way mesh-quad-verts turns a face into four vertices — the reusable core
;;! the swept and parametric built-ins are each one line over. mesh-revolve
;;! sweeps a meridian PROFILE around the Y axis (cylinder, cone, capsule, disc,
;;! and torus are each just a profile); mesh-param-surface samples a position
;;! function over a (u,v) grid, taking normals from the surface tangents
;;! (superquadric and heightfield are each just a function).

;;! A meridian profile point is (r y nr ny v): radius from the Y axis, height,
;;! the 2-D outward normal (nr ny) in the (radius,height) plane, and the V
;;! texcoord. Sweeping rotates (r,nr) into the XZ plane; a hard crease is two
;;! points at one (r,y) carrying different (nr,ny). mesh-arc-profile builds the
;;! common case — points along a circular arc — so a capsule cap or a torus tube
;;! stays a single call.

;;! (mesh-arc-profile cr cy rad a0 a1 n v0 v1) -> a list of (n+1) profile points
;;! along a circular arc of radius RAD centred at (CR,CY) in the meridian plane,
;;! sweeping angle A0..A1, with outward normals radial from that centre and V
;;! interpolating V0..V1. A hemisphere cap is a quarter turn; a torus tube a full
;;! one.
(define (mesh-arc-profile cr cy rad a0 a1 n v0 v1)
  (let loop ((i 0) (acc '()))
    (if (> i n)
        (reverse acc)
        (let* ((t (/ (exact->inexact i) n))
               (a (+ a0 (* (- a1 a0) t)))
               (ca (cos a)) (sa (sin a)))
          (loop (+ i 1)
                (cons (list (+ cr (* rad ca)) (+ cy (* rad sa)) ca sa
                            (+ v0 (* (- v1 v0) t)))
                      acc))))))

;;! (mesh-revolve-verts profile sectors) -> the swept vertex list: each of the
;;! (length profile) rings is SECTORS+1 vertices (the seam vertex is doubled so
;;! U runs a clean 0..1), row-major with profile order outermost.
(define (mesh-revolve-verts profile sectors)
  (let ploop ((rows profile) (acc '()))
    (if (null? rows)
        (reverse acc)
        (let* ((p (car rows))
               (r (car p)) (y (cadr p))
               (nr (caddr p)) (ny (list-ref p 3)) (vv (list-ref p 4)))
          (ploop (cdr rows)
                 (let sloop ((s 0) (a acc))
                   (if (> s sectors)
                       a
                       (let* ((phi (/ (* 2.0 pi s) sectors))
                              (cs (cos phi)) (sn (sin phi)))
                         (sloop (+ s 1)
                                (cons (list (* r cs) y (* r sn)
                                            (* nr cs) ny (* nr sn)
                                            (exact->inexact (/ s sectors)) vv)
                                      a))))))))))

;;! (mesh-revolve-indices profile sectors) -> the triangles matching
;;! mesh-revolve-verts. A profile segment whose two points share one (r,y) is a
;;! crease (a zero-area ring) and emits nothing, so a cylinder's or cone's cap
;;! rim costs no wasted triangles; every other segment is a quad strip. The row
;;! counter advances across creases too, since a crease still occupies a vertex
;;! row that later segments index against.
(define (mesh-revolve-indices profile sectors)
  (let rloop ((rows profile) (r 0) (acc '()))
    (if (null? (cdr rows))
        (reverse acc)
        (let* ((p0 (car rows)) (p1 (cadr rows))
               (crease (and (= (car p0) (car p1)) (= (cadr p0) (cadr p1)))))
          (rloop (cdr rows) (+ r 1)
                 (if crease
                     acc
                     (let sloop ((s 0) (a acc))
                       (if (= s sectors)
                           a
                           (let* ((k1 (+ (* r (+ sectors 1)) s))
                                  (k2 (+ k1 sectors 1)))
                             (sloop (+ s 1)
                                    (cons k2 (cons (+ k2 1) (cons k1
                                      (cons (+ k2 1)
                                        (cons (+ k1 1) (cons k1 a))))))))))))))))

;;! (mesh-revolve profile sectors) -> (VERTS . INDICES) for the surface swept by
;;! rotating PROFILE around the Y axis in SECTORS steps.
(define (mesh-revolve profile sectors)
  (cons (mesh-revolve-verts profile sectors)
        (mesh-revolve-indices profile sectors)))

;;! (mesh-grid-indices nu nv) -> triangles for an (nv+1)x(nu+1) vertex grid,
;;! row-major with columns fastest — the winding the built-in grid emits, shared
;;! by every parametric surface.
(define (mesh-grid-indices nu nv)
  (let rloop ((r 0) (acc '()))
    (if (= r nv)
        (reverse acc)
        (rloop (+ r 1)
               (let cloop ((c 0) (a acc))
                 (if (= c nu)
                     a
                     (let* ((k1 (+ (* r (+ nu 1)) c))
                            (k2 (+ k1 nu 1)))
                       (cloop (+ c 1)
                              (cons k2 (cons (+ k2 1) (cons k1
                                (cons (+ k2 1)
                                  (cons (+ k1 1) (cons k1 a))))))))))))))

;;! (mesh-param-surface f nu nv) -> (VERTS . INDICES) sampling F, a
;;! (u v)->(list x y z) position function with u,v in [0,1], over an
;;! (nu+1)x(nv+1) grid. Each normal is the cross product of the two surface
;;! tangents estimated by a one-sided finite difference (kept inside [0,1] near
;;! the edges), so an author supplies positions only; U,V ride along as uv0. A
;;! degenerate tangent (a pole, where a tangent collapses) falls back to the
;;! outward position ray so the normal stays unit length.
(define (mesh-param-surface f nu nv)
  (cons
    (let rloop ((r 0) (acc '()))
      (if (> r nv)
          (reverse acc)
          (rloop (+ r 1)
                 (let cloop ((c 0) (a acc))
                   (if (> c nu)
                       a
                       (let* ((u (/ (exact->inexact c) nu))
                              (v (/ (exact->inexact r) nv))
                              (h 1.0e-3)
                              (ub (if (> u 0.5) (- u h) u))
                              (uf (if (> u 0.5) u (+ u h)))
                              (vb (if (> v 0.5) (- v h) v))
                              (vf (if (> v 0.5) v (+ v h)))
                              (p  (f u v))
                              (pu (mesh-sub (f uf v) (f ub v)))
                              (pv (mesh-sub (f u vf) (f u vb)))
                              (cn (mesh-cross pv pu))
                              (cl (+ (* (car cn) (car cn))
                                     (* (cadr cn) (cadr cn))
                                     (* (caddr cn) (caddr cn))))
                              (n  (mesh-normalize (if (> cl 1.0e-12) cn p))))
                         (cloop (+ c 1)
                                (cons (list (car p) (cadr p) (caddr p)
                                            (car n) (cadr n) (caddr n) u v)
                                      a))))))))
    (mesh-grid-indices nu nv)))

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
  (catch #t
    (lambda ()
      (script-params-form (with-input-from-string src (lambda () (read)))))
    (lambda args (cons 0 '()))))

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
