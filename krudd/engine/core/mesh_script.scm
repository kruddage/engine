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

;;! --- signed distance fields & marching cubes ------------------------
;;!
;;! A third shape "engine", alongside mesh-revolve and mesh-param-surface: an
;;! implicit surface. Here a shape is a *field* — (lambda (x y z) -> signed
;;! distance to the surface), negative inside — and mesh-sdf marches a cubic grid
;;! over it to recover the (VERTS . INDICES) triangle mesh, the way
;;! mesh-param-surface samples an explicit position function. The win over a
;;! parametric patch: a field composes. Set-theoretic combinators (union =
;;! pointwise min, difference = max with a negated field, intersection = max) are
;;! constructive solid geometry for free, and their smooth variants blend two
;;! shapes into one organic surface no lathe or patch can express. Every helper
;;! here is a pure function of nothing but its arguments, so a field built from
;;! them is too — a param-less (mesh ...) over mesh-sdf caches once like any other.

;;! --- SDF primitives: each returns a field centred on the origin -------
;;! (r/half-extents in world units, matching the ±0.5 scale of the built-ins).

;;! (sdf-sphere r) -> the field of a sphere of radius R.
(define (sdf-sphere r)
  (lambda (x y z) (- (sqrt (+ (* x x) (* y y) (* z z))) r)))

;;! (sdf-box hx hy hz) -> the field of an axis-aligned box with the given half
;;! extents (the exact exterior distance, iq's formulation).
(define (sdf-box hx hy hz)
  (lambda (x y z)
    (let* ((qx (- (abs x) hx)) (qy (- (abs y) hy)) (qz (- (abs z) hz))
           (ox (max qx 0.0)) (oy (max qy 0.0)) (oz (max qz 0.0)))
      (+ (sqrt (+ (* ox ox) (* oy oy) (* oz oz)))
         (min (max qx (max qy qz)) 0.0)))))

;;! (sdf-round-box hx hy hz r) -> a box with edges rounded by radius R (the box
;;! field shrunk by R and then offset out by R — the canonical way to round any
;;! field, exposed on its own as sdf-round).
(define (sdf-round-box hx hy hz r)
  (sdf-round (sdf-box (- hx r) (- hy r) (- hz r)) r))

;;! (sdf-cylinder r h) -> a capped cylinder, radius R, half-height H, axis Y.
(define (sdf-cylinder r h)
  (lambda (x y z)
    (let* ((d  (- (sqrt (+ (* x x) (* z z))) r))
           (w  (- (abs y) h))
           (ox (max d 0.0)) (ow (max w 0.0)))
      (+ (min (max d w) 0.0)
         (sqrt (+ (* ox ox) (* ow ow)))))))

;;! (sdf-torus rr r) -> a torus of major radius RR (ring), minor radius R (tube),
;;! lying in the XZ plane, axis Y.
(define (sdf-torus rr r)
  (lambda (x y z)
    (let ((q (- (sqrt (+ (* x x) (* z z))) rr)))
      (- (sqrt (+ (* q q) (* y y))) r))))

;;! (sdf-plane nx ny nz d) -> the half-space below the plane with unit normal
;;! (NX NY NZ) at signed offset D (positive on the normal's side).
(define (sdf-plane nx ny nz d)
  (lambda (x y z) (- (+ (* x nx) (* y ny) (* z nz)) d)))

;;! --- field combinators: constructive solid geometry -----------------

;;! (sdf-union f ...) -> the union of any number of fields (pointwise min). With
;;! one field it is that field; with none, empty space.
(define (sdf-union . fs)
  (lambda (x y z)
    (let loop ((fs fs) (m #f))
      (if (null? fs)
          (or m 1.0e30)
          (let ((d ((car fs) x y z)))
            (loop (cdr fs) (if m (min m d) d)))))))

;;! (sdf-intersect f ...) -> the intersection of any number of fields (pointwise
;;! max).
(define (sdf-intersect . fs)
  (lambda (x y z)
    (let loop ((fs fs) (m #f))
      (if (null? fs)
          (or m -1.0e30)
          (let ((d ((car fs) x y z)))
            (loop (cdr fs) (if m (max m d) d)))))))

;;! (sdf-subtract a b) -> A with B carved out of it (max of A and the negated B).
(define (sdf-subtract a b)
  (lambda (x y z) (max (a x y z) (- (b x y z)))))

;;! (sdf-smooth-union a b k) -> A unioned with B, the seam rounded over a radius
;;! ~K into a smooth fillet (iq's polynomial smooth-min). K -> 0 recovers a hard
;;! sdf-union.
(define (sdf-smooth-union a b k)
  (lambda (x y z)
    (let* ((da (a x y z)) (db (b x y z))
           (h  (max 0.0 (min 1.0 (+ 0.5 (/ (* 0.5 (- db da)) k))))))
      (- (+ db (* h (- da db))) (* k h (- 1.0 h))))))

;;! (sdf-smooth-subtract a b k) -> B carved out of A with a smooth K-radius seam.
(define (sdf-smooth-subtract a b k)
  (lambda (x y z)
    (let* ((da (a x y z)) (db (b x y z))
           (h  (max 0.0 (min 1.0 (- 0.5 (/ (* 0.5 (+ db da)) k))))))
      (+ (+ da (* h (- (- db) da))) (* k h (- 1.0 h))))))

;;! (sdf-smooth-intersect a b k) -> the intersection of A and B with a smooth
;;! K-radius seam.
(define (sdf-smooth-intersect a b k)
  (lambda (x y z)
    (let* ((da (a x y z)) (db (b x y z))
           (h  (max 0.0 (min 1.0 (- 0.5 (/ (* 0.5 (- db da)) k))))))
      (+ (+ db (* h (- da db))) (* k h (- 1.0 h))))))

;;! --- field transforms -----------------------------------------------

;;! (sdf-translate f tx ty tz) -> F shifted by (TX TY TZ).
(define (sdf-translate f tx ty tz)
  (lambda (x y z) (f (- x tx) (- y ty) (- z tz))))

;;! (sdf-scale f s) -> F scaled uniformly by S about the origin (the field is
;;! rescaled with it so it stays a true distance).
(define (sdf-scale f s)
  (lambda (x y z) (* s (f (/ x s) (/ y s) (/ z s)))))

;;! (sdf-round f r) -> F grown outward by R, rounding convex edges by that radius.
(define (sdf-round f r)
  (lambda (x y z) (- (f x y z) r)))

;;! (sdf-normal f x y z) -> the unit outward normal of field F at (X Y Z), the
;;! normalized gradient estimated by central differences. A field's gradient
;;! points toward increasing distance — away from the solid — so this is the
;;! outward surface normal, and it is smooth across CSG seams the way a
;;! per-triangle face normal never is.
(define (sdf-normal f x y z)
  (let ((e 1.0e-4))
    (mesh-normalize
      (list (- (f (+ x e) y z) (f (- x e) y z))
            (- (f x (+ y e) z) (f x (- y e) z))
            (- (f x y (+ z e)) (f x y (- z e)))))))

;;! --- the marching-cubes lattice (Lorensen & Cline / Paul Bourke) -----
;;!
;;! Corner and edge numbering the triangle table is written against:
;;!   corner c at (dx dy dz) below; edge e joins corners (edge-a e)-(edge-b e).
;;! mc-tri-table maps a cube's 8-bit inside/outside code (bit c set when corner c
;;! is inside the solid, field < 0) to the list of edges its surface crosses,
;;! flat, three edges per triangle. This is the classic 256-entry table verbatim;
;;! mesh-sdf indexes it, no logic here to get wrong.
(define mc-corner-dx (vector 0 1 1 0 0 1 1 0))
(define mc-corner-dy (vector 0 0 0 0 1 1 1 1))
(define mc-corner-dz (vector 0 0 1 1 0 0 1 1))
(define mc-edge-a    (vector 0 1 2 3 4 5 6 7 0 1 2 3))
(define mc-edge-b    (vector 1 2 3 0 5 6 7 4 4 5 6 7))

(define mc-tri-table
  (vector
    '() '(0 8 3) '(0 1 9) '(1 8 3 9 8 1) '(1 2 10) '(0 8 3 1 2 10)
    '(9 2 10 0 2 9) '(2 8 3 2 10 8 10 9 8) '(3 11 2) '(0 11 2 8 11 0)
    '(1 9 0 2 3 11) '(1 11 2 1 9 11 9 8 11) '(3 10 1 11 10 3)
    '(0 10 1 0 8 10 8 11 10) '(3 9 0 3 11 9 11 10 9) '(9 8 10 10 8 11)
    '(4 7 8) '(4 3 0 7 3 4) '(0 1 9 8 4 7) '(4 1 9 4 7 1 7 3 1)
    '(1 2 10 8 4 7) '(3 4 7 3 0 4 1 2 10) '(9 2 10 9 0 2 8 4 7)
    '(2 10 9 2 9 7 2 7 3 7 9 4) '(8 4 7 3 11 2) '(11 4 7 11 2 4 2 0 4)
    '(9 0 1 8 4 7 2 3 11) '(4 7 11 9 4 11 9 11 2 9 2 1) '(3 10 1 3 11 10 7 8 4)
    '(1 11 10 1 4 11 1 0 4 7 11 4) '(4 7 8 9 0 11 9 11 10 11 0 3)
    '(4 7 11 4 11 9 9 11 10) '(9 5 4) '(9 5 4 0 8 3) '(0 5 4 1 5 0)
    '(8 5 4 8 3 5 3 1 5) '(1 2 10 9 5 4) '(3 0 8 1 2 10 4 9 5)
    '(5 2 10 5 4 2 4 0 2) '(2 10 5 3 2 5 3 5 4 3 4 8) '(9 5 4 2 3 11)
    '(0 11 2 0 8 11 4 9 5) '(0 5 4 0 1 5 2 3 11) '(2 1 5 2 5 8 2 8 11 4 8 5)
    '(10 3 11 10 1 3 9 5 4) '(4 9 5 0 8 1 8 10 1 8 11 10)
    '(5 4 0 5 0 11 5 11 10 11 0 3) '(5 4 8 5 8 10 10 8 11) '(9 7 8 5 7 9)
    '(9 3 0 9 5 3 5 7 3) '(0 7 8 0 1 7 1 5 7) '(1 5 3 3 5 7) '(9 7 8 9 5 7 10 1 2)
    '(10 1 2 9 5 0 5 3 0 5 7 3) '(8 0 2 8 2 5 8 5 7 10 5 2) '(2 10 5 2 5 3 3 5 7)
    '(7 9 5 7 8 9 3 11 2) '(9 5 7 9 7 2 9 2 0 2 7 11) '(2 3 11 0 1 8 1 7 8 1 5 7)
    '(11 2 1 11 1 7 7 1 5) '(9 5 8 8 5 7 10 1 3 10 3 11)
    '(5 7 0 5 0 9 7 11 0 1 0 10 11 10 0) '(11 10 0 11 0 3 10 5 0 8 0 7 5 7 0)
    '(11 10 5 7 11 5) '(10 6 5) '(0 8 3 5 10 6) '(9 0 1 5 10 6) '(1 8 3 1 9 8 5 10 6)
    '(1 6 5 2 6 1) '(1 6 5 1 2 6 3 0 8) '(9 6 5 9 0 6 0 2 6) '(5 9 8 5 8 2 5 2 6 3 2 8)
    '(2 3 11 10 6 5) '(11 0 8 11 2 0 10 6 5) '(0 1 9 2 3 11 5 10 6)
    '(5 10 6 1 9 2 9 11 2 9 8 11) '(6 3 11 6 5 3 5 1 3) '(0 8 11 0 11 5 0 5 1 5 11 6)
    '(3 11 6 0 3 6 0 6 5 0 5 9) '(6 5 9 6 9 11 11 9 8) '(5 10 6 4 7 8)
    '(4 3 0 4 7 3 6 5 10) '(1 9 0 5 10 6 8 4 7) '(10 6 5 1 9 7 1 7 3 7 9 4)
    '(6 1 2 6 5 1 4 7 8) '(1 2 5 5 2 6 3 0 4 3 4 7) '(8 4 7 9 0 5 0 6 5 0 2 6)
    '(7 3 9 7 9 4 3 2 9 5 9 6 2 6 9) '(3 11 2 7 8 4 10 6 5) '(5 10 6 4 7 2 4 2 0 2 7 11)
    '(0 1 9 4 7 8 2 3 11 5 10 6) '(9 2 1 9 11 2 9 4 11 7 11 4 5 10 6)
    '(8 4 7 3 11 5 3 5 1 5 11 6) '(5 1 11 5 11 6 1 0 11 7 11 4 0 4 11)
    '(0 5 9 0 6 5 0 3 6 11 6 3 8 4 7) '(6 5 9 6 9 11 4 7 9 7 11 9) '(10 4 9 6 4 10)
    '(4 10 6 4 9 10 0 8 3) '(10 0 1 10 6 0 6 4 0) '(8 3 1 8 1 6 8 6 4 6 1 10)
    '(1 4 9 1 2 4 2 6 4) '(3 0 8 1 2 9 2 4 9 2 6 4) '(0 2 4 4 2 6) '(8 3 2 8 2 4 4 2 6)
    '(10 4 9 10 6 4 11 2 3) '(0 8 2 2 8 11 4 9 10 4 10 6) '(3 11 2 0 1 6 0 6 4 6 1 10)
    '(6 4 1 6 1 10 4 8 1 2 1 11 8 11 1) '(9 6 4 9 3 6 9 1 3 11 6 3)
    '(8 11 1 8 1 0 11 6 1 9 1 4 6 4 1) '(3 11 6 3 6 0 0 6 4) '(6 4 8 11 6 8)
    '(7 10 6 7 8 10 8 9 10) '(0 7 3 0 10 7 0 9 10 6 7 10) '(10 6 7 1 10 7 1 7 8 1 8 0)
    '(10 6 7 10 7 1 1 7 3) '(1 2 6 1 6 8 1 8 9 8 6 7) '(2 6 9 2 9 1 6 7 9 0 9 3 7 3 9)
    '(7 8 0 7 0 6 6 0 2) '(7 3 2 6 7 2) '(2 3 11 10 6 8 10 8 9 8 6 7)
    '(2 0 7 2 7 11 0 9 7 6 7 10 9 10 7) '(1 8 0 1 7 8 1 10 7 6 7 10 2 3 11)
    '(11 2 1 11 1 7 10 6 1 6 7 1) '(8 9 6 8 6 7 9 1 6 11 6 3 1 3 6) '(0 9 1 11 6 7)
    '(7 8 0 7 0 6 3 11 0 11 6 0) '(7 11 6) '(7 6 11) '(3 0 8 11 7 6) '(0 1 9 11 7 6)
    '(8 1 9 8 3 1 11 7 6) '(10 1 2 6 11 7) '(1 2 10 3 0 8 6 11 7) '(2 9 0 2 10 9 6 11 7)
    '(6 11 7 2 10 3 10 8 3 10 9 8) '(7 2 3 6 2 7) '(7 0 8 7 6 0 6 2 0) '(2 7 6 2 3 7 0 1 9)
    '(1 6 2 1 8 6 1 9 8 8 7 6) '(10 7 6 10 1 7 1 3 7) '(10 7 6 1 7 10 1 8 7 1 0 8)
    '(0 3 7 0 7 10 0 10 9 6 10 7) '(7 6 10 7 10 8 8 10 9) '(6 8 4 11 8 6) '(3 6 11 3 0 6 0 4 6)
    '(8 6 11 8 4 6 9 0 1) '(9 4 6 9 6 3 9 3 1 11 3 6) '(6 8 4 6 11 8 2 10 1)
    '(1 2 10 3 0 11 0 6 11 0 4 6) '(4 11 8 4 6 11 0 2 9 2 10 9)
    '(10 9 3 10 3 2 9 4 3 11 3 6 4 6 3) '(8 2 3 8 4 2 4 6 2) '(0 4 2 4 6 2)
    '(1 9 0 2 3 4 2 4 6 4 3 8) '(1 9 4 1 4 2 2 4 6) '(8 1 3 8 6 1 8 4 6 6 10 1)
    '(10 1 0 10 0 6 6 0 4) '(4 6 3 4 3 8 6 10 3 0 3 9 10 9 3) '(10 9 4 6 10 4)
    '(4 9 5 7 6 11) '(0 8 3 4 9 5 11 7 6) '(5 0 1 5 4 0 7 6 11) '(11 7 6 8 3 4 3 5 4 3 1 5)
    '(9 5 4 10 1 2 7 6 11) '(6 11 7 1 2 10 0 8 3 4 9 5) '(7 6 11 5 4 10 4 2 10 4 0 2)
    '(3 4 8 3 5 4 3 2 5 10 5 2 11 7 6) '(7 2 3 7 6 2 5 4 9) '(9 5 4 0 8 6 0 6 2 6 8 7)
    '(3 6 2 3 7 6 1 5 0 5 4 0) '(6 2 8 6 8 7 2 1 8 4 8 5 1 5 8) '(9 5 4 10 1 6 1 7 6 1 3 7)
    '(1 6 10 1 7 6 1 0 7 8 7 0 9 5 4) '(4 0 10 4 10 5 0 3 10 6 10 7 3 7 10)
    '(7 6 10 7 10 8 5 4 10 4 8 10) '(6 9 5 6 11 9 11 8 9) '(3 6 11 0 6 3 0 5 6 0 9 5)
    '(0 11 8 0 5 11 0 1 5 5 6 11) '(6 11 3 6 3 5 5 3 1) '(1 2 10 9 5 11 9 11 8 11 5 6)
    '(0 11 3 0 6 11 0 9 6 5 6 9 1 2 10) '(11 8 5 11 5 6 8 0 5 10 5 2 0 2 5)
    '(6 11 3 6 3 5 2 10 3 10 5 3) '(5 8 9 5 2 8 5 6 2 3 8 2) '(9 5 6 9 6 0 0 6 2)
    '(1 5 8 1 8 0 5 6 8 3 8 2 6 2 8) '(1 5 6 2 1 6) '(1 3 6 1 6 10 3 8 6 5 6 9 8 9 6)
    '(10 1 0 10 0 6 9 5 0 5 6 0) '(0 3 8 5 6 10) '(10 5 6) '(11 5 10 7 5 11)
    '(11 5 10 11 7 5 8 3 0) '(5 11 7 5 10 11 1 9 0) '(10 7 5 10 11 7 9 8 1 8 3 1)
    '(11 1 2 11 7 1 7 5 1) '(0 8 3 1 2 7 1 7 5 7 2 11) '(9 7 5 9 2 7 9 0 2 2 11 7)
    '(7 5 2 7 2 11 5 9 2 3 2 8 9 8 2) '(2 5 10 2 3 5 3 7 5) '(8 2 0 8 5 2 8 7 5 10 2 5)
    '(9 0 1 5 10 3 5 3 7 3 10 2) '(9 8 2 9 2 1 8 7 2 10 2 5 7 5 2) '(1 3 5 3 7 5)
    '(0 8 7 0 7 1 1 7 5) '(9 0 3 9 3 5 5 3 7) '(9 8 7 5 9 7) '(5 8 4 5 10 8 10 11 8)
    '(5 0 4 5 11 0 5 10 11 11 3 0) '(0 1 9 8 4 10 8 10 11 10 4 5) '(10 11 4 10 4 5 11 3 4 9 4 1 3 1 4)
    '(2 5 1 2 8 5 2 11 8 4 5 8) '(0 4 11 0 11 3 4 5 11 2 11 1 5 1 11)
    '(0 2 5 0 5 9 2 11 5 4 5 8 11 8 5) '(9 4 5 2 11 3) '(2 5 10 3 5 2 3 4 5 3 8 4)
    '(5 10 2 5 2 4 4 2 0) '(3 10 2 3 5 10 3 8 5 4 5 8 0 1 9) '(5 10 2 5 2 4 1 9 2 9 4 2)
    '(8 4 5 8 5 3 3 5 1) '(0 4 5 1 0 5) '(8 4 5 8 5 3 9 0 5 0 3 5) '(9 4 5)
    '(4 11 7 4 9 11 9 10 11) '(0 8 3 4 9 7 9 11 7 9 10 11) '(1 10 11 1 11 4 1 4 0 7 4 11)
    '(3 1 4 3 4 8 1 10 4 7 4 11 10 11 4) '(4 11 7 9 11 4 9 2 11 9 1 2)
    '(9 7 4 9 11 7 9 1 11 2 11 1 0 8 3) '(11 7 4 11 4 2 2 4 0) '(11 7 4 11 4 2 8 3 4 3 2 4)
    '(2 9 10 2 7 9 2 3 7 7 4 9) '(9 10 7 9 7 4 10 2 7 8 7 0 2 0 7)
    '(3 7 10 3 10 2 7 4 10 1 10 0 4 0 10) '(1 10 2 8 7 4) '(4 9 1 4 1 7 7 1 3)
    '(4 9 1 4 1 7 0 8 1 8 7 1) '(4 0 3 7 4 3) '(4 8 7) '(9 10 8 10 11 8)
    '(3 0 9 3 9 11 11 9 10) '(0 1 10 0 10 8 8 10 11) '(3 1 10 11 3 10) '(1 2 11 1 11 9 9 11 8)
    '(3 0 9 3 9 11 1 2 9 2 11 9) '(0 2 11 8 0 11) '(3 2 11) '(2 3 8 2 8 10 10 8 9)
    '(9 10 2 0 9 2) '(2 3 8 2 8 10 0 1 8 1 10 8) '(1 10 2) '(1 3 8 9 1 8) '(0 9 1) '(0 3 8) '()))

;;! (mesh-sdf f res lo hi) -> (VERTS . INDICES) for the isosurface F = 0, marched
;;! over a RES x RES x RES cell grid spanning the axis-aligned box from LO to HI
;;! (each a (x y z) list; choose them a cell or two larger than the shape so it is
;;! fully enclosed and the mesh closes). Each surface-crossing edge yields one
;;! vertex, positioned by linear interpolation to where the field hits zero and
;;! shaded by the field's own gradient normal (sdf-normal), with a cylindrical uv
;;! (angle around Y, height up the box). Edge vertices are shared between the
;;! cells that touch them — keyed on the grid edge — so the surface is a closed,
;;! welded mesh, not a triangle soup. UINT16 indices cap a single bake at 65535
;;! vertices, which a RES up to ~64 stays well under for a compact shape.
(define (mesh-sdf f res lo hi)
  (let* ((lx (car lo)) (ly (cadr lo)) (lz (caddr lo))
         (hx (car hi)) (hy (cadr hi)) (hz (caddr hi))
         (dx (/ (- hx lx) res)) (dy (/ (- hy ly) res)) (dz (/ (- hz lz) res))
         (np1  (+ res 1))
         (big  (* np1 np1 np1))
         (gid  (lambda (i j k) (+ (* (+ (* i np1) j) np1) k)))
         (wx   (lambda (i) (+ lx (* i dx))))
         (wy   (lambda (j) (+ ly (* j dy))))
         (wz   (lambda (k) (+ lz (* k dz))))
         (vals (make-vector big 0.0))
         (cache (make-hash-table))
         (verts '()) (nverts 0)
         (indices '()))
    ;;! Sample the field once per grid point; every cube reuses its 8 corners.
    (do ((i 0 (+ i 1))) ((> i res))
      (do ((j 0 (+ j 1))) ((> j res))
        (do ((k 0 (+ k 1))) ((> k res))
          (vector-set! vals (gid i j k) (f (wx i) (wy j) (wz k))))))
    ;;! The vertex on edge E of the cube at (i j k): interpolated where the field
    ;;! crosses zero along it, created once and cached by the edge's endpoints so
    ;;! adjacent cubes weld to one shared vertex.
    (define (edge-vertex i j k e)
      (let* ((a  (vector-ref mc-edge-a e)) (b (vector-ref mc-edge-b e))
             (ai (+ i (vector-ref mc-corner-dx a)))
             (aj (+ j (vector-ref mc-corner-dy a)))
             (ak (+ k (vector-ref mc-corner-dz a)))
             (bi (+ i (vector-ref mc-corner-dx b)))
             (bj (+ j (vector-ref mc-corner-dy b)))
             (bk (+ k (vector-ref mc-corner-dz b)))
             (ida (gid ai aj ak)) (idb (gid bi bj bk))
             (key (if (< ida idb) (+ (* ida big) idb) (+ (* idb big) ida)))
             (hit (hash-table-ref cache key)))
        (or hit
            (let* ((va (vector-ref vals ida)) (vb (vector-ref vals idb))
                   (den (- va vb))
                   (t   (if (> (abs den) 1.0e-12) (/ va den) 0.5))
                   (px (+ (wx ai) (* t (- (wx bi) (wx ai)))))
                   (py (+ (wy aj) (* t (- (wy bj) (wy aj)))))
                   (pz (+ (wz ak) (* t (- (wz bk) (wz ak)))))
                   (n  (sdf-normal f px py pz))
                   (u  (+ 0.5 (/ (atan pz px) (* 2.0 pi))))
                   (vv (/ (- py ly) (if (> (- hy ly) 1.0e-6) (- hy ly) 1.0)))
                   (idx nverts))
              (hash-table-set! cache key idx)
              (set! verts (cons (list px py pz (car n) (cadr n) (caddr n) u vv)
                                verts))
              (set! nverts (+ nverts 1))
              idx))))
    ;;! March: each cube's inside/outside code selects its triangles from the
    ;;! table; every triple of edges is one triangle, wound so the sdf-normal
    ;;! faces out (CCW front).
    (do ((i 0 (+ i 1))) ((= i res))
      (do ((j 0 (+ j 1))) ((= j res))
        (do ((k 0 (+ k 1))) ((= k res))
          (let ((code 0))
            (do ((c 0 (+ c 1))) ((= c 8))
              (when (< (vector-ref vals (gid (+ i (vector-ref mc-corner-dx c))
                                             (+ j (vector-ref mc-corner-dy c))
                                             (+ k (vector-ref mc-corner-dz c))))
                       0.0)
                (set! code (+ code (ash 1 c)))))
            (let loop ((es (vector-ref mc-tri-table code)))
              (when (pair? es)
                (let ((v0 (edge-vertex i j k (car es)))
                      (v1 (edge-vertex i j k (cadr es)))
                      (v2 (edge-vertex i j k (caddr es))))
                  (set! indices (cons v2 (cons v1 (cons v0 indices))))
                  (loop (cdddr es)))))))))
    (cons (reverse verts) (reverse indices))))

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
