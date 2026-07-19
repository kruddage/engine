; SPDX-License-Identifier: GPL-2.0-or-later

(define krudd-root (or (getenv "KRUDD_ROOT") "."))

(load (string-append krudd-root "/krudd/engine/math/math.scm"))

(define fail-count 0)

(define (check name ok)
  (if ok
      (display (string-append "  ok    " name "\n"))
      (begin
        (set! fail-count (+ fail-count 1))
        (display (string-append "  FAIL  " name "\n")))))

(define (feq a b) (< (abs (- a b)) 1e-4))

(define near 0.1)
(define far  100.0)
(define m (mat4-perspective (* pi 0.5) 1.0 near far))

(define (mref i) (list-ref m i))

(check "perspective is 16 elements" (= (length m) 16))
(check "perspective m[0]  = 1"        (feq (mref 0)  1.0))
(check "perspective m[5]  = 1"        (feq (mref 5)  1.0))
(check "perspective m[10] = (f+n)/(n-f)"
       (feq (mref 10) (/ (+ far near) (- near far))))
(check "perspective m[11] = -1"       (feq (mref 11) -1.0))
(check "perspective m[14] = 2fn/(n-f)"
       (feq (mref 14) (/ (* 2.0 (* far near)) (- near far))))
(check "perspective m[3]  = 0"        (feq (mref 3)  0.0))
(check "perspective m[7]  = 0"        (feq (mref 7)  0.0))
(check "perspective m[15] = 0"        (feq (mref 15) 0.0))

(if (= fail-count 0)
    (begin (display "MATH-TESTS: OK\n") (exit 0))
    (begin (display (string-append "MATH-TESTS: FAIL ("
                                   (number->string fail-count) ")\n"))
           (exit 1)))
