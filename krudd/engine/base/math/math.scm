; SPDX-License-Identifier: GPL-2.0-or-later

(define (vec4 a b c d) (list a b c d))
(define (mat4-cols c0 c1 c2 c3) (append c0 c1 c2 c3))

(define-macro (define-c-fn sig . body)
  (let loop ((l (cdr sig)) (args '()))
    (if (or (null? l) (eq? (car l) '->))
        (cons 'define (cons (cons (car sig) (reverse args)) body))
        (loop (cdr l) (cons (caar l) args)))))

(define-c-fn (mat4-perspective (fov-y f32) (aspect f32) (near f32) (far f32)
                               -> mat4)
  (let* ((f         (/ 1.0 (tan (* fov-y 0.5))))
         (inv-range (/ 1.0 (- near far))))
    (mat4-cols
     (vec4 (/ f aspect) 0.0 0.0 0.0)
     (vec4 0.0 f 0.0 0.0)
     (vec4 0.0 0.0 (* (+ far near) inv-range) -1.0)
     (vec4 0.0 0.0 (* 2.0 far near inv-range) 0.0))))
