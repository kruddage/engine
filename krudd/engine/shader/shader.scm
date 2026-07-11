; SPDX-License-Identifier: GPL-2.0-or-later

;;! shader — the krudd shader DSL and its transpiler.
;;!
;;! A shader asset is a single (shader NAME ...) S-expression carrying BOTH
;;! stages and a shared IO model (inputs, uniforms, varyings, targets) declared
;;! once. This is the source of truth; GLSL is a backend target the renderer
;;! lowers to at bind time. The same file is embedded into the runtime image so
;;! the web editor transpiles on the fly, and loaded at build time by the
;;! Scheme oracle test — write once, run in both hosts.
;;!
;;! (shader-transpile SRC STAGE) parses the DSL text SRC and returns the
;;! GLSL ES 3.00 for STAGE ("vertex" or "fragment"), or #f when the shader has
;;! no such stage — the "matching stage else error" contract the renderer wants.

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
(define (shader-type->glsl t) (symbol->string t))

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
	       ((cross) 'vec3)
	       ((normalize sin cos tan sqrt abs floor fract exp log
		 radians degrees)
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
	       (else (error 'shader-unknown-op op)))))
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
;;! the shared IO model. Locals are layered on top during emission.
(define (shader-env form)
	(append
	  (map (lambda (i) (cons (car i) (cadr i))) (shader-section form 'inputs))
	  (apply append
		 (map (lambda (b)
			(map (lambda (f) (cons (car f) (cadr f)))
			     (shader-block-fields b)))
		      (shader-section form 'uniforms)))
	  (map (lambda (v) (cons (car v) (cadr v)))
	       (shader-section form 'varyings))
	  (map (lambda (tg) (cons (car tg) (cadr tg)))
	       (shader-section form 'targets))))

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

;;! --- stage assembly ---

;;! GLSL ES 300 for one stage, or #f when the shader declares no such stage.
(define (shader->glsl form stage)
	(let ((body (assq stage (cddr form))))
	  (and body
	       (let ((env  (shader-env form))
		     (refs (shader-refs (cdr body))))
		 (string-append
		   "#version 300 es\n"
		   (if (eq? stage 'fragment) "precision mediump float;\n" "")
		   (if (eq? stage 'vertex)
		       (shader-emit-inputs (shader-section form 'inputs)) "")
		   (shader-emit-uniforms (shader-section form 'uniforms) refs)
		   (shader-emit-varyings (shader-section form 'varyings)
					 stage refs)
		   (if (eq? stage 'fragment)
		       (shader-emit-targets (shader-section form 'targets)) "")
		   "void main() {\n"
		   (car (shader-emit-stmts (cdr body) env))
		   "}\n")))))

;;! Entry point called from the runtime (via s7_call) and the oracle test.
(define (shader-transpile src stage-str)
	(shader->glsl (with-input-from-string src (lambda () (read)))
		      (string->symbol stage-str)))
