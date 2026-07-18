; SPDX-License-Identifier: GPL-2.0-or-later

(define krudd-root (or (getenv "KRUDD_ROOT") "."))

(load (string-append krudd-root "/krudd/engine/shader/shader.scm"))

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

(display "shader: samplers\n")
;;! A shader declaring a sampler alongside a std140 Material block: the fragment
;;! samples it, the vertex never touches it. Samplers live in (uniforms ...) but
;;! bind to their own texture unit rather than the block.
(define tex-shader "(shader textured
  (inputs (a_pos vec3 (location 0)) (a_uv0 vec2 (location 1)))
  (uniforms (Camera (block 0) (layout std140) (view_proj mat4))
            (Material (block 1) (layout std140) (base_color vec4 (edit color)))
            (albedo sampler2D))
  (varyings (v_uv vec2))
  (targets (frag_color vec4 (location 0)))
  (vertex
    (set v_uv a_uv0)
    (set position (* view_proj (vec4 a_pos 1.0))))
  (fragment
    (set frag_color (* (sample albedo v_uv) base_color))))")
(define tvs (shader-transpile tex-shader "vertex"))
(define tfs (shader-transpile tex-shader "fragment"))
(check "the fragment declares the sampler it uses (own unit, no std140 layout)"
       (has? tfs "uniform sampler2D albedo;"))
(check "the sampler declaration carries no layout(std140)"
       (not (has? tfs "layout(std140) uniform sampler2D")))
(check "(sample tex uv) lowers to the GLSL texture() builtin"
       (has? tfs "frag_color = (texture(albedo, v_uv) * base_color);"))
(check "the Material block still declares alongside the sampler"
       (has? tfs "layout(std140) uniform Material {\n\thighp vec4 base_color;\n};"))
(check "the vertex omits the sampler it never samples"
       (not (has? tvs "sampler2D")))
(check "a sampler is not a material param (only base_color is, block size 16)"
       (let ((mp (shader-material-params tex-shader)))
	 (and (= (car mp) 16) (= (length (cdr mp)) 1)
	      (equal? (car (list-ref mp 1)) "base_color"))))

;;! kruddgui's SDF overlay shader — the reason fwidth and (precision highp) exist,
;;! and the shape they have to work on. kruddgui still compiles the hand-written
;;! GLSL below as a literal string; flipping it to this DSL source is a separate
;;! change, blocked on the WebGPU backend acquiring its surface texture per frame
;;! rather than per command buffer.
;;!
;;! Checking it here anyway is the point: every assertion below is against the
;;! GLSL kruddgui emits *today*, statement for statement, so the flip lands
;;! against a lowering already proven to reproduce it. Text that renders subtly
;;! fat or thin is the failure this is guarding against, and it is not a failure
;;! that announces itself.
(define kgui-sdf "(shader kruddgui-sdf
  (precision highp)
  (inputs (a_pos vec2 (location 0)) (a_uv vec2 (location 1))
          (a_col vec4 (location 2)))
  (uniforms (View (block 0) (layout std140) (u_viewport vec2))
            (u_tex sampler2D))
  (varyings (v_uv vec2) (v_col vec4))
  (targets (frag vec4 (location 0)))
  (vertex
    (let* ((p (/ a_pos u_viewport))
           (q (- (* p 2.0) 1.0)))
      (set v_uv a_uv)
      (set v_col a_col)
      (set position (vec4 (swizzle q x) (- 0.0 (swizzle q y)) 0.0 1.0))))
  (fragment
    (let* ((d (swizzle (sample u_tex v_uv) a))
           (w (max (fwidth d) 0.00390625))
           (cov (smoothstep (- 0.5 w) (+ 0.5 w) d)))
      (set frag (vec4 (swizzle v_col rgb) (* (swizzle v_col a) cov))))))")

(define kvs (shader-transpile kgui-sdf "vertex"))
(define kfs (shader-transpile kgui-sdf "fragment"))

(display "shader: fragment precision\n")
(check "a fragment stage defaults to mediump"
       (has? fs "precision mediump float;"))
(check "(precision highp) overrides that default"
       (has? kfs "precision highp float;"))
(check "and the default spelling is then absent"
       (not (has? kfs "precision mediump float;")))
(check "precision is a fragment-stage qualifier; the vertex declares none"
       (not (has? kvs "precision")))

(display "shader: fwidth\n")
(check "fwidth lowers to the GLSL builtin of the same name"
       (has? kfs "float w = max(fwidth(d), 0.00390625);"))
(check "fwidth returns its argument's type (float here, so w is a float)"
       (has? kfs "float w = "))

(display "shader: kruddgui overlay parity with the GLSL it replaced\n")
(check "the vertex reproduces the viewport divide"
       (has? kvs "vec2 p = (a_pos / u_viewport);"))
(check "and the 0..1 -> -1..1 remap"
       (has? kvs "vec2 q = ((p * 2.0) - 1.0);"))
(check "and the y-down flip into clip space"
       (has? kvs "gl_Position = vec4(q.x, (0.0 - q.y), 0.0, 1.0);"))
(check "the fragment reads the SDF distance from the atlas alpha"
       (has? kfs "float d = texture(u_tex, v_uv).a;"))
(check "the coverage ramp thresholds at 0.5 +/- the derivative width"
       (has? kfs "float cov = smoothstep((0.5 - w), (0.5 + w), d);"))
(check "colour comes from the vertex, alpha is scaled by coverage"
       (has? kfs "frag = vec4(v_col.rgb, (v_col.a * cov));"))
(check "the vertex declares the View block it reads"
       (has? kvs "layout(std140) uniform View {\n\thighp vec2 u_viewport;\n};"))
(check "the fragment omits View, which it never reads"
       (not (has? kfs "uniform View")))
(check "the fragment declares the atlas sampler"
       (has? kfs "uniform sampler2D u_tex;"))

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
