; SPDX-License-Identifier: GPL-2.0-or-later

;;! mesh-script — the (mesh NAME (generate () ...)) form, its geometry helper
;;! vocabulary, and the mesh generator. Every mesh in the engine — including
;;! the built-in cube/sphere/plane/pyramid/grid (see builtin_mesh_scripts.h)
;;! — is one of these forms; there is no hardcoded C mesh generator anymore.
;;! Mirrors entity_script.scm's (script NAME ...) dispatcher.
;;!
;;! A mesh script is one (mesh NAME (generate () EXPR ...)) S-expression. Its
;;! generate clause takes no arguments and must evaluate to (cons VERTS
;;! INDICES): VERTS a list of 8-element (px py pz nx ny nz u v) lists,
;;! INDICES a flat list of vertex-index integers, a multiple of 3 (one
;;! triangle per triple).
;;!
;;! The engine's mesh-script driver (asset/mesh_script.c) calls
;;! mesh-script-generate with the source text of a bound ASSET_TYPE_MESH_SCRIPT
;;! asset and marshals the (VERTS . INDICES) result into a mesh_blob. Unlike
;;! an entity script (re-run every tick, cheap pose math) a mesh script is a
;;! pure function of nothing — no clock, no entity — so its result is parsed
;;! AND generated once per exact source text, then cached: a render upload and
;;! an editor hit-test both asking for the same mesh cost nothing after the
;;! first.

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

;;! The (mesh NAME (generate () BODY ...)) form evaluates to a zero-argument
;;! thunk over BODY. NAME is a human-readable label only, exactly like the
;;! entity script DSL's NAME — the registry below keys on source text, not
;;! NAME, so a renamed clone never collides with the original it was copied
;;! from.
(define-macro (mesh name generate-clause)
  `(lambda () ,@(cddr generate-clause)))

;;! (mesh-script-generate src) -> (VERTS . INDICES), parsing and running SRC's
;;! generate thunk the first time this exact source is seen and caching the
;;! result. A fault (a malformed form, or a generate body that errors) is
;;! caught and logged, never taking the frame down, and caches to an empty
;;! mesh so a broken script degrades to "nothing renders" rather than a fault
;;! on every subsequent call.
(define (mesh-script-generate src)
  (or (hash-table-ref *mesh-scripts* src)
      (let ((result
              (catch #t
                (lambda ()
                  (let ((thunk (eval (with-input-from-string src (lambda () (read)))
                                     (rootlet))))
                    (thunk)))
                (lambda args
                  (krudd-log 2 (string-append "mesh-script: fault: " src))
                  (cons '() '())))))
        (hash-table-set! *mesh-scripts* src result)
        result)))
