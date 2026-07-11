; SPDX-License-Identifier: GPL-2.0-or-later

(define prim-verts-max 561)
(define prim-indices-max 2880)

(define-macro (define-c-struct . _) #f)
(define-macro (define-c-export . _) #f)

(define-c-struct prim-vertex
  (px f32) (py f32) (pz f32)
  (nx f32) (ny f32) (nz f32)
  (u  f32) (w  f32))

(define-c-export (primitive-vertices (kind i32) -> (vector prim-vertex max))
  (calls primitive-vertices))

(define-c-export (primitive-indices (kind i32) -> (vector u16 max))
  (calls primitive-indices))

(define (v3 a b c) (list a b c))

(define prim-corners '((-1.0 -1.0) (1.0 -1.0) (1.0 1.0) (-1.0 1.0)))

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

(define (prim-quad-indices base)
  (list base (+ base 1) (+ base 2) base (+ base 2) (+ base 3)))

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

(define (prim-plane)
  (let ((top (list (v3 0.0 1.0 0.0) (v3 1.0 0.0 0.0) (v3 0.0 0.0 1.0))))
    (cons (prim-quad-verts top 0.0)
          (prim-quad-indices 0))))

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

(define prim-pyr-base
  (list (v3 -0.5 -0.5 -0.5) (v3 0.5 -0.5 -0.5)
        (v3 0.5 -0.5 0.5)   (v3 -0.5 -0.5 0.5)))
(define prim-pyr-apex (v3 0.0 0.5 0.0))

(define (prim-pyramid)
  (let* ((base-face (list (v3 0.0 -1.0 0.0) (v3 1.0 0.0 0.0) (v3 0.0 0.0 1.0)))
         (base-verts (prim-quad-verts base-face 0.5))
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
                              (a1 (if (not (= r 0))
                                      (cons (+ k1 1) (cons k2 (cons k1 a)))
                                      a))
                              (a2 (if (not (= r (- rings 1)))
                                      (cons (+ k2 1) (cons k2 (cons (+ k1 1) a1)))
                                      a1)))
                         (sloop (+ s 1) a2)))))))))

(define (prim-sphere)
  (cons (prim-sphere-verts) (prim-sphere-indices)))

(define (prim-data kind)
  (cond ((= kind 0) (prim-cube))
        ((= kind 1) (prim-sphere))
        ((= kind 2) (prim-plane))
        ((= kind 3) (prim-pyramid))
        (else (cons '() '()))))

(define (primitive-vertices kind) (car (prim-data kind)))
(define (primitive-indices kind)  (cdr (prim-data kind)))
