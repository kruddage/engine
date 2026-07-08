; SPDX-License-Identifier: GPL-2.0-or-later
;; scm-lint:off
;
; math.scm — spatial math primitives, in the monolang.
;
; This is the first module lowered from C to *native C* by krudd's expression
; emitter. Unlike md_parse.scm (which bakes an s7 image the shim calls back into
; at runtime), the (define-c-fn ...) forms here carry an arithmetic body that
; krudd lowers to standalone C — no interpreter at runtime. The same body is
; also valid, evaluable Scheme, so this file doubles as its own reference
; implementation: math_test.scm loads it and checks the numbers in s7, while the
; generated C is checked by the existing math_test.c. One spec, two proofs.
;
; The value-oriented shape (functions return a matrix built from column vectors,
; rather than mutating an out-pointer) is deliberate: it lowers cleanly to C
; today and to WGSL/Metal/GLSL later — a shader cannot host an interpreter, but
; it can host emitted arithmetic. The C backend maps a `-> mat4` return to the
; engine's `void f(struct mat4 *out, ...)` convention.
;
; The binding vocabulary the emitter understands — the whole surface; anything
; outside it is a loud build error, never silently extended:
;   scalars        f32 / f64 params (float / double)
;   arithmetic     (+ - * /), n-ary, on scalars
;   intrinsics     (tan sin cos sqrt) -> the C float variants (tanf, ...)
;   let*           sequential float temporaries
;   vec4 / mat4-cols   build a mat4 value, column-major
;   return         -> mat4  (aggregate, lowered to an out-pointer)
;
; The emitter that reads these forms lives in krudd/build/introspect.scm.
;; scm-lint:on

;; scm-lint:off
;; ---------------------------------------------------------------------------
;; Runtime prelude.
;;
;; These make the file loadable standalone, so the exact same (define-c-fn ...)
;; bodies krudd lowers to C can be *evaluated* in s7 as the reference oracle.
;; krudd ignores everything but the (define-c-fn ...) forms at synthesis time,
;; so the prelude is invisible to the C emitter.
;;
;; A mat4 value is a flat 16-element list in column-major order: element (row r,
;; col c) is at index c*4 + r, matching struct mat4's m[] and GL's convention.
;; ---------------------------------------------------------------------------
;; scm-lint:on

(define (vec4 a b c d) (list a b c d))
(define (mat4-cols c0 c1 c2 c3) (append c0 c1 c2 c3))

;; scm-lint:off
;; define-c-fn: at runtime, expands to a real procedure (so the oracle can call
;; it); at synthesis time krudd reads the form as data and lowers the body to C.
;; The signature is (name (arg kind) ... -> ret); the runtime lambda keeps only
;; the argument names.
;; scm-lint:on
(define-macro (define-c-fn sig . body)
  (let loop ((l (cdr sig)) (args '()))
    (if (or (null? l) (eq? (car l) '->))
	(cons 'define (cons (cons (car sig) (reverse args)) body))
	(loop (cdr l) (cons (caar l) args)))))

;; scm-lint:off
;; ---------------------------------------------------------------------------
;; The monolang.
;; ---------------------------------------------------------------------------
;; scm-lint:on

;; scm-lint:off
;; Perspective projection (right-handed, z maps to GL NDC [-1, 1]). fov-y is the
;; vertical field of view in radians. Passing the result to glUniformMatrix4fv
;; with transpose=GL_FALSE matches glm::perspective.
;; scm-lint:on
(define-c-fn (mat4-perspective (fov-y f32) (aspect f32) (near f32) (far f32)
			       -> mat4)
  (let* ((f         (/ 1.0 (tan (* fov-y 0.5))))
	 (inv-range (/ 1.0 (- near far))))
    (mat4-cols
      (vec4 (/ f aspect) 0.0 0.0 0.0)
      (vec4 0.0 f 0.0 0.0)
      (vec4 0.0 0.0 (* (+ far near) inv-range) -1.0)
      (vec4 0.0 0.0 (* 2.0 far near inv-range) 0.0))))
