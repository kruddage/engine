; SPDX-License-Identifier: GPL-2.0-or-later

;;! The shader DSL's WGSL oracle — the twin of shader_test.scm for the WebGPU
;;! target. Runs shader.scm's shader-transpile-wgsl in s7 and checks the WGSL it
;;! lowers the built-in shaders to: struct-based IO, @group/@binding uniforms,
;;! split texture/sampler bindings, and the mat3(mat4) column truncation GLSL
;;! gets for free but WGSL does not. Structural checks here; a real naga(1)
;;! validation of the same output runs from run-tests.sh when naga is installed.

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

;;! The built-in scene shader — the exact DSL asset_plugin seeds, matching the
;;! GLSL oracle so both targets are checked against the same source.
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

(define vs (shader-transpile-wgsl scene "vertex"))
(define fs (shader-transpile-wgsl scene "fragment"))

(display "wgsl: stage presence\n")
(check "vertex stage transpiles to a string" (string? vs))
(check "fragment stage transpiles to a string" (string? fs))
(check "absent stage returns #f (the renderer's error signal)"
       (eq? #f (shader-transpile-wgsl scene "compute")))

(display "wgsl: vertex codegen\n")
(check "the vertex entry point is a @vertex fn returning VertexOutput"
       (and (has? vs "@vertex\n")
	    (has? vs "fn vs_main(in : VertexInput) -> VertexOutput {")))
(check "a used uniform block becomes a WGSL struct, all its fields typed"
       (has? vs "struct Camera {\n\tview_proj : mat4x4<f32>,\n\tmodel : mat4x4<f32>,\n};"))
(check "the block binds at @group(0) @binding(N) from its (block N)"
       (has? vs "@group(0) @binding(0) var<uniform> u_Camera : Camera;"))
(check "vertex inputs carry their @location in a VertexInput struct"
       (and (has? vs "struct VertexInput {")
	    (has? vs "@location(1) a_normal : vec3<f32>,")))
(check "the output struct pairs @builtin(position) with the used varying"
       (has? vs "struct VertexOutput {\n\t@builtin(position) position : vec4<f32>,\n\t@location(0) v_normal : vec3<f32>,\n};"))
(check "mat3(mat4) truncates by columns — WGSL has no direct constructor"
       (has? vs "out.v_normal = (mat3x3<f32>((u_Camera.model)[0].xyz, (u_Camera.model)[1].xyz, (u_Camera.model)[2].xyz) * in.a_normal);"))
(check "block members read qualified through the uniform var; position is out.*"
       (has? vs "out.position = (u_Camera.view_proj * u_Camera.model * vec4<f32>(in.a_pos, 1.0));"))
(check "vertex omits the Material block it never reads"
       (not (has? vs "Material")))

(display "wgsl: fragment codegen\n")
(check "the fragment entry point takes FragmentInput, returns FragmentOutput"
       (and (has? fs "@fragment\n")
	    (has? fs "fn fs_main(in : FragmentInput) -> FragmentOutput {")))
(check "the read-side varying carries the matching @location in FragmentInput"
       (has? fs "struct FragmentInput {\n\t@location(0) v_normal : vec3<f32>,\n};"))
(check "the color target lands in a FragmentOutput struct with its @location"
       (has? fs "struct FragmentOutput {\n\t@location(0) frag_color : vec4<f32>,\n};"))
(check "let* locals lower to immutable WGSL let bindings"
       (has? fs "let n = normalize(in.v_normal);"))
(check "scalar+vector broadcast survives to WGSL"
       (has? fs "let base = (0.5 + (0.5 * n));"))
(check "dot/normalize/typed-vector constructor lower, literals stay short"
       (has? fs "let diff = max(dot(n, normalize(vec3<f32>(0.5, 0.8, 0.4))), 0.0);"))
(check "the final local multiplies through cleanly"
       (has? fs "let col = (base * (0.35 + (0.65 * diff)));"))
(check "the material tint reads through u_Material and swizzles"
       (has? fs "out.frag_color = vec4<f32>((col * u_Material.base_color.rgb), 1.0);"))
(check "Material binds at @group(0) @binding(1) from its (block 1)"
       (has? fs "@group(0) @binding(1) var<uniform> u_Material : Material;"))
(check "the fragment omits the Camera block it never reads"
       (not (has? fs "Camera")))

(display "wgsl: samplers split into texture + sampler\n")
;;! Same textured shader as the GLSL oracle: the fragment samples an albedo map,
;;! the vertex never touches it.
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
(define tvs (shader-transpile-wgsl tex-shader "vertex"))
(define tfs (shader-transpile-wgsl tex-shader "fragment"))
(check "a sampler2D binds as a texture_2d at @group(1) @binding(2i)"
       (has? tfs "@group(1) @binding(0) var albedo : texture_2d<f32>;"))
(check "and gets a companion sampler var at @binding(2i+1)"
       (has? tfs "@group(1) @binding(1) var albedo_sampler : sampler;"))
(check "(sample tex uv) lowers to textureSample(tex, tex_sampler, uv)"
       (has? tfs "out.frag_color = (textureSample(albedo, albedo_sampler, in.v_uv) * u_Material.base_color);"))
(check "the vertex omits the sampler it never samples"
       (not (has? tvs "albedo")))
(check "the vertex omits the Material block it never reads"
       (not (has? tvs "Material")))

(display "wgsl: missing-stage / no-varying shader\n")
(let* ((frag-only "(shader glow (targets (c vec4 (location 0)))
                     (fragment (set c (vec4 1.0 1.0 1.0 1.0))))")
       (gvs (shader-transpile-wgsl frag-only "vertex"))
       (gfs (shader-transpile-wgsl frag-only "fragment")))
  (check "a shader with only a fragment stage has no vertex WGSL"
	 (eq? #f gvs))
  (check "a fragment that reads no varyings takes no input parameter"
	 (has? gfs "fn fs_main() -> FragmentOutput {"))
  (check "its lone target still writes through out.*"
	 (has? gfs "out.c = vec4<f32>(1.0, 1.0, 1.0, 1.0);")))

;;! When KRUDD_WGSL_DUMP names a directory, also write the emitted WGSL there so
;;! run-tests.sh can hand it to naga(1) for a real validation pass — the checks
;;! above prove the shape, naga proves the WGSL actually compiles. Skipped (and
;;! the harness runs oracle-only) when the variable is unset.
(let ((dir (getenv "KRUDD_WGSL_DUMP")))
  (if (and (string? dir) (> (string-length dir) 0))
      (let ((write-wgsl
	      (lambda (path text)
		(if (string? text)
		    (call-with-output-file (string-append dir "/" path)
		      (lambda (p) (write-string text p)))))))
	(write-wgsl "scene.vert.wgsl" vs)
	(write-wgsl "scene.frag.wgsl" fs)
	(write-wgsl "tex.vert.wgsl"   tvs)
	(write-wgsl "tex.frag.wgsl"   tfs)
	(write-wgsl "glow.frag.wgsl"
		    (shader-transpile-wgsl
		      "(shader glow (targets (c vec4 (location 0)))
                         (fragment (set c (vec4 1.0 1.0 1.0 1.0))))"
		      "fragment")))))

(if (= fail-count 0)
    (begin (display "WGSL-TESTS: OK\n") (exit 0))
    (begin (display (string-append "WGSL-TESTS: FAIL ("
				   (number->string fail-count) ")\n"))
	   (exit 1)))
