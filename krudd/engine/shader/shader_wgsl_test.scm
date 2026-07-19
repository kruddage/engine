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

(display "wgsl: depth2D lowers to a depth texture\n")
;;! A depth sampler alongside an ordinary one, so the slot numbering is checked
;;! with both kinds present — the shadow shaders' real shape.
(define depth-shader "(shader shadowed
  (inputs (a_pos vec3 (location 0)) (a_uv0 vec2 (location 1)))
  (uniforms (Camera (block 0) (layout std140) (view_proj mat4))
            (albedo sampler2D)
            (shadow_map depth2D))
  (varyings (v_uv vec2))
  (targets (frag_color vec4 (location 0)))
  (vertex
    (set v_uv a_uv0)
    (set position (* view_proj (vec4 a_pos 1.0))))
  (fragment
    (set frag_color (* (sample albedo v_uv)
                       (swizzle (sample shadow_map v_uv) r)))))")
(define dfs (shader-transpile-wgsl depth-shader "fragment"))
(check "depth2D binds as a texture_depth_2d, not a texture_2d<f32>"
       (has? dfs "@group(1) @binding(2) var shadow_map : texture_depth_2d;"))
(check "it still takes a companion sampler at 2i+1"
       (has? dfs "@group(1) @binding(3) var shadow_map_sampler : sampler;"))
(check "an ordinary sampler2D in the same shader is unaffected"
       (has? dfs "@group(1) @binding(0) var albedo : texture_2d<f32>;"))
;;! textureSample returns f32 for a depth texture, so the .r the DSL (and GL)
;;! spell has to survive — the widen is what keeps `sample` meaning vec4 on both
;;! backends rather than a rule that holds only here.
(check "a depth sample widens to vec4 so .r stays valid"
       (has? dfs "vec4<f32>(textureSample(shadow_map, shadow_map_sampler, in.v_uv)).r"))
(check "a colour sample is not widened"
       (has? dfs "(textureSample(albedo, albedo_sampler, in.v_uv)"))
;;! GL reads depth through a plain sampler2D, so the GLSL target must not learn
;;! a new type name it has no meaning for.
(check "GLSL spells depth2D as sampler2D"
       (has? (shader-transpile depth-shader "fragment")
             "uniform sampler2D shadow_map;"))

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

;;! kruddgui's SDF overlay shader — the same DSL shader_test.scm checks the GLSL
;;! lowering of, here on the WGSL side. This is the shader that will put the
;;! editor UI on WebGPU once kruddgui flips to it, so its bindings are the
;;! contract renderer_webgpu.c scans for: the View block at group 0 binding 0,
;;! and the atlas sampler pair at group 1 bindings 0 and 1. Pinning them now
;;! means the flip cannot quietly land on different slots than the backend binds.
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

(define kvs (shader-transpile-wgsl kgui-sdf "vertex"))
(define kfs (shader-transpile-wgsl kgui-sdf "fragment"))

(display "wgsl: fwidth\n")
(check "fwidth lowers to the WGSL builtin of the same name"
       (has? kfs "let w = max(fwidth(d), 0.00390625);"))

(display "wgsl: precision is GLSL-only\n")
(check "(precision highp) emits nothing into WGSL, which has no qualifier for it"
       (not (has? kfs "precision")))
(check "and it does not leak into the vertex stage either"
       (not (has? kvs "precision")))

(display "wgsl: kruddgui overlay bindings\n")
(check "the View block lands at group 0 binding 0, where kruddgui binds its UBO"
       (has? kvs "@group(0) @binding(0) var<uniform> u_View : View;"))
(check "the block member reads through the uniform var"
       (has? kvs "let p = (in.a_pos / u_View.u_viewport);"))
(check "the fragment omits View, which it never reads"
       (not (has? kfs "u_View")))
(check "the atlas sampler is a texture/sampler pair at group 1 bindings 0 and 1"
       (and (has? kfs "@group(1) @binding(0) var u_tex : texture_2d<f32>;")
            (has? kfs "@group(1) @binding(1) var u_tex_sampler : sampler;")))
(check "(sample u_tex v_uv) lowers to textureSample with the companion sampler"
       (has? kfs "let d = textureSample(u_tex, u_tex_sampler, in.v_uv).a;"))
(check "the varyings keep matching locations across the stage boundary"
       (and (has? kvs "@location(0) v_uv : vec2<f32>,")
            (has? kvs "@location(1) v_col : vec4<f32>,")
            (has? kfs "@location(0) v_uv : vec2<f32>,")
            (has? kfs "@location(1) v_col : vec4<f32>,")))
(check "the clip-space write goes through the position builtin"
       (has? kvs "out.position = vec4<f32>(q.x, (0.0 - q.y), 0.0, 1.0);"))

;;! Byte index of SUB in S, or -1 — for asserting module-scope order (a helper
;;! must sit below the vars it reads and above the entry point that calls it).
(define (idx s sub)
  (let ((hl (string-length s)) (nl (string-length sub)))
    (let loop ((i 0))
      (cond ((> (+ i nl) hl) -1)
            ((string=? (substring s i (+ i nl)) sub) i)
            (else (loop (+ i 1)))))))

(display "wgsl: functions (reusable helpers)\n")
;;! The same helped shader the GLSL oracle checks, so both targets lower one
;;! source: a pure tonemap, a shadow_at that samples a depth map only inside the
;;! helper, and an unreached `unused`.
(define fn-shader "(shader helped
  (inputs (a_pos vec3 (location 0)) (a_uv0 vec2 (location 1)))
  (uniforms (Camera (block 0) (layout std140) (view_proj mat4))
            (shadow_map depth2D))
  (varyings (v_uv vec2) (v_lightpos vec4))
  (targets (frag_color vec4 (location 0)))
  (functions
    (tonemap ((color vec3)) vec3
      (let* ((mapped (/ color (+ color (vec3 1.0 1.0 1.0))))
             (g (pow mapped (vec3 0.4545 0.4545 0.4545))))
        (return g)))
    (shadow_at ((lp vec4)) float
      (let* ((uv (swizzle lp xy))
             (s (swizzle (sample shadow_map uv) r)))
        (return s)))
    (unused ((x float)) float
      (return (* x 2.0))))
  (vertex
    (set v_uv a_uv0)
    (set position (* view_proj (vec4 a_pos 1.0))))
  (fragment
    (let* ((sh (shadow_at v_lightpos))
           (col (tonemap (vec3 sh sh sh))))
      (set frag_color (vec4 col 1.0)))))")
(define hfs (shader-transpile-wgsl fn-shader "fragment"))
(define hvs (shader-transpile-wgsl fn-shader "vertex"))
(check "a helper lowers to a WGSL fn with typed params and return"
       (has? hfs "fn tonemap(color : vec3<f32>) -> vec3<f32> {"))
(check "its let* -> let and (return EXPR) -> return"
       (and (has? hfs "let mapped = (color / (color + vec3<f32>(1.0, 1.0, 1.0)));")
            (has? hfs "return g;")))
(check "a sampling helper is fragment-only: textureSample, depth widened to vec4"
       (has? hfs "let s = vec4<f32>(textureSample(shadow_map, shadow_map_sampler, uv)).r;"))
(check "a call reads as a WGSL call, typed by the helper's return"
       (and (has? hfs "let sh = shadow_at(in.v_lightpos);")
            (has? hfs "let col = tonemap(vec3<f32>(sh, sh, sh));")))
(check "the fragment binds a sampler only its helper reads (transitive refs)"
       (and (has? hfs "@group(1) @binding(0) var shadow_map : texture_depth_2d;")
            (has? hfs "@group(1) @binding(1) var shadow_map_sampler : sampler;")))
(check "helpers sit below their module-scope vars and above the entry point"
       (and (< (idx hfs "var shadow_map :") (idx hfs "fn tonemap"))
            (< (idx hfs "fn shadow_at") (idx hfs "@fragment"))))
(check "an unreached helper is not emitted"
       (not (has? hfs "fn unused")))
(check "the vertex reaches no helper, so emits none and no helper-only binding"
       (and (not (has? hvs "fn shadow_at")) (not (has? hvs "fn tonemap"))
            (not (has? hvs "shadow_map"))))

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
        ;;! kruddgui's overlay: the fwidth path, and the only shader here whose
        ;;! WGSL a real backend rejects loudly if the derivative lowers wrong.
        (write-wgsl "kgui-sdf.vert.wgsl" kvs)
        (write-wgsl "kgui-sdf.frag.wgsl" kfs)
        (write-wgsl "glow.frag.wgsl"
                    (shader-transpile-wgsl
                     "(shader glow (targets (c vec4 (location 0)))
                         (fragment (set c (vec4 1.0 1.0 1.0 1.0))))"
                     "fragment"))
        (write-wgsl "helped.frag.wgsl" hfs))))

(if (= fail-count 0)
    (begin (display "WGSL-TESTS: OK\n") (exit 0))
    (begin (display (string-append "WGSL-TESTS: FAIL ("
                                   (number->string fail-count) ")\n"))
           (exit 1)))
