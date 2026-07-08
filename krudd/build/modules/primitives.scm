; SPDX-License-Identifier: GPL-2.0-or-later
;; scm-lint:off
;
; primitives.scm — the engine's built-in procedural geometry, in Scheme.
;
; This is a strangler-fig port of modules/asset/primitives.c: the same four
; unit primitives (cube, sphere, plane, pyramid), generated from the same math,
; but authored in Scheme and run inside the s7 runtime the engine boots (see
; modules/core/script.c). It lives outside the ninja build tree, under
; krudd/build/modules/, as a build-owned Scheme module, and — like md_parse.scm
; — is the single ABI artifact in git: the C ABI declaration below drives
; krudd's binding generator, which emits primitives_gen.h (the struct + the two
; export prototypes) and primitives.scm.c (this module baked to a byte array,
; the marshalers, and the driver). A small hand-written primitives_blob.c packs
; the marshaled vertex/index arrays into a mesh_blob via mesh.h — the byte
; layout and allocation stay in C, where they belong; only the geometry moves.
;
; A mesh is delivered as two arrays. Rather than marshal an opaque mesh_blob out
; of Scheme, the ABI exposes them as two exports over the same primitive-kind
; enum: primitive-vertices returns the interleaved vertex records and
; primitive-indices the uint16 triangle indices. Kinds mirror enum
; primitive_kind / the builtin_paths table (cube 0, sphere 1, plane 2,
; pyramid 3); a bad kind yields empty lists, which the driver reports as NULL.
;
; Faithfulness note: primitives.c computes in native float (sinf/cosf/sqrtf);
; s7 computes in double, narrowed to float when marshaled. Cube and plane are
; exact half-integers and come out bit-identical; the sphere's transcendentals
; and the pyramid's normalize agree only to within float epsilon. So the port is
; proven against the C reference by the geometric invariants both must satisfy
; (exact counts/indices, unit bounds, radius, unit normals) rather than by
; byte-for-byte equality — see primitive_test.c, run against both backends.
;; scm-lint:on

;; scm-lint:off
;; ---------------------------------------------------------------------------
;; C ABI declaration.
;;
;; krudd reads these forms at synthesis time — structurally, without evaluating
;; them — and generates primitives_gen.h and the primitives.scm.c shim. The two
;; macros expand them to #f when the module is loaded into s7, so the exact same
;; text is both the runtime image the shim bakes in and the ABI the generator
;; reads. The binding vocabulary lives in krudd/build/introspect.scm.
;; ---------------------------------------------------------------------------
;; scm-lint:on

;; scm-lint:off
;; Upper bounds on a single primitive's arrays — the sphere (561 verts / 2880
;; indices) is the largest. Emitted as PRIM_VERTS_MAX / PRIM_INDICES_MAX for the
;; driver's temp buffers.
;; scm-lint:on
(define prim-verts-max 561)
(define prim-indices-max 2880)

(define-macro (define-c-struct . _) #f)
(define-macro (define-c-export . _) #f)

;; scm-lint:off
;; The interleaved vertex the built-in meshes advertise: position, normal, uv0.
;; Flat scalar fields, laid out identically to mesh.h's struct mesh_vertex
;; (8 contiguous floats, 32-byte stride), so primitives_blob.c copies the
;; marshaled array straight into the blob.
;; scm-lint:on
(define-c-struct prim-vertex
  (px f32) (py f32) (pz f32)
  (nx f32) (ny f32) (nz f32)
  (u  f32) (w  f32))

(define-c-export (primitive-vertices (kind i32) -> (vector prim-vertex max))
  (calls primitive-vertices))

(define-c-export (primitive-indices (kind i32) -> (vector u16 max))
  (calls primitive-indices))

;; scm-lint:off
;; ---------------------------------------------------------------------------
;; Geometry. A vertex is (px py pz nx ny nz u w) — the field order above; an
;; index list is plain integers. Each generator returns (cons vertices indices).
;; ---------------------------------------------------------------------------
;; scm-lint:on

(define (v3 a b c) (list a b c))

;; scm-lint:off
;; Corner sign pattern, CCW: (-,-), (+,-), (+,+), (-,+) — matches CORNER[][].
;; scm-lint:on
(define prim-corners '((-1.0 -1.0) (1.0 -1.0) (1.0 1.0) (-1.0 1.0)))

;; scm-lint:off
;; One axis-aligned quad face -> its four vertices. FACE is (normal ax ay), each
;; a unit 3-vector; CENTER offsets the quad along its normal (0.5 for a cube
;; face, 0 for a flat plane, -0.5 for the pyramid base). Mirrors emit_quad.
;; scm-lint:on
(define (prim-quad-verts face center)
  (let ((n (car face)) (ax (cadr face)) (ay (caddr face)))
    (map (lambda (corner)
           (let* ((sx (car corner)) (sy (cadr corner))
                  (px (+ (* center (car n))
                         (* 0.5 sx (car ax)) (* 0.5 sy (car ay))))
                  (py (+ (* center (cadr n))
                         (* 0.5 sx (cadr ax)) (* 0.5 sy (cadr ay))))
                  (pz (+ (* center (caddr n))
                         (* 0.5 sx (caddr ax)) (* 0.5 sy (caddr ay)))))
             (list px py pz (car n) (cadr n) (caddr n)
                   (* (+ sx 1.0) 0.5) (* (+ sy 1.0) 0.5))))
         prim-corners)))

;; scm-lint:off
;; The two triangles of a quad whose four vertices start at BASE.
;; scm-lint:on
(define (prim-quad-indices base)
  (list base (+ base 1) (+ base 2) base (+ base 2) (+ base 3)))

;; scm-lint:off
;; --- Cube: 6 quad faces, 24 verts / 36 indices ---------------------------
;; scm-lint:on
(define prim-cube-faces
  (list
    (list (v3  1.0 0.0 0.0) (v3 0.0 0.0 1.0) (v3 0.0 1.0 0.0))
    (list (v3 -1.0 0.0 0.0) (v3 0.0 0.0 1.0) (v3 0.0 1.0 0.0))
    (list (v3 0.0  1.0 0.0) (v3 1.0 0.0 0.0) (v3 0.0 0.0 1.0))
    (list (v3 0.0 -1.0 0.0) (v3 1.0 0.0 0.0) (v3 0.0 0.0 1.0))
    (list (v3 0.0 0.0  1.0) (v3 1.0 0.0 0.0) (v3 0.0 1.0 0.0))
    (list (v3 0.0 0.0 -1.0) (v3 1.0 0.0 0.0) (v3 0.0 1.0 0.0))))

(define (prim-cube)
  (let loop ((f 0) (verts '()) (indices '()))
    (if (= f 6)
        (cons verts indices)
        (loop (+ f 1)
              (append verts
                      (prim-quad-verts (list-ref prim-cube-faces f) 0.5))
              (append indices (prim-quad-indices (* f 4)))))))

;; scm-lint:off
;; --- Plane: one quad on the XZ plane, 4 verts / 6 indices ----------------
;; scm-lint:on
(define (prim-plane)
  (let ((top (list (v3 0.0 1.0 0.0) (v3 1.0 0.0 0.0) (v3 0.0 0.0 1.0))))
    (cons (prim-quad-verts top 0.0)
          (prim-quad-indices 0))))

;; scm-lint:off
;; --- Pyramid: square base + 4 triangular sides, 16 verts / 18 indices ----
;; scm-lint:on
(define (prim-normalize v)
  (let* ((x (car v)) (y (cadr v)) (z (caddr v))
         (len (sqrt (+ (* x x) (* y y) (* z z)))))
    (if (> len 1.0e-6)
        (list (/ x len) (/ y len) (/ z len))
        v)))

(define (prim-cross a b)
  (list (- (* (cadr a) (caddr b)) (* (caddr a) (cadr b)))
        (- (* (caddr a) (car b)) (* (car a) (caddr b)))
        (- (* (car a) (cadr b)) (* (cadr a) (car b)))))

(define (prim-sub a b)
  (list (- (car a) (car b)) (- (cadr a) (cadr b)) (- (caddr a) (caddr b))))

;; scm-lint:off
;; Base corners (y = -0.5), CCW seen from below; apex at the top.
;; scm-lint:on
(define prim-pyr-base
  (list (v3 -0.5 -0.5 -0.5) (v3 0.5 -0.5 -0.5)
        (v3 0.5 -0.5 0.5)   (v3 -0.5 -0.5 0.5)))
(define prim-pyr-apex (v3 0.0 0.5 0.0))

(define (prim-pyramid)
  (let* ((base-face (list (v3 0.0 -1.0 0.0) (v3 1.0 0.0 0.0) (v3 0.0 0.0 1.0)))
         (base-verts (prim-quad-verts base-face -0.5))
         (base-idx   (prim-quad-indices 0)))
    (let loop ((s 0) (verts base-verts) (indices base-idx))
      (if (= s 4)
          (cons verts indices)
          (let* ((p0 (list-ref prim-pyr-base s))
                 (p1 (list-ref prim-pyr-base (modulo (+ s 1) 4)))
                 (vb (+ 4 (* s 3)))
                 (e0 (prim-sub p1 p0))
                 (e1 (prim-sub prim-pyr-apex p0))
                 (n  (prim-normalize (prim-cross e0 e1))))
            (loop (+ s 1)
                  (append verts
                          (list (list (car p0) (cadr p0) (caddr p0)
                                      (car n) (cadr n) (caddr n) 0.0 0.0)
                                (list (car p1) (cadr p1) (caddr p1)
                                      (car n) (cadr n) (caddr n) 1.0 0.0)
                                (list (car prim-pyr-apex) (cadr prim-pyr-apex)
                                      (caddr prim-pyr-apex)
                                      (car n) (cadr n) (caddr n) 0.5 1.0)))
                  (append indices (list vb (+ vb 1) (+ vb 2)))))))))

;; scm-lint:off
;; --- Sphere: UV sphere, 561 verts / 2880 indices -------------------------
;; scm-lint:on
(define prim-sphere-rings   16)
(define prim-sphere-sectors 32)

(define (prim-sphere-verts)
  (let ((rings prim-sphere-rings) (sectors prim-sphere-sectors))
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
                                        a)))))))))))

(define (prim-sphere-indices)
  (let ((rings prim-sphere-rings) (sectors prim-sphere-sectors))
    (let rloop ((r 0) (acc '()))
      (if (= r rings)
          (reverse acc)
          (rloop (+ r 1)
                 (let sloop ((s 0) (a acc))
                   (if (= s sectors)
                       a
                       (let* ((k1 (+ (* r (+ sectors 1)) s))
                              (k2 (+ k1 sectors 1))
                              ;; scm-lint:off
                              ;; top cap (r=0) and bottom cap (r=rings-1) each
                              ;; drop one triangle per sector.
                              ;; scm-lint:on
                              (a1 (if (not (= r 0))
                                      (cons (+ k1 1) (cons k2 (cons k1 a)))
                                      a))
                              (a2 (if (not (= r (- rings 1)))
                                      (cons (+ k2 1) (cons k2 (cons (+ k1 1) a1)))
                                      a1)))
                         (sloop (+ s 1) a2)))))))))

(define (prim-sphere)
  (cons (prim-sphere-verts) (prim-sphere-indices)))

;; scm-lint:off
;; --- Dispatch ------------------------------------------------------------
;; KIND mirrors enum primitive_kind. An unknown kind yields empty arrays, which
;; the driver turns into a NULL blob — matching primitive_generate's default.
;; scm-lint:on
(define (prim-data kind)
  (cond ((= kind 0) (prim-cube))
        ((= kind 1) (prim-sphere))
        ((= kind 2) (prim-plane))
        ((= kind 3) (prim-pyramid))
        (else (cons '() '()))))

(define (primitive-vertices kind) (car (prim-data kind)))
(define (primitive-indices kind)  (cdr (prim-data kind)))
