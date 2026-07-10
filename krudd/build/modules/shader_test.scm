; SPDX-License-Identifier: GPL-2.0-or-later

(define krudd-root (or (getenv "KRUDD_ROOT") "."))

(load (string-append krudd-root "/krudd/build/modules/shader.scm"))

(define fail-count 0)

(define (check name ok)
	(if ok
	    (display (string-append "  ok    " name "\n"))
	    (begin
	      (set! fail-count (+ fail-count 1))
	      (display (string-append "  FAIL  " name "\n")))))

(define (has? s sub)
	(let ((hl (string-length s)) (nl (string-length sub)))
	  (let loop ((i 0))
	    (cond ((> (+ i nl) hl) #f)
		  ((string=? (substring s i (+ i nl)) sub) #t)
		  (else (loop (+ i 1)))))))

;;! The built-in scene shader, the exact DSL asset_plugin seeds.
(define scene "(shader scene
  (inputs (a_pos vec3 (location 0)) (a_normal vec3 (location 1)) (a_uv0 vec2 (location 2)))
  (uniforms (Camera (block 0) (layout std140) (view_proj mat4) (model mat4))
            (Material (block 1) (layout std140) (base_color vec4 (edit color))))
  (varyings (v_normal vec3))
  (targets (frag_color vec4 (location 0)))
  (vertex
    (set v_normal (* (mat3 model) a_normal))
    (set position (* view_proj model (vec4 a_pos 1.0))))
  (fragment
    (let* ((n (normalize v_normal))
           (base (+ 0.5 (* 0.5 n)))
           (diff (max (dot n (normalize (vec3 0.5 0.8 0.4))) 0.0))
           (col (* base (+ 0.35 (* 0.65 diff)))))
      (set frag_color (vec4 (* col (swizzle base_color rgb)) 1.0)))))")

(define vs (shader-transpile scene "vertex"))
(define fs (shader-transpile scene "fragment"))

(display "shader: stage presence\n")
(check "vertex stage transpiles to a string" (string? vs))
(check "fragment stage transpiles to a string" (string? fs))
(check "absent stage returns #f (the renderer's error signal)"
       (eq? #f (shader-transpile scene "compute")))

(display "shader: vertex codegen\n")
(check "vertex opens with the GLSL ES 300 version"
       (has? vs "#version 300 es\n"))
(check "attribute locations come from the shared IO model"
       (has? vs "layout(location = 1) in vec3 a_normal;"))
(check "std140 uniform block is declared from the DSL, members pinned highp"
       (has? vs "layout(std140) uniform Camera {\n\thighp mat4 view_proj;\n\thighp mat4 model;\n};"))
(check "the varying is an out in the vertex stage"
       (has? vs "out vec3 v_normal;"))
(check "mat3 constructor and component multiply lower to GLSL"
       (has? vs "v_normal = (mat3(model) * a_normal);"))
(check "the position builtin lowers to gl_Position"
       (has? vs "gl_Position = (view_proj * model * vec4(a_pos, 1.0));"))
(check "vertex declares no fragment precision"
       (not (has? vs "precision mediump float;")))
(check "vertex omits the Material block it never reads"
       (not (has? vs "uniform Material")))

(display "shader: fragment codegen\n")
(check "fragment sets the float precision"
       (has? fs "precision mediump float;\n"))
(check "the same varying is an in in the fragment stage"
       (has? fs "in vec3 v_normal;"))
(check "the fragment omits the Camera block it never reads (no precision clash)"
       (not (has? fs "uniform Camera")))
(check "std140 Material block is declared from the DSL, members pinned highp"
       (has? fs "layout(std140) uniform Material {\n\thighp vec4 base_color;\n};"))
(check "the color target carries its location"
       (has? fs "layout(location = 0) out vec4 frag_color;"))
(check "let* locals get inferred types (vec3 from normalize)"
       (has? fs "vec3 n = normalize(v_normal);"))
(check "scalar+vector broadcast infers vec3"
       (has? fs "vec3 base = (0.5 + (0.5 * n));"))
(check "dot infers float and prints clean literals"
       (has? fs "float diff = max(dot(n, normalize(vec3(0.5, 0.8, 0.4))), 0.0);"))
(check "0.35 round-trips to a short literal, not 0.35000000000000003"
       (has? fs "0.35 + "))
(check "vector*scalar infers vec3 for the final color"
       (has? fs "vec3 col = (base * (0.35 + (0.65 * diff)));"))
(check "the material base_color tints the final output"
       (has? fs "frag_color = vec4((col * base_color.rgb), 1.0);"))

(display "shader: material params\n")
(check "an (edit color) hint on base_color never changes the compiled GLSL"
       (has? fs "layout(std140) uniform Material {\n\thighp vec4 base_color;\n};"))

;;! A richer Material block exercising std140 packing and every edit hint.
(define param-shader "(shader p
  (uniforms (Material (block 1) (layout std140)
    (base_color vec4 (edit color))
    (roughness  float (edit range 0 1))
    (tint       vec3 (edit color))
    (uv_scale   vec2)))
  (fragment (set c (vec4 (* base_color roughness) 1.0))))")
(define mp (shader-material-params param-shader))
(check "material params report the std140 block size (64)"
       (= (car mp) 64))
(check "base_color: vec4 color at offset 0, size 16, 4 components"
       (equal? (list-ref mp 1)
	       (list "base_color" "vec4" 0 16 4 "color" 0 0 '())))
(check "roughness: float range [0 1] at offset 16, size 4, 1 component"
       (equal? (list-ref mp 2)
	       (list "roughness" "float" 16 4 1 "range" 0 1 '())))
(check "tint: vec3 color aligns to 16 (offset 32), size 12, 3 components"
       (equal? (list-ref mp 3)
	       (list "tint" "vec3" 32 12 3 "color" 0 0 '())))
(check "uv_scale: unhinted vec2 aligns to 8 (offset 48), size 8, kind none"
       (equal? (list-ref mp 4)
	       (list "uv_scale" "vec2" 48 8 2 "none" 0 0 '())))
(check "the scene shader's single base_color is one color param, block size 16"
       (let ((sp (shader-material-params scene)))
	 (and (= (car sp) 16)
	      (equal? (list-ref sp 1)
		      (list "base_color" "vec4" 0 16 4 "color" 0 0 '())))))
(check "a shader with no Material block reports size 0 and no params"
       (equal? (shader-material-params
		 "(shader n (targets (c vec4 (location 0)))
		    (fragment (set c (vec4 1.0 1.0 1.0 1.0))))")
	       (list 0)))

(display "shader: missing-stage shader\n")
(let ((frag-only "(shader glow (targets (c vec4 (location 0)))
                    (fragment (set c (vec4 1.0 1.0 1.0 1.0))))"))
  (check "a shader with only a fragment stage has no vertex GLSL"
	 (eq? #f (shader-transpile frag-only "vertex")))
  (check "and its fragment stage still transpiles"
	 (string? (shader-transpile frag-only "fragment"))))

(if (= fail-count 0)
    (begin (display "SHADER-TESTS: OK\n") (exit 0))
    (begin (display (string-append "SHADER-TESTS: FAIL ("
				   (number->string fail-count) ")\n"))
	   (exit 1)))
