; SPDX-License-Identifier: GPL-2.0-or-later
;
; math_test.scm — the monolang reference oracle, run through s7.
;
; Loads krudd/build/modules/math.scm (the monolang spec) and evaluates its
; (define-c-fn ...) bodies directly in the interpreter, checking the numbers the
; same way the generated C is checked by modules/math/math_test.c. Together they
; pin both ends of the monolang: this proves the spec computes the right math,
; math_test.c proves the C krudd lowers it to matches. Same source, two proofs.
;
; Run via krudd/build/ninja/run-tests.sh. Prints "MATH-TESTS: OK" and exits 0
; when every check passes; prints failures and exits 1 otherwise.

(define krudd-root (or (getenv "KRUDD_ROOT") "."))

(load (string-append krudd-root "/krudd/build/modules/math.scm"))

;; ---------------------------------------------------------------------------
;; Assertion plumbing.
;; ---------------------------------------------------------------------------

(define fail-count 0)

(define (check name ok)
	(if ok
	    (display (string-append "  ok    " name "\n"))
	    (begin
	      (set! fail-count (+ fail-count 1))
	      (display (string-append "  FAIL  " name "\n")))))

;; Float compare with the same tolerance math_test.c uses.
(define (feq a b) (< (abs (- a b)) 1e-4))

;; ---------------------------------------------------------------------------
;; mat4-perspective — mirrors test_perspective in modules/math/math_test.c.
;; fov_y = pi/2, aspect = 1 => f = 1/tan(pi/4) = 1.
;; ---------------------------------------------------------------------------

(define near 0.1)
(define far  100.0)
(define m (mat4-perspective (* pi 0.5) 1.0 near far))

;; A mat4 value is a flat 16-list, column-major: (row r, col c) at index c*4 + r.
(define (mref i) (list-ref m i))

(check "perspective is 16 elements" (= (length m) 16))
(check "perspective m[0]  = 1"        (feq (mref 0)  1.0))
(check "perspective m[5]  = 1"        (feq (mref 5)  1.0))
(check "perspective m[10] = (f+n)/(n-f)"
       (feq (mref 10) (/ (+ far near) (- near far))))
(check "perspective m[11] = -1"       (feq (mref 11) -1.0))
(check "perspective m[14] = 2fn/(n-f)"
       (feq (mref 14) (/ (* 2.0 (* far near)) (- near far))))
;; row 3 of columns 0-2, and the last column's w, must be zero.
(check "perspective m[3]  = 0"        (feq (mref 3)  0.0))
(check "perspective m[7]  = 0"        (feq (mref 7)  0.0))
(check "perspective m[15] = 0"        (feq (mref 15) 0.0))

;; ---------------------------------------------------------------------------
;; Verdict.
;; ---------------------------------------------------------------------------

(if (= fail-count 0)
    (begin (display "MATH-TESTS: OK\n") (exit 0))
    (begin (display (string-append "MATH-TESTS: FAIL ("
				   (number->string fail-count) ")\n"))
	   (exit 1)))
