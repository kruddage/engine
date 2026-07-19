; SPDX-License-Identifier: GPL-2.0-or-later

;;! shader — the krudd shader DSL and its transpiler.
;;!
;;! A shader asset is a single (shader NAME ...) S-expression carrying BOTH
;;! stages, a shared IO model (inputs, uniforms, varyings, targets) declared
;;! once, and an optional (functions ...) section of reusable helpers both
;;! stages can call (see "shader functions" below). This is the source of truth;
;;! GLSL and WGSL are backend targets the renderer lowers to at bind time. The
;;! same file is embedded into the runtime
;;! image so the web editor transpiles on the fly, and loaded at build time by
;;! the Scheme oracle tests — write once, run in both hosts.
;;!
;;! (shader-transpile SRC STAGE) parses the DSL text SRC and returns the
;;! GLSL ES 3.00 for STAGE ("vertex" or "fragment"), or #f when the shader has
;;! no such stage — the "matching stage else error" contract the renderer wants.
;;! (shader-transpile-wgsl SRC STAGE) is its twin for the WebGPU backend, lowering
;;! the same DSL to WGSL (see the "WGSL backend target" section at the bottom).

;;! --- small list helpers (the runtime image has only base s7) ---

(define (shader-keep pred lst)
  (cond ((null? lst) '())
        ((pred (car lst)) (cons (car lst) (shader-keep pred (cdr lst))))
        (else (shader-keep pred (cdr lst)))))

(define (shader-foldl f init lst)
  (if (null? lst) init (shader-foldl f (f init (car lst)) (cdr lst))))

;;! Join STRS with SEP between them (no trailing separator).
(define (shader-join sep strs)
  (cond ((null? strs) "")
        ((null? (cdr strs)) (car strs))
        (else (string-append (car strs) sep (shader-join sep (cdr strs))))))

;;! --- type system ---

(define (shader-vec? t) (and (memq t '(vec2 vec3 vec4)) #t))
(define (shader-mat? t) (and (memq t '(mat2 mat3 mat4)) #t))

;;! GLSL name of a DSL type. GLSL ES 300 spells them the same; a future WGSL or
;;! MSL backend is where these diverge.
;;! GLSL spelling of a DSL type. depth2D is the one that is not its own name: GL
;;! reads a depth texture through an ordinary sampler2D (depth in .r), so the
;;! distinction exists only to tell the WGSL path which textures are depth.
(define (shader-type->glsl t)
  (case t
    ((depth2D) "sampler2D")
    (else (symbol->string t))))

;;! Result type of a * b, covering matrix·matrix, matrix·vector, scalar
;;! broadcast, and component-wise vector products the way GLSL's * overloads.
(define (shader-mul-type a b)
  (cond ((and (shader-mat? a) (shader-mat? b)) a)
        ((and (shader-mat? a) (shader-vec? b)) b)
        ((and (shader-vec? a) (shader-mat? b)) a)
        ((eq? a 'float) b)
        ((eq? b 'float) a)
        (else a)))

;;! Result type of a + b / a - b: a matrix or vector operand widens a scalar.
(define (shader-add-type a b)
  (cond ((shader-mat? a) a) ((shader-mat? b) b)
        ((shader-vec? a) a) ((shader-vec? b) b)
        (else 'float)))

(define (shader-swizzle-type comps)
  (let ((n (string-length (symbol->string comps))))
    (if (= n 1) 'float
        (string->symbol (string-append "vec" (number->string n))))))

;;! Infer the type of expression E under the identifier→type alist ENV.
(define (shader-infer e env)
  (cond
   ((number? e) 'float)
   ((symbol? e)
    (let ((p (assq e env)))
      (if p (cdr p) (error 'shader-unknown-identifier e))))
   ((pair? e)
    (let ((op (car e)) (args (cdr e)))
      (case op
        ((vec2) 'vec2) ((vec3) 'vec3) ((vec4) 'vec4)
        ((mat2) 'mat2) ((mat3) 'mat3) ((mat4) 'mat4)
        ((float) 'float) ((int) 'int)
        ((swizzle) (shader-swizzle-type (cadr args)))
        ((dot length distance) 'float)
        ((sample) 'vec4)
        ((cross) 'vec3)
        ;;! fwidth reads a screen-space derivative, so it is fragment-only
        ;;! on both targets; like the rest of this group it returns its
        ;;! argument's type. GLSL and WGSL spell it identically, so the
        ;;! generic call emit at the bottom of each lowering covers it.
        ((normalize sin cos tan sqrt abs floor fract exp log
                    radians degrees fwidth)
         (shader-infer (car args) env))
        ((max min pow mod step reflect mix clamp smoothstep)
         (shader-infer (car args) env))
        ((*) (shader-foldl (lambda (acc x)
                             (shader-mul-type acc (shader-infer x env)))
                           (shader-infer (car args) env) (cdr args)))
        ((+ -) (shader-foldl (lambda (acc x)
                               (shader-add-type acc (shader-infer x env)))
                             (shader-infer (car args) env) (cdr args)))
        ((/) (shader-infer (car args) env))
        (else
         ;;! A call to a shader helper: its signature rides in ENV as a
         ;;! (NAME fn RET-TYPE) entry (see shader-fn-sigs), so the call
         ;;! types as the helper's return type. The pair? guard keeps an
         ;;! ordinary (NAME . TYPE) identifier entry from being mistaken
         ;;! for one.
         (let ((sig (assq op env)))
           (if (and sig (pair? (cdr sig)) (eq? (cadr sig) 'fn))
               (caddr sig)
               (error 'shader-unknown-op op)))))))
   (else (error 'shader-bad-expr e))))

;;! --- expression emit ---

(define (shader-has-char? s ch)
  (let loop ((i 0))
    (cond ((>= i (string-length s)) #f)
          ((char=? (string-ref s i) ch) #t)
          (else (loop (+ i 1))))))

;;! A GLSL float literal for N: always carries a decimal point (so it reads as
;;! float, not int), integral values print plainly (avoiding 1e+02), and
;;! fractional ones use the fewest digits that still round-trip exactly.
(define (shader-num n)
  (let ((x (exact->inexact n)))
    (if (= x (floor x))
        (string-append (number->string (inexact->exact (floor x))) ".0")
        (let loop ((p 1))
          (let ((s (format #f (string-append "~," (number->string p) "g") x)))
            (if (or (>= p 17) (= (string->number s) x))
                (if (or (shader-has-char? s #\.) (shader-has-char? s #\e)
                        (shader-has-char? s #\E))
                    s
                    (string-append s ".0"))
                (loop (+ p 1))))))))

(define (shader-emit e env)
  (cond
   ((number? e) (shader-num e))
   ((symbol? e) (symbol->string e))
   ((pair? e)
    (let ((op (car e)) (args (cdr e)))
      (case op
        ((vec2 vec3 vec4 mat2 mat3 mat4 float int)
         (string-append (symbol->string op) "("
                        (shader-join ", "
                                     (map (lambda (a) (shader-emit a env)) args))
                        ")"))
        ((swizzle)
         (string-append (shader-emit (car args) env) "."
                        (symbol->string (cadr args))))
        ((+ - * /)
         (if (and (eq? op '-) (= (length args) 1))
             (string-append "(-" (shader-emit (car args) env) ")")
             (string-append "("
                            (shader-join (string-append " " (symbol->string op) " ")
                                         (map (lambda (a) (shader-emit a env)) args))
                            ")")))
        ((sample)
         (string-append "texture("
                        (shader-join ", "
                                     (map (lambda (a) (shader-emit a env)) args))
                        ")"))
        (else
         (string-append (symbol->string op) "("
                        (shader-join ", "
                                     (map (lambda (a) (shader-emit a env)) args))
                        ")")))))
   (else (error 'shader-bad-expr e))))

;;! --- statement emit ---

;;! Emit a statement, returning (cons GLSL-TEXT NEW-ENV). let* locals stay in
;;! NEW-ENV so later sibling statements see them — GLSL has no block here.
(define (shader-emit-stmt s env)
  (case (car s)
    ((set)
     (let* ((target (cadr s))
            (name   (if (eq? target 'position) "gl_Position"
                        (symbol->string target))))
       (cons (string-append "\t" name " = "
                            (shader-emit (caddr s) env) ";\n")
             env)))
    ((let*)
     (let loop ((bs (cadr s)) (env env) (acc ""))
       (if (null? bs)
           (let ((r (shader-emit-stmts (cddr s) env)))
             (cons (string-append acc (car r)) (cdr r)))
           (let* ((b  (car bs))
                  (nm (car b))
                  (ty (shader-infer (cadr b) env)))
             (loop (cdr bs)
                   (cons (cons nm ty) env)
                   (string-append acc "\t" (shader-type->glsl ty) " "
                                  (symbol->string nm) " = "
                                  (shader-emit (cadr b) env) ";\n"))))))
    ((return)
     (cons (string-append "\treturn " (shader-emit (cadr s) env) ";\n")
           env))
    (else (error 'shader-bad-statement s))))

(define (shader-emit-stmts stmts env)
  (if (null? stmts)
      (cons "" env)
      (let* ((r1 (shader-emit-stmt (car stmts) env))
             (r2 (shader-emit-stmts (cdr stmts) (cdr r1))))
        (cons (string-append (car r1) (car r2)) (cdr r2)))))

;;! --- reference analysis ---
;;!
;;! Which identifiers a stage body touches, so a stage only declares the
;;! uniform blocks and varyings it uses. A uniform block declared in two stages
;;! must be byte-identical, and the fragment's `precision mediump float` would
;;! otherwise make its members mediump against the vertex's highp — the link
;;! error that motivates this.

(define (shader-syms-expr e acc)
  (cond ((symbol? e) (cons e acc))
        ((pair? e)
         (if (eq? (car e) 'swizzle)
             (shader-syms-expr (cadr e) acc)
             (shader-foldl (lambda (a x) (shader-syms-expr x a))
                           acc (cdr e))))
        (else acc)))

(define (shader-syms-stmt s acc)
  (case (car s)
    ((set)
     (let ((acc (shader-syms-expr (caddr s) acc)))
       (if (eq? (cadr s) 'position) acc (cons (cadr s) acc))))
    ((let*)
     (let ((acc (shader-foldl (lambda (a b) (shader-syms-expr (cadr b) a))
                              acc (cadr s))))
       (shader-foldl (lambda (a st) (shader-syms-stmt st a)) acc (cddr s))))
    ((return) (shader-syms-expr (cadr s) acc))
    (else acc)))

(define (shader-refs stmts)
  (shader-foldl (lambda (a s) (shader-syms-stmt s a)) '() stmts))

(define (shader-uses? refs name) (and (memq name refs) #t))

;;! --- declaration sections ---

;;! The (KEY ...) subforms of a shader, skipping the leading 'shader and NAME.
(define (shader-section form key)
  (let ((p (assq key (cddr form))))
    (if p (cdr p) '())))

(define (shader-opt-value opts key)
  (let ((p (assq key opts)))
    (and p (cadr p))))

;;! Fields of a uniform block are its (NAME TYPE) forms; (block N)/(layout X)
;;! are options, filtered out here.
(define (shader-block-fields block)
  (shader-keep (lambda (f) (not (memq (car f) '(block layout))))
               (cdr block)))

;;! A (uniforms ...) entry is either a std140 block or a standalone sampler
;;! (NAME sampler2D) — a texture bound to its own unit, not packed into a block.
;;! The cadr tells them apart: a block's is an option list like (block N), a
;;! sampler's is a bare type symbol. Samplers carry no std140 layout and no
;;! cross-stage precision constraint, so they take a separate emit path and never
;;! reach the block/material machinery.
;;! depth2D is a sampler2D that happens to name a depth texture. GL does not care
;;! — it reads depth through a plain sampler2D — but WebGPU does: a Depth32Float
;;! texture cannot bind to a texture_2d<f32>, whose sample type is Float. So the
;;! declaration has to say which textures are depth, and the WGSL path lowers
;;! those to texture_depth_2d instead.
(define shader-sampler-types '(sampler2D depth2D))

(define (shader-depth-sampler-type? t) (eq? t 'depth2D))

(define (shader-uniform-sampler? u)
  (and (pair? (cdr u)) (memq (cadr u) shader-sampler-types) #t))

(define (shader-uniform-blocks form)
  (shader-keep (lambda (u) (not (shader-uniform-sampler? u)))
               (shader-section form 'uniforms)))

(define (shader-uniform-samplers form)
  (shader-keep shader-uniform-sampler? (shader-section form 'uniforms)))

;;! Emit `uniform sampler2D NAME;` for each sampler the stage actually samples
;;! (unused ones are omitted, like unused blocks/varyings). A sampler binds to a
;;! texture unit rather than an interface block, so it needs no layout qualifier
;;! and no highp pin.
(define (shader-emit-samplers samplers refs)
  (apply string-append
         (map (lambda (s)
                (if (shader-uses? refs (car s))
                    (string-append "uniform " (shader-type->glsl (cadr s)) " "
                                   (symbol->string (car s)) ";\n")
                    ""))
              samplers)))

(define (shader-emit-inputs inputs)
  (apply string-append
         (map (lambda (i)
                (string-append "layout(location = "
                               (number->string
                                (shader-opt-value (cddr i) 'location))
                               ") in " (shader-type->glsl (cadr i)) " "
                               (symbol->string (car i)) ";\n"))
              inputs)))

(define (shader-block-used? fields refs)
  (shader-foldl (lambda (acc f) (or acc (shader-uses? refs (car f))))
                #f fields))

;;! Emit only the blocks a stage uses; members are pinned highp so a block
;;! shared across stages stays identical regardless of default precision.
(define (shader-emit-uniforms blocks refs)
  (apply string-append
         (map (lambda (b)
                (let ((fields (shader-block-fields b)))
                  (if (not (shader-block-used? fields refs))
                      ""
                      (string-append
                       "layout(std140) uniform " (symbol->string (car b)) " {\n"
                       (apply string-append
                              (map (lambda (f)
                                     (string-append "\thighp "
                                                    (shader-type->glsl (cadr f))
                                                    " " (symbol->string (car f))
                                                    ";\n"))
                                   fields))
                       "};\n"))))
              blocks)))

(define (shader-emit-varyings varys stage refs)
  (let ((dir (if (eq? stage 'vertex) "out " "in ")))
    (apply string-append
           (map (lambda (v)
                  (if (shader-uses? refs (car v))
                      (string-append dir (shader-type->glsl (cadr v)) " "
                                     (symbol->string (car v)) ";\n")
                      ""))
                varys))))

(define (shader-emit-targets tgts)
  (apply string-append
         (map (lambda (tg)
                (string-append "layout(location = "
                               (number->string
                                (shader-opt-value (cddr tg) 'location))
                               ") out " (shader-type->glsl (cadr tg)) " "
                               (symbol->string (car tg)) ";\n"))
              tgts)))

;;! Identifier→type alist visible inside a stage body: everything declared in
;;! the shared IO model, plus the helper signatures so a call types as its
;;! return. Locals are layered on top during emission.
(define (shader-env form)
  (append
   (shader-fn-sigs form)
   (map (lambda (i) (cons (car i) (cadr i))) (shader-section form 'inputs))
   (apply append
          (map (lambda (b)
                 (map (lambda (f) (cons (car f) (cadr f)))
                      (shader-block-fields b)))
               (shader-uniform-blocks form)))
   (map (lambda (s) (cons (car s) (cadr s)))
        (shader-uniform-samplers form))
   (map (lambda (v) (cons (car v) (cadr v)))
        (shader-section form 'varyings))
   (map (lambda (tg) (cons (car tg) (cadr tg)))
        (shader-section form 'targets))))

;;! --- shader functions (reusable helpers) ---
;;!
;;! A (functions ...) section declares helpers both stages can call, so shared
;;! math (a tonemap curve, a shadow lookup) lives in one place instead of being
;;! copied into every shader that wants it. Each helper is
;;!   (NAME ((PARAM TYPE) ...) RET-TYPE BODY-STMT ...)
;;! with a body of the same set/let*/return statements a stage uses, ending in a
;;! (return EXPR). A helper sees the shared uniform blocks and samplers as
;;! globals — exactly as a stage body does — but takes varyings and per-pixel
;;! values as parameters: it never reads an input, varying, or target directly,
;;! which is what lets both targets lower it the same way (WGSL has no global
;;! varyings to reference). Helpers are emitted before the stage entry point and
;;! only when a reachable call uses them; declare a helper before any other
;;! helper that calls it, since GLSL and WGSL both need the callee in scope
;;! first. A helper that samples a texture is fragment-only (the WGSL lowering
;;! emits fragment-stage textureSample), which covers every shared helper today.
(define (shader-functions form) (shader-section form 'functions))
(define (shader-fn-name f)   (car f))
(define (shader-fn-params f) (cadr f))
(define (shader-fn-ret f)    (caddr f))
(define (shader-fn-body f)   (cdddr f))

;;! Helper names as inference entries — (NAME fn RET-TYPE). shader-infer's call
;;! path reads RET off these (see its else branch), so a call types as the
;;! helper's declared return.
(define (shader-fn-sigs form)
  (map (lambda (f) (list (shader-fn-name f) 'fn (shader-fn-ret f)))
       (shader-functions form)))

;;! Helper names (from FNAMES) called anywhere in expression E. The operator of
;;! a call is E's car, which the plain shader-syms-* walk skips, so calls need
;;! their own collector.
(define (shader-fn-calls-expr e fnames acc)
  (cond ((pair? e)
         (let ((acc (if (memq (car e) fnames) (cons (car e) acc) acc)))
           (if (eq? (car e) 'swizzle)
               (shader-fn-calls-expr (cadr e) fnames acc)
               (shader-foldl (lambda (a x) (shader-fn-calls-expr x fnames a))
                             acc (cdr e)))))
        (else acc)))

(define (shader-fn-calls-stmt s fnames acc)
  (case (car s)
    ((set)    (shader-fn-calls-expr (caddr s) fnames acc))
    ((return) (shader-fn-calls-expr (cadr s) fnames acc))
    ((let*)
     (let ((acc (shader-foldl
                 (lambda (a b) (shader-fn-calls-expr (cadr b) fnames a))
                 acc (cadr s))))
       (shader-foldl (lambda (a st) (shader-fn-calls-stmt st fnames a))
                     acc (cddr s))))
    (else acc)))

(define (shader-fn-calls-stmts stmts fnames)
  (shader-foldl (lambda (a s) (shader-fn-calls-stmt s fnames a)) '() stmts))

;;! The helpers STMTS can reach — directly called, plus helpers those call,
;;! transitively — as a subset of FORM's functions kept in declaration order (so
;;! a callee declared earlier is emitted before its caller). Shaders don't
;;! recurse (GLSL/WGSL forbid it), so the fixpoint always terminates.
(define (shader-reachable-fns form stmts)
  (let ((fns    (shader-functions form))
        (fnames (map shader-fn-name (shader-functions form))))
    (let loop ((seen '()) (frontier (shader-fn-calls-stmts stmts fnames)))
      (if (null? frontier)
          (shader-keep (lambda (f) (memq (shader-fn-name f) seen)) fns)
          (let ((n (car frontier)))
            (if (memq n seen)
                (loop seen (cdr frontier))
                (let* ((f    (assq n fns))
                       (more (if f (shader-fn-calls-stmts
                                    (shader-fn-body f) fnames) '())))
                  (loop (cons n seen) (append (cdr frontier) more)))))))))

;;! The identifiers a stage effectively references: its own body's, plus every
;;! reachable helper's, so a uniform block or sampler a helper reads is still
;;! declared and bound in the stage that calls it. With no (functions ...) this
;;! is exactly shader-refs, leaving a functionless shader byte-for-byte the same.
(define (shader-stage-refs form stmts)
  (shader-foldl (lambda (acc f) (append (shader-refs (shader-fn-body f)) acc))
                (shader-refs stmts)
                (shader-reachable-fns form stmts)))

;;! Identifier→type alist inside a helper body: its parameters, the shared
;;! uniform blocks and samplers it may read as globals, and the other helpers'
;;! signatures. Inputs, varyings, and targets are deliberately absent — a helper
;;! takes those as parameters, which is what keeps it lowerable identically on a
;;! backend with no global varyings.
(define (shader-fn-env form f)
  (append
   (shader-fn-sigs form)
   (map (lambda (p) (cons (car p) (cadr p))) (shader-fn-params f))
   (apply append
          (map (lambda (b)
                 (map (lambda (fld) (cons (car fld) (cadr fld)))
                      (shader-block-fields b)))
               (shader-uniform-blocks form)))
   (map (lambda (s) (cons (car s) (cadr s)))
        (shader-uniform-samplers form))))

;;! One helper as a GLSL function definition.
(define (shader-emit-fn form f)
  (string-append
   (shader-type->glsl (shader-fn-ret f)) " "
   (symbol->string (shader-fn-name f)) "("
   (shader-join ", "
                (map (lambda (p) (string-append (shader-type->glsl (cadr p)) " "
                                                (symbol->string (car p))))
                     (shader-fn-params f)))
   ") {\n"
   (car (shader-emit-stmts (shader-fn-body f) (shader-fn-env form f)))
   "}\n"))

;;! Every helper a stage body reaches, GLSL, in declaration order, ready to sit
;;! above main(). "" when the shader declares (or reaches) none.
(define (shader-emit-fns form stmts)
  (apply string-append
         (map (lambda (f) (string-append (shader-emit-fn form f) "\n"))
              (shader-reachable-fns form stmts))))

;;! --- material parameter introspection ---
;;!
;;! (shader-material-params SRC) reports the shader's Material uniform block as
;;! editable parameters: the std140-packed block size plus, per field, its name,
;;! type, byte offset, byte size, editable float-component count, and edit hint.
;;! The editor derives its widgets from this (a material has no fixed schema of
;;! its own — the shader owns it) and the same offsets pack a material's values.
;;! GLSL emission ignores the optional (edit ...) clause (it reads only a field's
;;! name and type), so annotating a field never changes the compiled shader.

;;! std140 base alignment (bytes) of a block member type.
(define (shader-std140-align t)
  (case t
    ((float int) 4)
    ((vec2) 8)
    ((vec3 vec4) 16)
    ((mat2 mat3 mat4) 16)
    (else 16)))

;;! std140 size (bytes) a block member consumes (a vec3 occupies 12 but aligns
;;! to 16; a matN is N columns each rounded to a vec4).
(define (shader-std140-size t)
  (case t
    ((float int) 4)
    ((vec2) 8)
    ((vec3) 12)
    ((vec4) 16)
    ((mat2) 32)
    ((mat3) 48)
    ((mat4) 64)
    (else 16)))

;;! How many float components the editor exposes for a type (0 = not scalar-
;;! editable, e.g. a matrix — surfaced read-only rather than as sliders).
(define (shader-type-components t)
  (case t
    ((float int) 1)
    ((vec2) 2) ((vec3) 3) ((vec4) 4)
    (else 0)))

;;! Round N up to the next multiple of A (A a power of two here).
(define (shader-round-up n a) (* a (quotient (+ n a -1) a)))

;;! The edit hint of a field as (KIND MIN MAX): ("none" 0 0), ("color" 0 0), or
;;! ("range" MIN MAX). A field is (NAME TYPE) or (NAME TYPE (edit ...)).
(define (shader-field-edit f)
  (let ((e (assq 'edit (cddr f))))
    (cond ((not e) (list "none" 0 0))
          ((eq? (cadr e) 'color) (list "color" 0 0))
          ((eq? (cadr e) 'range) (list "range" (caddr e) (cadddr e)))
          (else (list "none" 0 0)))))

;;! The authored default of a field as a list of component values, or '() when it
;;! declares none. A field may carry (default V ...) alongside its (edit ...) to
;;! seed its un-overridden value independently of the edit hint. GLSL emission
;;! ignores it (like (edit ...)); only the editor/host default resolver reads it.
(define (shader-field-default f)
  (let ((d (assq 'default (cddr f))))
    (if d (cdr d) '())))

;;! The Material block form, or #f when the shader declares none.
(define (shader-material-block form)
  (assq 'Material (shader-section form 'uniforms)))

;;! Walk the Material block into (TOTAL-SIZE (NAME TYPE OFFSET SIZE COMPONENTS
;;! EDIT-KIND EDIT-MIN EDIT-MAX DEFAULT) ...), std140-packed. DEFAULT is the
;;! field's authored (default V ...) values or '() for none. Total is 0 with no
;;! block.
(define (shader-material-params-form form)
  (let ((blk (shader-material-block form)))
    (if (not blk)
        (list 0)
        (let loop ((fields (shader-block-fields blk)) (off 0) (acc '()))
          (if (null? fields)
              (cons (shader-round-up off 16) (reverse acc))
              (let* ((f     (car fields))
                     (ty    (cadr f))
                     (foff  (shader-round-up off (shader-std140-align ty)))
                     (ed    (shader-field-edit f)))
                (loop (cdr fields)
                      (+ foff (shader-std140-size ty))
                      (cons (list (symbol->string (car f))
                                  (symbol->string ty)
                                  foff
                                  (shader-std140-size ty)
                                  (shader-type-components ty)
                                  (car ed) (cadr ed) (caddr ed)
                                  (shader-field-default f))
                            acc))))))))

(define (shader-material-params src)
  (shader-material-params-form
   (with-input-from-string src (lambda () (read)))))

;;! --- fragment precision ---
;;!
;;! GLSL ES makes the fragment stage's default float precision the shader's
;;! problem, and mediump is the right default: it is what every scene shader
;;! wants and what mobile GPUs run fastest. But a shader that samples a signed
;;! distance field and takes fwidth of the result needs the extra mantissa —
;;! kruddgui's text is the case in hand — so a shader may opt in with a
;;! top-level (precision highp).
;;!
;;! This is GLSL-only by construction: WGSL has no precision qualifiers (f32 is
;;! f32), so the WGSL lowering ignores the declaration rather than mapping it to
;;! anything. Omitting it leaves a shader byte-identical to before this existed.
(define (shader-precision form)
  (let ((p (shader-section form 'precision)))
    (if (null? p) "mediump" (symbol->string (car p)))))

;;! --- stage assembly ---

;;! GLSL ES 300 for one stage, or #f when the shader declares no such stage.
(define (shader->glsl form stage)
  (let ((body (assq stage (cddr form))))
    (and body
         (let ((env  (shader-env form))
               (refs (shader-stage-refs form (cdr body))))
           (string-append
            "#version 300 es\n"
            (if (eq? stage 'fragment)
                (string-append "precision " (shader-precision form)
                               " float;\n")
                "")
            (if (eq? stage 'vertex)
                (shader-emit-inputs (shader-section form 'inputs)) "")
            (shader-emit-uniforms (shader-uniform-blocks form) refs)
            (shader-emit-samplers (shader-uniform-samplers form) refs)
            (shader-emit-varyings (shader-section form 'varyings)
                                  stage refs)
            (if (eq? stage 'fragment)
                (shader-emit-targets (shader-section form 'targets)) "")
            (shader-emit-fns form (cdr body))
            "void main() {\n"
            (car (shader-emit-stmts (cdr body) env))
            "}\n")))))

;;! Entry point called from the runtime (via s7_call) and the oracle test.
(define (shader-transpile src stage-str)
  (shader->glsl (with-input-from-string src (lambda () (read)))
                (string->symbol stage-str)))

;;! --- WGSL backend target ---
;;!
;;! A second lowering of the same DSL, for the WebGPU renderer. The GLSL path
;;! above is untouched; this reuses the shared type system, type inference, and
;;! reference analysis but assembles WGSL, which is structurally different: IO
;;! travels in @location/@builtin structs rather than free in/out globals, a
;;! uniform block is a struct plus a @group/@binding var whose members are read
;;! qualified (view_proj -> u_Camera.view_proj), and a sampler splits into a
;;! texture_2d and a companion sampler bound together. shader-transpile-wgsl is
;;! the entry the WebGPU backend calls, mirroring shader-transpile for GLSL.
;;!
;;! Binding scheme (the backend mirrors it when it builds the bind group layout):
;;!   uniform block (block N)  -> @group(0) @binding(N) var<uniform> u_<Name>
;;!   sampler #i (declaration order) -> @group(1) @binding(2i)   texture_2d
;;!                                     @group(1) @binding(2i+1) sampler
;;! A varying's @location is its position in the full (varyings ...) list, so the
;;! vertex output and fragment input agree even when a stage uses only a subset.

;;! (0 1 ... n-1) without leaning on a non-base-s7 iota.
(define (shader-iota n)
  (let loop ((i (- n 1)) (acc '()))
    (if (< i 0) acc (loop (- i 1) (cons i acc)))))

;;! WGSL spelling of a DSL type. Vectors/matrices carry their f32 element type
;;! and matrices spell columns×rows; a sampler2D becomes the sampled texture
;;! (its companion `sampler` is emitted separately, at bind time).
(define (shader-type->wgsl t)
  (case t
    ((float) "f32") ((int) "i32")
    ((vec2) "vec2<f32>") ((vec3) "vec3<f32>") ((vec4) "vec4<f32>")
    ((mat2) "mat2x2<f32>") ((mat3) "mat3x3<f32>") ((mat4) "mat4x4<f32>")
    ((sampler2D) "texture_2d<f32>")
    ((depth2D) "texture_depth_2d")
    (else (symbol->string t))))

(define (shader-mat-cols t)
  (case t ((mat2) 2) ((mat3) 3) ((mat4) 4) (else 0)))

;;! The first N components as a swizzle string ("xyz" for 3), for truncating a
;;! wider matrix's columns down to a smaller matrix.
(define (shader-col-swizzle n) (substring "xyzw" 0 n))

(define (shader-wgsl-uniform-var block-name)
  (string-append "u_" (symbol->string block-name)))

;;! name -> spelling alist for a stage body's leaf identifiers. Block members
;;! resolve through their uniform var, inputs/varyings/targets through the IO
;;! struct (`in.`/`out.`) for that stage, samplers stay bare (they name a
;;! module-scope texture var). let* locals are consed on the front during emit.
(define (shader-wgsl-names form stage)
  ;;! The emitter needs to know its stage at a `sample` (WGSL restricts the
  ;;! implicit-derivative textureSample to the fragment stage), so carry it
  ;;! in the name alist under a key no DSL identifier can spell.
  (cons
   (cons '*wgsl-stage* stage)
   (append
    (apply append
           (map (lambda (b)
                  (let ((var (shader-wgsl-uniform-var (car b))))
                    (map (lambda (f)
                           (cons (car f)
                                 (string-append var "."
                                                (symbol->string (car f)))))
                         (shader-block-fields b))))
                (shader-uniform-blocks form)))
    (map (lambda (s) (cons (car s) (symbol->string (car s))))
         (shader-uniform-samplers form))
    (if (eq? stage 'vertex)
        (append
         (map (lambda (i)
                (cons (car i)
                      (string-append "in." (symbol->string (car i)))))
              (shader-section form 'inputs))
         (map (lambda (v)
                (cons (car v)
                      (string-append "out." (symbol->string (car v)))))
              (shader-section form 'varyings)))
        (append
         (map (lambda (v)
                (cons (car v)
                      (string-append "in." (symbol->string (car v)))))
              (shader-section form 'varyings))
         (map (lambda (tg)
                (cons (car tg)
                      (string-append "out." (symbol->string (car tg)))))
              (shader-section form 'targets)))))))

;;! Emit a WGSL expression under NM (name spelling alist) and TENV (name->type,
;;! for the mat-truncation special case). Mirrors shader-emit's shape; the
;;! divergences are typed constructors, the mat(matN) truncation, textureSample,
;;! and the GLSL-accurate mod expansion.
(define (shader-wgsl-emit e nm tenv)
  (cond
   ((number? e) (shader-num e))
   ((symbol? e)
    (let ((p (assq e nm))) (if p (cdr p) (symbol->string e))))
   ((pair? e)
    (let ((op (car e)) (args (cdr e)))
      (case op
        ((vec2 vec3 vec4)
         (string-append (shader-type->wgsl op) "("
                        (shader-join ", "
                                     (map (lambda (a) (shader-wgsl-emit a nm tenv))
                                          args))
                        ")"))
        ((mat2 mat3 mat4)
         (if (and (= (length args) 1)
                  (shader-mat? (shader-infer (car args) tenv)))
             (let* ((k   (shader-mat-cols op))
                    (swz (shader-col-swizzle k))
                    (m   (shader-wgsl-emit (car args) nm tenv)))
               (string-append (shader-type->wgsl op) "("
                              (shader-join ", "
                                           (map (lambda (i)
                                                  (string-append "(" m ")["
                                                                 (number->string i) "]." swz))
                                                (shader-iota k)))
                              ")"))
             (string-append (shader-type->wgsl op) "("
                            (shader-join ", "
                                         (map (lambda (a)
                                                (shader-wgsl-emit a nm tenv))
                                              args))
                            ")")))
        ((float) (string-append "f32(" (shader-wgsl-emit (car args) nm tenv) ")"))
        ((int)   (string-append "i32(" (shader-wgsl-emit (car args) nm tenv) ")"))
        ((swizzle)
         (string-append (shader-wgsl-emit (car args) nm tenv) "."
                        (symbol->string (cadr args))))
        ((+ - * /)
         (if (and (eq? op '-) (= (length args) 1))
             (string-append "(-" (shader-wgsl-emit (car args) nm tenv) ")")
             (string-append "("
                            (shader-join (string-append " " (symbol->string op) " ")
                                         (map (lambda (a) (shader-wgsl-emit a nm tenv))
                                              args))
                            ")")))
        ((sample)
         ;;! textureSample returns vec4<f32> for a sampled texture but a
         ;;! bare f32 for texture_depth_2d, while the DSL says `sample` is
         ;;! always vec4 (shaders read depth as .r, as they do on GL). So a
         ;;! depth read widens back to vec4 here, keeping one meaning of
         ;;! `sample` across both backends rather than a typing rule that
         ;;! holds only on WebGPU.
         (let* ((tex (shader-wgsl-emit (car args) nm tenv))
                (depth (shader-depth-sampler-type?
                        (shader-infer (car args) tenv)))
                (in-vertex (let ((p (assq '*wgsl-stage* nm)))
                             (and p (eq? (cdr p) 'vertex))))
                (rest (map (lambda (a) (shader-wgsl-emit a nm tenv))
                           (cdr args)))
                (coords (shader-join ", " rest))
                (call (if in-vertex
                          (string-append "textureSampleLevel(" tex ", "
                                         tex "_sampler, " coords ", 0.0)")
                          (string-append "textureSample(" tex ", "
                                         tex "_sampler, " coords ")"))))
           (if depth
               (string-append "vec4<f32>(" call ")")
               call)))
        ((mod)
         ;;! GLSL mod(a,b) = a - b*floor(a/b); WGSL % is fmod-like, so
         ;;! expand to keep the DSL's GLSL semantics on either target.
         (let ((a (shader-wgsl-emit (car args) nm tenv))
               (b (shader-wgsl-emit (cadr args) nm tenv)))
           (string-append "(" a " - " b " * floor(" a " / " b "))")))
        (else
         (string-append (symbol->string op) "("
                        (shader-join ", "
                                     (map (lambda (a) (shader-wgsl-emit a nm tenv))
                                          args))
                        ")")))))
   (else (error 'shader-bad-expr e))))

;;! Emit a statement, returning (cons TEXT (cons NEW-NM NEW-TENV)). A set writes
;;! through the target's IO spelling (position is the vertex builtin); a let*
;;! lowers to immutable `let` bindings that later siblings see (name added bare
;;! to NM, typed to TENV) — the same single-assignment locals GLSL emits.
(define (shader-wgsl-emit-stmt s nm tenv)
  (case (car s)
    ((set)
     (let* ((target   (cadr s))
            (spelling (if (eq? target 'position) "out.position"
                          (let ((p (assq target nm)))
                            (if p (cdr p) (symbol->string target))))))
       (cons (string-append "\t" spelling " = "
                            (shader-wgsl-emit (caddr s) nm tenv) ";\n")
             (cons nm tenv))))
    ((let*)
     (let loop ((bs (cadr s)) (nm nm) (tenv tenv) (acc ""))
       (if (null? bs)
           (let ((r (shader-wgsl-emit-stmts (cddr s) nm tenv)))
             (cons (string-append acc (car r)) (cdr r)))
           (let* ((b   (car bs))
                  (ty  (shader-infer (cadr b) tenv))
                  (rhs (shader-wgsl-emit (cadr b) nm tenv)))
             (loop (cdr bs)
                   (cons (cons (car b) (symbol->string (car b))) nm)
                   (cons (cons (car b) ty) tenv)
                   (string-append acc "\tlet " (symbol->string (car b))
                                  " = " rhs ";\n"))))))
    ((return)
     (cons (string-append "\treturn "
                          (shader-wgsl-emit (cadr s) nm tenv) ";\n")
           (cons nm tenv)))
    (else (error 'shader-bad-statement s))))

(define (shader-wgsl-emit-stmts stmts nm tenv)
  (if (null? stmts)
      (cons "" (cons nm tenv))
      (let* ((r1 (shader-wgsl-emit-stmt (car stmts) nm tenv))
             (r2 (shader-wgsl-emit-stmts (cdr stmts)
                                         (car (cdr r1)) (cdr (cdr r1)))))
        (cons (string-append (car r1) (car r2)) (cdr r2)))))

;;! A used uniform block as a WGSL struct plus its @group(0) @binding(N) var.
;;! All of a used block's fields are emitted (the host uploads the whole std140
;;! block); an unreferenced block is omitted entirely, like the GLSL path.
(define (shader-wgsl-uniform-structs form refs)
  (apply string-append
         (map (lambda (b)
                (let ((fields (shader-block-fields b)))
                  (if (not (shader-block-used? fields refs))
                      ""
                      (let ((name    (symbol->string (car b)))
                            (binding (or (shader-opt-value (cdr b) 'block) 0)))
                        (string-append
                         "struct " name " {\n"
                         (apply string-append
                                (map (lambda (f)
                                       (string-append "\t" (symbol->string (car f))
                                                      " : "
                                                      (shader-type->wgsl (cadr f))
                                                      ",\n"))
                                     fields))
                         "};\n"
                         "@group(0) @binding(" (number->string binding)
                         ") var<uniform> " (shader-wgsl-uniform-var (car b))
                         " : " name ";\n\n")))))
              (shader-uniform-blocks form))))

;;! Each used sampler as a texture var and its companion sampler var. The slot
;;! index (declaration order) fixes the bindings whether or not a sampler is
;;! used, so a stage that skips one keeps the others' numbers stable.
(define (shader-wgsl-sampler-bindings form refs)
  (let loop ((ss (shader-uniform-samplers form)) (i 0) (acc ""))
    (if (null? ss)
        acc
        (let ((s (car ss)))
          (if (shader-uses? refs (car s))
              (let ((name (symbol->string (car s))))
                (loop (cdr ss) (+ i 1)
                      (string-append acc
                                     "@group(1) @binding(" (number->string (* 2 i))
                                     ") var " name " : "
                                     (shader-type->wgsl (cadr s)) ";\n"
                                     "@group(1) @binding(" (number->string (+ (* 2 i) 1))
                                     ") var " name "_sampler : sampler;\n\n")))
              (loop (cdr ss) (+ i 1) acc))))))

;;! The vertex input struct (every declared input; the pipeline's vertex layout
;;! provides them), each field carrying its @location from the IO model.
(define (shader-wgsl-vertex-input form)
  (let ((inputs (shader-section form 'inputs)))
    (if (null? inputs)
        ""
        (string-append
         "struct VertexInput {\n"
         (apply string-append
                (map (lambda (i)
                       (string-append "\t@location("
                                      (number->string
                                       (shader-opt-value (cddr i) 'location))
                                      ") " (symbol->string (car i)) " : "
                                      (shader-type->wgsl (cadr i)) ",\n"))
                     inputs))
         "};\n\n"))))

;;! "@location(POS) name : type,\n" for each USED varying, where POS is the
;;! varying's index in the full list so vertex-out and fragment-in line up.
(define (shader-wgsl-varying-fields form refs)
  (let loop ((vs (shader-section form 'varyings)) (i 0) (acc ""))
    (if (null? vs)
        acc
        (let ((v (car vs)))
          (loop (cdr vs) (+ i 1)
                (if (shader-uses? refs (car v))
                    (string-append acc "\t@location(" (number->string i)
                                   ") " (symbol->string (car v)) " : "
                                   (shader-type->wgsl (cadr v)) ",\n")
                    acc))))))

(define (shader-wgsl-targets-struct form)
  (string-append
   "struct FragmentOutput {\n"
   (apply string-append
          (map (lambda (tg)
                 (string-append "\t@location("
                                (number->string
                                 (shader-opt-value (cddr tg) 'location))
                                ") " (symbol->string (car tg)) " : "
                                (shader-type->wgsl (cadr tg)) ",\n"))
               (shader-section form 'targets)))
   "};\n\n"))

;;! name→spelling alist for a helper body: parameters spell as themselves, block
;;! members and samplers resolve exactly as in a stage (u_Block.field / bare
;;! sampler var), and the stage marker is fragment — a helper that samples is
;;! reached from the fragment stage, where textureSample is legal. Inputs,
;;! varyings, and targets are absent by design (a helper takes them as params).
(define (shader-wgsl-fn-names form f)
  (cons (cons '*wgsl-stage* 'fragment)
        (append
         (map (lambda (p) (cons (car p) (symbol->string (car p))))
              (shader-fn-params f))
         (apply append
                (map (lambda (b)
                       (let ((var (shader-wgsl-uniform-var (car b))))
                         (map (lambda (fld)
                                (cons (car fld)
                                      (string-append var "."
                                                     (symbol->string (car fld)))))
                              (shader-block-fields b))))
                     (shader-uniform-blocks form)))
         (map (lambda (s) (cons (car s) (symbol->string (car s))))
              (shader-uniform-samplers form)))))

;;! One helper as a WGSL function definition.
(define (shader-wgsl-emit-fn form f)
  (string-append
   "fn " (symbol->string (shader-fn-name f)) "("
   (shader-join ", "
                (map (lambda (p) (string-append (symbol->string (car p)) " : "
                                                (shader-type->wgsl (cadr p))))
                     (shader-fn-params f)))
   ") -> " (shader-type->wgsl (shader-fn-ret f)) " {\n"
   (car (shader-wgsl-emit-stmts (shader-fn-body f)
                                (shader-wgsl-fn-names form f)
                                (shader-fn-env form f)))
   "}\n"))

;;! Every helper a stage body reaches, WGSL, in declaration order. Sits above
;;! the entry point and below the uniform/sampler module-scope vars it reads.
;;! "" when the shader declares (or reaches) none.
(define (shader-wgsl-emit-fns form stmts)
  (apply string-append
         (map (lambda (f) (string-append (shader-wgsl-emit-fn form f) "\n"))
              (shader-reachable-fns form stmts))))

;;! WGSL for one stage, or #f when the shader declares no such stage — the same
;;! "matching stage else error" contract shader->glsl honours.
(define (shader->wgsl form stage)
  (let ((body (assq stage (cddr form))))
    (and body
         (let* ((nm       (shader-wgsl-names form stage))
                (tenv     (shader-env form))
                (refs     (shader-stage-refs form (cdr body)))
                (fns      (shader-wgsl-emit-fns form (cdr body)))
                (bodytext (car (shader-wgsl-emit-stmts (cdr body) nm tenv))))
           (if (eq? stage 'vertex)
               (string-append
                (shader-wgsl-uniform-structs form refs)
                fns
                (shader-wgsl-vertex-input form)
                "struct VertexOutput {\n"
                "\t@builtin(position) position : vec4<f32>,\n"
                (shader-wgsl-varying-fields form refs)
                "};\n\n"
                "@vertex\n"
                (if (null? (shader-section form 'inputs))
                    "fn vs_main() -> VertexOutput {\n"
                    "fn vs_main(in : VertexInput) -> VertexOutput {\n")
                "\tvar out : VertexOutput;\n"
                bodytext
                "\treturn out;\n}\n")
               (let ((varyings (shader-wgsl-varying-fields form refs)))
                 (string-append
                  (shader-wgsl-uniform-structs form refs)
                  (shader-wgsl-sampler-bindings form refs)
                  fns
                  (if (string=? varyings "")
                      ""
                      (string-append "struct FragmentInput {\n"
                                     varyings "};\n\n"))
                  (shader-wgsl-targets-struct form)
                  "@fragment\n"
                  (if (string=? varyings "")
                      "fn fs_main() -> FragmentOutput {\n"
                      "fn fs_main(in : FragmentInput) -> FragmentOutput {\n")
                  "\tvar out : FragmentOutput;\n"
                  bodytext
                  "\treturn out;\n}\n")))))))

;;! Entry point the WebGPU backend calls, the WGSL twin of shader-transpile.
(define (shader-transpile-wgsl src stage-str)
  (shader->wgsl (with-input-from-string src (lambda () (read)))
                (string->symbol stage-str)))
