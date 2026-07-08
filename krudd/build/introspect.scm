; SPDX-License-Identifier: GPL-2.0-or-later
;
; introspect.scm — the build-time introspection krudd owns.
;
; The root build spec used to hand CMake a block of imperative bootstrap
; verbatim: read the VERSION file, run git to derive the build number and commit
; hash, and FetchContent-clone imgui. Those are things CMake did for us at
; configure time; the Ninja backend has no execute_process or FetchContent, so
; krudd has to own them. This module is that ownership — plain Scheme that shells
; out (via s7's system for captured output, and the krudd `run` primitive for
; side-effecting commands) so both backends can bake the same values into their
; generated build files.
;
; It depends only on s7 built-ins plus, for the fetch path, the `run` primitive
; krudd.c installs — and `run` is touched only when a dependency actually needs
; cloning, so the version/git helpers work under a bare s7 too.

;; ---------------------------------------------------------------------------
;; String + file helpers (kept local, as the other krudd Scheme modules do).
;; ---------------------------------------------------------------------------

(define (krudd-whitespace? c)
	(or (char=? c #\space) (char=? c #\tab)
	    (char=? c #\newline) (char=? c #\return)))

;; Trim leading and trailing whitespace — the moral equivalent of CMake's
;; string(STRIP), for the newline git and file reads leave behind.
(define (krudd-strip s)
	(let* ((n (string-length s))
	       (start (let loop ((i 0))
			(if (and (< i n) (krudd-whitespace? (string-ref s i)))
			    (loop (+ i 1)) i)))
	       (end (let loop ((i n))
		      (if (and (> i start)
			       (krudd-whitespace? (string-ref s (- i 1))))
			  (loop (- i 1)) i))))
		(if (>= start end) "" (substring s start end))))

;; Slurp a whole file to a string (VERSION is a single short line, but keep it
;; general).
(define (krudd-slurp path)
	(call-with-input-file path
	  (lambda (port)
	    (let loop ((cs '()) (c (read-char port)))
	      (if (eof-object? c)
		  (list->string (reverse cs))
		  (loop (cons c cs) (read-char port)))))))

(define (krudd-path-exists? p)
	(if (defined? 'file-exists?)
	    (file-exists? p)
	    (= 0 (system (string-append "test -e \"" p "\"")))))

;; ---------------------------------------------------------------------------
;; Repository facts.
;; ---------------------------------------------------------------------------

(define (krudd-repo-root) (or (getenv "KRUDD_ROOT") "."))

;; The version string, read and stripped from the repo-root VERSION file.
(define (krudd-version)
	(krudd-strip (krudd-slurp (string-append (krudd-repo-root) "/VERSION"))))

;; Run git in the repo root and return its captured, stripped stdout ("" on
;; failure — git absent, not a repo, empty result), mirroring how CMake fell
;; back to defaults when GIT_FOUND was false or a command produced nothing.
(define (krudd-git args)
	(krudd-strip
	  (system (string-append "cd \"" (krudd-repo-root) "\" && git " args
				 " 2>/dev/null")
		  #t)))

;; The build number: commits since the current version was introduced, found by
;; the same VERSION-anchor walk the CMake bootstrap did. Defaults to "0".
(define (krudd-build-number version)
	(let ((anchor (krudd-git
			(string-append "log -1 --format=%H -S \"" version
				       "\" -- VERSION"))))
		(if (> (string-length anchor) 0)
		    (let ((count (krudd-git
				   (string-append "rev-list --count HEAD ^"
						  anchor))))
		      (if (> (string-length count) 0) count "0"))
		    "0")))

;; The short commit hash, or "unknown" when git can't answer.
(define (krudd-commit-hash)
	(let ((h (krudd-git "rev-parse --short HEAD")))
		(if (> (string-length h) 0) h "unknown")))

;; ---------------------------------------------------------------------------
;; Dependency fetch: krudd's replacement for FetchContent. Clone REPO at the
;; pinned TAG into build/_deps/<name> if it is not already there, and return the
;; directory (the ${<name>_SOURCE_DIR} the specs reference). Idempotent: a
;; complete checkout is left untouched, so repeat builds do no network I/O.
;; ---------------------------------------------------------------------------

(define (krudd-fetch-dir name)
	(string-append (krudd-repo-root) "/build/_deps/" name))

;; A checkout counts as present only if its .git is there — an empty or
;; half-cloned directory (an interrupted clone, or a stray mkdir) must not pass
;; for a real one, or the build inherits a broken dependency.
(define (krudd-fetch name repo tag)
	(let ((dir (krudd-fetch-dir name)))
		(if (not (krudd-path-exists? (string-append dir "/.git")))
		    (run (string-append "rm -rf \"" dir "\" && git clone --depth 1 "
					"--branch " tag " " repo " \"" dir "\"")))
		dir))

;; ---------------------------------------------------------------------------
;; Codegen: krudd's replacements for CMake's configure_file and its embed
;; embed script — the file transforms the build needs generated before
;; compiling. The emitter calls these at synthesis time (as CMake ran them at
;; configure time) and references the outputs.
;; ---------------------------------------------------------------------------

;; Split "6.3.2" into ("6" "3" "2").
(define (krudd-split s ch)
	(let loop ((i 0) (start 0) (out '()))
		(cond ((>= i (string-length s))
		       (reverse (cons (substring s start i) out)))
		      ((char=? (string-ref s i) ch)
		       (loop (+ i 1) (+ i 1) (cons (substring s start i) out)))
		      (else (loop (+ i 1) start out)))))

;; The @VAR@ substitutions the version templates use, mirroring the CMake
;; variables project() and git-build-info supply.
(define (krudd-template-values)
	(let* ((version (krudd-version))
	       (parts   (krudd-split version #\.)))
		(list (cons "PROJECT_NAME" "krudd")
		      (cons "PROJECT_VERSION" version)
		      (cons "PROJECT_VERSION_MAJOR" (list-ref parts 0))
		      (cons "PROJECT_VERSION_MINOR" (list-ref parts 1))
		      (cons "PROJECT_VERSION_PATCH" (list-ref parts 2))
		      (cons "ENGINE_BUILD_NUMBER" (krudd-build-number version))
		      (cons "GIT_COMMIT_HASH" (krudd-commit-hash)))))

;; Replace every occurrence of OLD in S with NEW.
(define (krudd-replace s old new)
	(let ((ol (string-length old)))
		(let loop ((i 0) (out ""))
		  (cond ((> (+ i ol) (string-length s))
			 (string-append out (substring s i)))
			((string=? (substring s i (+ i ol)) old)
			 (loop (+ i ol) (string-append out new)))
			(else (loop (+ i 1)
				    (string-append out
						   (string (string-ref s i)))))))))

;; configure_file: expand @VAR@ occurrences in the template at IN and write the
;; result to OUT. (@ONLY semantics — only @VAR@, never ${VAR} — which is all the
;; version.h.in / shell.html.in templates use.)
(define (krudd-configure-file in out)
	(let loop ((text (krudd-slurp in)) (vals (krudd-template-values)))
		(if (null? vals)
		    (call-with-output-file out
		      (lambda (port) (write-string text port)))
		    (loop (krudd-replace text (string-append "@" (caar vals) "@")
					 (cdar vals))
			  (cdr vals)))))

;; Two hex nibbles for a byte value 0..255.
(define (krudd-hex-byte b)
	(let ((digits "0123456789abcdef"))
		(string (string-ref digits (quotient b 16))
			(string-ref digits (remainder b 16)))))

;; File embed: bake the file at IN into a NUL-terminated char array under
;; SYMBOL, each byte written as a 0xNN element (so non-ASCII bytes and quotes
;; survive and md_parse()/script_eval() get a valid string), and write the
;; header to OUT.
;;
;; A byte array rather than a string literal on purpose: C99 only guarantees
;; 4095 chars per string literal and gcc -Wpedantic -Werror enforces it (via
;; -Woverlength-strings, which counts the concatenated length, so splitting
;; into adjacent literals does not help). An array initializer carries no such
;; limit. Each element is written as (char)0xNN: char is signed on native gcc,
;; so a byte over 127 (a UTF-8 em-dash, say) would trip -Werror=overflow
;; without the explicit narrowing cast — clang for WASM has unsigned char and
;; never hit it, but md_parse.scm is compiled natively for the Scheme port.
;; Format the bytes of S as a C array-initializer body: each byte as (char)0xNN
;; (see the rationale above), twelve per line, terminated with an explicit
;; (char)0x00 so the embedded string stays NUL-terminated. Shared by the
;; runtime header embed and the md_parse shim's baked-in image.
(define (krudd-bytes->c-init s)
	(let* ((n    (string-length s))
	       (body (let loop ((i 0) (acc ""))
		       (if (>= i n)
			   acc
			   (loop (+ i 1)
				 (string-append acc
				   "(char)0x" (krudd-hex-byte
						(char->integer
						  (string-ref s i)))
				   ","
				   (if (= 0 (modulo (+ i 1) 12)) "\n\t" "")))))))
		(string-append body "(char)0x00")))

(define (krudd-embed-file in out symbol)
	(let ((init (krudd-bytes->c-init (krudd-slurp in))))
		(call-with-output-file out
		  (lambda (port)
		    (write-string
		      (string-append
			"/* SPDX-License-Identifier: GPL-2.0-or-later */\n"
			"/* Generated by krudd. Do not edit. */\n"
			"#ifndef " symbol "_H\n"
			"#define " symbol "_H\n\n"
			"static const char " symbol "[] = {\n\t"
			init "\n};\n\n"
			"#endif /* " symbol "_H */\n")
		      port)))))

;; ===========================================================================
;; Scheme -> C binding generator.
;;
;; The next turn of the C-binds-generated-Scheme screw. Rather than hand-write
;; the C ABI header and the marshaling shim beside a ported module, krudd reads
;; an ABI *declaration* embedded in the Scheme file — (define-c-struct ...),
;; (define-c-export ...), plus the plain (define <name> <int>) constants — and
;; emits both C files from it. The .scm is then the only ABI artifact in git;
;; the header and the shim are build outputs.
;;
;; The generator is driven entirely by the declaration it reads; nothing here
;; knows md_parse's fields. Only the wiring (which .scm to run it on, over in
;; ninja.scm's codegen) is module-specific for now. A later PR promotes this to
;; a general `scheme-module` build-spec form; this is the machinery it will use.
;;
;; The declaration forms are no-ops when the module is loaded into s7 (the file
;; defines them as do-nothing macros first), so the same text is both the
;; runtime image the shim bakes in and the ABI source of truth.
;;
;; The locked binding vocabulary — the whole surface; anything else is a loud
;; build error, never silently extended:
;;   scalars      i8 i16 i32 i64 / u8 u16 u32 u64 (s7_integer, narrowing cast);
;;                f32 f64 (float/double, s7_real)
;;   (char N)     char name[N], NUL-terminated; N a constant symbol or literal
;;   (vector T N) T name[N] with a sibling u32/i32 count field; T a scalar or a
;;                declared struct; one level deep only
;;   struct       (define-c-struct name (field kind) ...); fields marshal
;;                positionally from the returned list, count fields skipped
;;   export       (define-c-export (proc (arg kind) ... -> return) (calls sp));
;;                arg kinds string/scalar; return (vector T max) ->
;;                describe(out,max)->count, or a bare scalar; on error returns 0
;;   constants    (define <kebab> <int>) -> #define <SCREAMING_SNAKE> <int>
;; Out of scope (fail loud): pointers/opaque handles, callbacks, structs passed
;; INTO Scheme (reverse marshal), unions, bitfields, heap/varlen, nested vectors.
;; ===========================================================================

;; --- small helpers -----------------------------------------------------------

(define (krudd-filter pred lst)
	(cond ((null? lst) '())
	      ((pred (car lst)) (cons (car lst) (krudd-filter pred (cdr lst))))
	      (else (krudd-filter pred (cdr lst)))))

(define (krudd-join sep lst)
	(cond ((null? lst) "")
	      ((null? (cdr lst)) (car lst))
	      (else (string-append (car lst) sep (krudd-join sep (cdr lst))))))

(define (krudd-upcase s)
	(list->string (map char-upcase (string->list s))))

;; kebab-case symbol/string -> snake_case string (struct/field/proc names).
(define (krudd-snake s)
	(krudd-replace (if (symbol? s) (symbol->string s) s) "-" "_"))

;; kebab -> SCREAMING_SNAKE (constants and include guards).
(define (krudd-screaming s) (krudd-upcase (krudd-snake s)))

;; The last path component of PATH.
(define (krudd-basename path)
	(let loop ((i (- (string-length path) 1)))
	  (cond ((< i 0) path)
		((char=? (string-ref path i) #\/) (substring path (+ i 1)))
		(else (loop (- i 1))))))

;; PATH with its final .extension removed.
(define (krudd-strip-ext s)
	(let loop ((i (- (string-length s) 1)))
	  (cond ((< i 0) s)
		((char=? (string-ref s i) #\.) (substring s 0 i))
		(else (loop (- i 1))))))

;; A repo-relative label for a generated-from banner ("krudd/build/modules/...").
(define (krudd-rel-label path)
	(let ((pfx (string-append (krudd-repo-root) "/")))
	  (if (and (>= (string-length path) (string-length pfx))
		   (string=? (substring path 0 (string-length pfx)) pfx))
	      (substring path (string-length pfx))
	      (krudd-basename path))))

;; Every top-level form of the module at PATH, read (not evaluated) in order.
(define (krudd-scm-forms path)
	(call-with-input-file path
	  (lambda (port)
	    (let loop ((acc '()))
	      (let ((form (read port)))
		(if (eof-object? form) (reverse acc)
		    (loop (cons form acc))))))))

;; Every top-level (define <symbol> <integer>) in the file at PATH, as an alist
;; of (symbol . value). The forms are read, not evaluated: the constants are
;; plain integer literals, so no parser code runs at build time.
(define (krudd-scm-int-defs path)
	(krudd-filter
	  (lambda (x) x)
	  (map (lambda (form)
		 (and (pair? form) (eq? (car form) 'define)
		      (pair? (cdr form)) (symbol? (cadr form))
		      (pair? (cddr form)) (integer? (caddr form))
		      (cons (cadr form) (caddr form))))
	       (krudd-scm-forms path))))

;; --- binding vocabulary ------------------------------------------------------

(define krudd-scalar-ctypes
	'((i8 . "int8_t")  (i16 . "int16_t")  (i32 . "int32_t")  (i64 . "int64_t")
	  (u8 . "uint8_t") (u16 . "uint16_t") (u32 . "uint32_t") (u64 . "uint64_t")
	  (f32 . "float")  (f64 . "double")))

(define (krudd-scalar? k) (and (symbol? k) (assq k krudd-scalar-ctypes) #t))
(define (krudd-float-kind? k) (or (eq? k 'f32) (eq? k 'f64)))
(define (krudd-scalar-ctype k)
	(let ((p (assq k krudd-scalar-ctypes)))
	  (if p (cdr p) (error 'krudd-abi-bad-scalar k))))

(define (krudd-char-kind? k)   (and (pair? k) (eq? (car k) 'char)))
(define (krudd-vector-kind? k) (and (pair? k) (eq? (car k) 'vector)))

;; A C array bound: a constant symbol renders as its #define name, an integer as
;; the literal — either way a valid C constant expression.
(define (krudd-c-size n)
	(cond ((symbol? n) (krudd-screaming n))
	      ((and (integer? n) (>= n 0)) (number->string n))
	      (else (error 'krudd-abi-bad-size n))))

;; The C element type for a scalar or declared-struct kind T ("struct <snake>").
(define (krudd-elem-ctype names t)
	(cond ((krudd-scalar? t) (krudd-scalar-ctype t))
	      ((and (symbol? t) (memq t names))
	       (string-append "struct " (krudd-snake t)))
	      (else (error 'krudd-abi-bad-type t))))

;; --- declaration accessors ---------------------------------------------------

(define (krudd-abi-structs forms)
	(krudd-filter (lambda (f) (and (pair? f) (eq? (car f) 'define-c-struct)))
		      forms))
(define (krudd-abi-exports forms)
	(krudd-filter (lambda (f) (and (pair? f) (eq? (car f) 'define-c-export)))
		      forms))

(define (krudd-struct-name s)   (cadr s))
(define (krudd-struct-fields s) (cddr s))

;; A vector field carries a third element — the count field it derives — which
;; ordinary (name kind) fields lack.
(define (krudd-field-count-field f) (and (pair? (cddr f)) (caddr f)))

(define (krudd-export-sig e)  (cadr e))
(define (krudd-export-proc e) (car (krudd-export-sig e)))

;; Split an export signature on -> into (args . return-spec).
(define (krudd-export-parts e)
	(let loop ((l (cdr (krudd-export-sig e))) (args '()))
	  (cond ((null? l) (error 'krudd-abi-export-no-arrow e))
		((eq? (car l) '->) (cons (reverse args) (cadr l)))
		(else (loop (cdr l) (cons (car l) args))))))

;; The Scheme procedure an export calls, as a C string, from its (calls sp).
(define (krudd-export-calls e)
	(let ((c (caddr e)))
	  (if (and (pair? c) (eq? (car c) 'calls))
	      (symbol->string (cadr c))
	      (error 'krudd-abi-export-no-calls e))))

(define (krudd-export-return-vector? ret)
	(and (pair? ret) (eq? (car ret) 'vector)))

;; --- fail loud: reject anything outside the vocabulary before emitting -------

(define (krudd-abi-check structs exports names)
	(for-each (lambda (s)
		    (for-each (lambda (f) (krudd-check-field names s f))
			      (krudd-struct-fields s)))
		  structs)
	(for-each (lambda (e) (krudd-check-export names e)) exports))

(define (krudd-check-field names s f)
	(if (not (and (pair? f) (symbol? (car f))))
	    (error 'krudd-abi-bad-field f))
	(let ((kind (cadr f)))
	  (cond
	    ((krudd-scalar? kind) #t)
	    ((krudd-char-kind? kind)
	     (or (symbol? (cadr kind)) (integer? (cadr kind))
		 (error 'krudd-abi-bad-char kind)))
	    ((krudd-vector-kind? kind)
	     (let ((t (cadr kind)) (cf (krudd-field-count-field f)))
	       (if (not cf) (error 'krudd-abi-vector-needs-count f))
	       (if (krudd-vector-kind? t) (error 'krudd-abi-nested-vector f))
	       (if (not (or (krudd-scalar? t) (memq t names)))
		   (error 'krudd-abi-bad-vector-type t))
	       (krudd-c-size (caddr kind))
	       (krudd-check-count-field s cf)))
	    (else (error 'krudd-abi-unsupported-kind kind)))))

;; A vector's count field must name a sibling declared u32/i32.
(define (krudd-check-count-field s cf)
	(let ((sib (krudd-filter (lambda (g) (eq? (car g) cf))
				 (krudd-struct-fields s))))
	  (if (not (and (pair? sib) (memq (cadr (car sib)) '(u32 i32))))
	      (error 'krudd-abi-bad-count-field cf))))

(define (krudd-check-export names e)
	(let* ((parts (krudd-export-parts e))
	       (args  (car parts))
	       (ret   (cdr parts)))
	  (for-each
	    (lambda (a)
	      (if (not (and (pair? a) (symbol? (car a))
			    (or (eq? (cadr a) 'string) (krudd-scalar? (cadr a)))))
		  (error 'krudd-abi-bad-export-arg a)))
	    args)
	  (cond ((krudd-export-return-vector? ret)
		 (let ((t (cadr ret)))
		   (if (not (or (krudd-scalar? t) (memq t names)))
		       (error 'krudd-abi-bad-return t))))
		((krudd-scalar? ret) #t)
		(else (error 'krudd-abi-bad-return ret)))
	  (krudd-export-calls e)))

;; --- header emission ---------------------------------------------------------

(define (krudd-header-field names f)
	(let ((cname (krudd-snake (car f)))
	      (kind  (cadr f)))
	  (cond
	    ((krudd-scalar? kind)
	     (string-append "\t" (krudd-scalar-ctype kind) " " cname ";"))
	    ((krudd-char-kind? kind)
	     (string-append "\tchar " cname "[" (krudd-c-size (cadr kind)) "];"))
	    ((krudd-vector-kind? kind)
	     (string-append "\t" (krudd-elem-ctype names (cadr kind)) " "
			    cname "[" (krudd-c-size (caddr kind)) "];"))
	    (else (error 'krudd-abi-bad-kind kind)))))

(define (krudd-header-struct names s)
	(string-append
	  "struct " (krudd-snake (krudd-struct-name s)) " {\n"
	  (krudd-join "\n" (map (lambda (f) (krudd-header-field names f))
				(krudd-struct-fields s)))
	  "\n};"))

(define (krudd-c-param arg)
	(let ((nm (krudd-snake (car arg))) (kind (cadr arg)))
	  (cond ((eq? kind 'string) (string-append "const char *" nm))
		((krudd-scalar? kind)
		 (string-append (krudd-scalar-ctype kind) " " nm))
		(else (error 'krudd-abi-bad-arg kind)))))

(define (krudd-export-prototype names e)
	(let* ((parts  (krudd-export-parts e))
	       (ret    (cdr parts))
	       (proc   (krudd-snake (krudd-export-proc e)))
	       (params (map krudd-c-param (car parts))))
	  (if (krudd-export-return-vector? ret)
	      (string-append
		"int32_t " proc "("
		(krudd-join ", "
		  (append params
			  (list (string-append (krudd-elem-ctype names (cadr ret))
					       " *out")
				(string-append "int32_t "
					       (krudd-snake (caddr ret))))))
		");")
	      (string-append (krudd-scalar-ctype ret) " " proc "("
			     (krudd-join ", " params) ");"))))

(define (krudd-gen-header out header-name structs exports ints names label)
	(let ((guard (krudd-upcase (krudd-replace header-name "." "_"))))
	  (call-with-output-file out
	    (lambda (port)
	      (write-string
		(string-append
		  "/* SPDX-License-Identifier: GPL-2.0-or-later */\n"
		  "/* Generated by krudd from " label ". Do not edit. */\n"
		  "#ifndef " guard "\n"
		  "#define " guard "\n\n"
		  "#include <stdint.h>\n\n"
		  "#ifdef __cplusplus\n"
		  "extern \"C\" {\n"
		  "#endif\n\n"
		  (krudd-join "\n"
		    (map (lambda (d)
			   (string-append "#define " (krudd-screaming (car d)) " "
					  (number->string (cdr d))))
			 ints))
		  "\n\n"
		  (krudd-join "\n\n"
		    (map (lambda (s) (krudd-header-struct names s)) structs))
		  "\n\n"
		  (krudd-join "\n"
		    (map (lambda (e) (krudd-export-prototype names e)) exports))
		  "\n\n"
		  "#ifdef __cplusplus\n"
		  "}\n"
		  "#endif\n\n"
		  "#endif /* " guard " */\n")
		port)))))

;; --- shim emission -----------------------------------------------------------

;; Read the s7 value in EXPR as the scalar kind, narrowing to the C type.
(define (krudd-scalar-read kind expr)
	(string-append "(" (krudd-scalar-ctype kind) ")"
		       (if (krudd-float-kind? kind) "s7_real(" "s7_integer(")
		       expr ")"))

;; Marshal one already-pulled list value (in `f`) into out->NAME.
(define (krudd-marshal-field base names f)
	(let ((cname (krudd-snake (car f)))
	      (kind  (cadr f)))
	  (cond
	    ((krudd-scalar? kind)
	     (string-append "\tout->" cname " = " (krudd-scalar-read kind "f")
			    ";\n"))
	    ((krudd-char-kind? kind)
	     (let ((sz (krudd-c-size (cadr kind))))
	       (string-append
		 "\tif (s7_is_string(f)) {\n"
		 "\t\tconst char *s = s7_string(f);\n"
		 "\t\tsize_t n = strlen(s);\n\n"
		 "\t\tif (n > " sz " - 1)\n"
		 "\t\t\tn = " sz " - 1;\n"
		 "\t\tmemcpy(out->" cname ", s, n);\n"
		 "\t\tout->" cname "[n] = '\\0';\n"
		 "\t}\n")))
	    ((krudd-vector-kind? kind)
	     (let ((t  (cadr kind))
		   (sz (krudd-c-size (caddr kind)))
		   (cf (krudd-snake (krudd-field-count-field f))))
	       (string-append
		 "\t{\n"
		 "\t\tuint32_t n = 0;\n\n"
		 "\t\twhile (s7_is_pair(f) && n < " sz ") {\n"
		 (if (krudd-scalar? t)
		     (string-append "\t\t\tout->" cname "[n] = "
				    (krudd-scalar-read t "s7_car(f)") ";\n")
		     (string-append "\t\t\t" base "_marshal_" (krudd-snake t)
				    "(s7, s7_car(f), &out->" cname "[n]);\n"))
		 "\t\t\tn++;\n"
		 "\t\t\tf = s7_cdr(f);\n"
		 "\t\t}\n"
		 "\t\tout->" cf " = n;\n"
		 "\t}\n")))
	    (else (error 'krudd-abi-bad-kind kind)))))

(define (krudd-gen-marshaler base names s)
	(let* ((cname  (krudd-snake (krudd-struct-name s)))
	       (fields (krudd-struct-fields s))
	       (counts (krudd-filter (lambda (x) x)
				     (map krudd-field-count-field fields)))
	       (data   (krudd-filter (lambda (f) (not (memq (car f) counts)))
				     fields))
	       (arity  (length data)))
	  (string-append
	    "static void " base "_marshal_" cname
	    "(s7_scheme *s7, s7_pointer v,\n\t\tstruct " cname " *out)\n"
	    "{\n"
	    (if (> arity 0) "\ts7_pointer f;\n\n" "")
	    "\tmemset(out, 0, sizeof(*out));\n"
	    "\tif (!s7_is_pair(v))\n\t\treturn;\n"
	    "\tassert(s7_list_length(s7, v) == " (number->string arity) ");\n"
	    (if (> arity 0) "\n" "")
	    (krudd-join ""
	      (map (lambda (f)
		     (string-append "\tf = s7_car(v);\n\tv = s7_cdr(v);\n"
				    (krudd-marshal-field base names f)))
		   data))
	    "}\n")))

;; The s7 argument list for a call: s7_nil for none, else s7_list(s7, N, ...).
(define (krudd-s7-arglist args)
	(if (null? args)
	    "s7_nil(s7)"
	    (string-append
	      "s7_list(s7, " (number->string (length args)) ",\n\t\t\t\t"
	      (krudd-join ",\n\t\t\t\t"
		(map (lambda (a)
		       (let ((nm (krudd-snake (car a))) (kind (cadr a)))
			 (cond ((eq? kind 'string)
				(string-append "s7_make_string(s7, " nm ")"))
			       ((krudd-float-kind? kind)
				(string-append "s7_make_real(s7, " nm ")"))
			       (else
				(string-append "s7_make_integer(s7, " nm ")")))))
		     args))
	      ")")))

;; The NULL/bounds guard: every string arg plus, for a vector return, the out
;; buffer and a positive max.
(define (krudd-guard args vector? maxname)
	(krudd-join " || "
	  (append
	    (krudd-filter (lambda (x) x)
	      (map (lambda (a)
		     (and (eq? (cadr a) 'string)
			  (string-append "!" (krudd-snake (car a)))))
		   args))
	    (if vector?
		(list "!out" (string-append maxname " <= 0"))
		'()))))

(define (krudd-gen-driver base names e)
	(let* ((parts (krudd-export-parts e))
	       (args  (car parts))
	       (ret   (cdr parts))
	       (proc  (krudd-snake (krudd-export-proc e)))
	       (calls (krudd-export-calls e)))
	  (if (krudd-export-return-vector? ret)
	      (krudd-gen-driver-vector base names proc args ret calls)
	      (krudd-gen-driver-scalar base proc args ret calls))))

(define (krudd-gen-driver-vector base names proc args ret calls)
	(let* ((t       (cadr ret))
	       (maxname (krudd-snake (caddr ret)))
	       (params  (append (map krudd-c-param args)
				(list (string-append (krudd-elem-ctype names t)
						     " *out")
				      (string-append "int32_t " maxname))))
	       (elem    (if (krudd-scalar? t)
			    (string-append "out[count] = "
					   (krudd-scalar-read t "s7_car(res)") ";")
			    (string-append base "_marshal_" (krudd-snake t)
					   "(s7, s7_car(res), &out[count]);"))))
	  (string-append
	    "int32_t " proc "(" (krudd-join ", " params) ")\n"
	    "{\n"
	    "\ts7_scheme *s7;\n"
	    "\ts7_pointer proc, res;\n"
	    "\tint32_t    count = 0;\n\n"
	    "\tif (" (krudd-guard args #t maxname) ")\n\t\treturn 0;\n\n"
	    "\ts7 = " base "_ensure();\n"
	    "\tif (!s7)\n\t\treturn 0;\n\n"
	    "\tproc = s7_name_to_value(s7, \"" calls "\");\n"
	    "\tif (!s7_is_procedure(proc))\n\t\treturn 0;\n\n"
	    "\tres = s7_call(s7, proc, " (krudd-s7-arglist args) ");\n\n"
	    "\twhile (count < " maxname " && s7_is_pair(res)) {\n"
	    "\t\t" elem "\n"
	    "\t\tcount++;\n"
	    "\t\tres = s7_cdr(res);\n"
	    "\t}\n"
	    "\treturn count;\n"
	    "}\n")))

(define (krudd-gen-driver-scalar base proc args ret calls)
	(let ((rtype (krudd-scalar-ctype ret))
	      (guard (krudd-guard args #f "")))
	  (string-append
	    rtype " " proc "(" (krudd-join ", " (map krudd-c-param args)) ")\n"
	    "{\n"
	    "\ts7_scheme *s7;\n"
	    "\ts7_pointer proc, res;\n\n"
	    (if (string=? guard "")
		""
		(string-append "\tif (" guard ")\n\t\treturn 0;\n\n"))
	    "\ts7 = " base "_ensure();\n"
	    "\tif (!s7)\n\t\treturn 0;\n\n"
	    "\tproc = s7_name_to_value(s7, \"" calls "\");\n"
	    "\tif (!s7_is_procedure(proc))\n\t\treturn 0;\n\n"
	    "\tres = s7_call(s7, proc, " (krudd-s7-arglist args) ");\n"
	    "\treturn " (krudd-scalar-read ret "res") ";\n"
	    "}\n")))

(define (krudd-gen-shim out header-name base image structs exports names label)
	(call-with-output-file out
	  (lambda (port)
	    (write-string
	      (string-append
		"/* SPDX-License-Identifier: GPL-2.0-or-later */\n"
		"/* Generated by krudd from " label ". Do not edit. */\n"
		"#include \"" header-name "\"\n\n"
		"#include \"script.h\"\n\n"
		"#include \"s7.h\"\n\n"
		"#include <assert.h>\n"
		"#include <string.h>\n\n"
		"static const char " base "_scm_image[] = {\n\t"
		image "\n};\n\n"
		"/*\n"
		" * Return the interpreter with the module loaded, or NULL if it\n"
		" * could not be started. The image is evaluated once; s7 is\n"
		" * process-global, so the definitions persist across calls.\n"
		" */\n"
		"static s7_scheme *" base "_ensure(void)\n"
		"{\n"
		"\tstatic int  loaded;\n"
		"\ts7_scheme  *s7 = script_s7();\n\n"
		"\tif (s7 && !loaded) {\n"
		"\t\tscript_eval(" base "_scm_image);\n"
		"\t\tloaded = 1;\n"
		"\t}\n"
		"\treturn s7;\n"
		"}\n\n"
		(krudd-join "\n"
		  (map (lambda (s)
			 (let ((nm (krudd-snake (krudd-struct-name s))))
			   (string-append "static void " base "_marshal_" nm
			     "(s7_scheme *s7, s7_pointer v,\n\t\tstruct "
			     nm " *out);")))
		       structs))
		"\n\n"
		(krudd-join "\n"
		  (map (lambda (s) (krudd-gen-marshaler base names s)) structs))
		"\n"
		(krudd-join "\n"
		  (map (lambda (e) (krudd-gen-driver base names e)) exports)))
	      port))))

;; Generate the C ABI from the Scheme module at IN: the header at HEADER-OUT
;; (constants + structs + prototypes) and the marshaling shim at SHIM-OUT (the
;; baked image + generated marshalers + drivers). The module name (the header's
;; basename, e.g. "md_parse") namespaces the shim's static symbols.
(define (krudd-embed-scheme-module in header-out shim-out)
	(let* ((forms       (krudd-scm-forms in))
	       (structs     (krudd-abi-structs forms))
	       (exports     (krudd-abi-exports forms))
	       (ints        (krudd-scm-int-defs in))
	       (names       (map krudd-struct-name structs))
	       (header-name (krudd-basename header-out))
	       (base        (krudd-strip-ext header-name))
	       (label       (krudd-rel-label in)))
	  (krudd-abi-check structs exports names)
	  (krudd-gen-header header-out header-name structs exports ints names label)
	  (krudd-gen-shim shim-out header-name base
			  (krudd-bytes->c-init (krudd-slurp in))
			  structs exports names label)))

;; ===========================================================================
;; The monolang: lower (define-c-fn ...) arithmetic bodies to native C.
;;
;; This is a different screw from the scheme-module ABI above. That one bakes an
;; s7 image and marshals across a runtime s7_call; this one *transpiles* the
;; arithmetic body to standalone C — no interpreter at runtime, so the same
;; source can later drive WGSL/Metal/GLSL backends a shader could actually run.
;; v1 is C-only and deliberately narrow: scalar float arithmetic, let* locals, a
;; whitelist of intrinsics, and mat4-valued returns. Anything outside the
;; vocabulary is a loud build error (see krudd/build/modules/math.scm), never
;; silently emitted.
;; ===========================================================================

;; Backend spellings. binops render infix; intrinsics map the backend-neutral
;; monolang name to the C float variant. A second column per backend is all a
;; future GLSL/Metal emitter adds here.
(define krudd-math-binops
	'((+ . "+") (- . "-") (* . "*") (/ . "/")))

(define krudd-math-intrinsics
	'((tan . "tanf") (sin . "sinf") (cos . "cosf") (sqrt . "sqrtf")))

;; A monolang number as a C float literal: force inexact so integers still carry
;; a decimal point (2 -> "2.0"), then suffix f (-> "2.0f").
(define (krudd-math-num->c n)
	(string-append (number->string (exact->inexact n)) "f"))

;; A scalar arithmetic expression -> a C expression string. Compound forms are
;; fully parenthesised, so operator precedence never depends on the emitter.
(define (krudd-math-expr e)
	(cond
	  ((number? e) (krudd-math-num->c e))
	  ((symbol? e) (krudd-snake e))
	  ((and (pair? e) (assq (car e) krudd-math-binops))
	   (if (< (length (cdr e)) 2)
	       (error 'krudd-math-binop-arity e)
	       (string-append "("
		 (krudd-join
		   (string-append " " (cdr (assq (car e) krudd-math-binops)) " ")
		   (map krudd-math-expr (cdr e)))
		 ")")))
	  ((and (pair? e) (assq (car e) krudd-math-intrinsics))
	   (string-append (cdr (assq (car e) krudd-math-intrinsics))
			  "(" (krudd-join ", " (map krudd-math-expr (cdr e))) ")"))
	  (else (error 'krudd-math-bad-expr e))))

;; Split a (define-c-fn ...) signature (name (arg kind) ... -> ret) into
;; (name ((cname . ctype) ...) ret). Only f32/f64 params for now.
(define (krudd-math-fn-parts sig)
	(let loop ((l (cdr sig)) (args '()))
	  (cond ((null? l) (error 'krudd-math-no-arrow sig))
		((eq? (car l) '->) (list (car sig) (reverse args) (cadr l)))
		(else
		 (let ((a (car l)))
		   (if (not (and (pair? a) (symbol? (car a))
				 (krudd-float-kind? (cadr a))))
		       (error 'krudd-math-bad-arg a))
		   (loop (cdr l)
			 (cons (cons (krudd-snake (car a))
				     (krudd-scalar-ctype (cadr a)))
			       args)))))))

;; A (mat4-cols (vec4 ...) x4) return: flatten the 16 column-major elements to
;; out->m[i] = <expr>; assignments.
(define (krudd-math-emit-mat4 cols)
	(if (not (= (length cols) 4)) (error 'krudd-math-mat4-cols cols))
	(let ((elts (apply append
			    (map (lambda (c)
				   (if (not (and (pair? c) (eq? (car c) 'vec4)
						 (= (length (cdr c)) 4)))
				       (error 'krudd-math-bad-col c))
				   (cdr c))
				 cols))))
	  (let loop ((i 0) (l elts) (acc ""))
	    (if (null? l) acc
		(loop (+ i 1) (cdr l)
		      (string-append acc "\tout->m[" (number->string i) "] = "
				     (krudd-math-expr (car l)) ";\n"))))))

;; A function body form -> C statements. let* becomes sequential float locals;
;; the tail must build the returned mat4.
(define (krudd-math-emit-form f)
	(cond
	  ((and (pair? f) (eq? (car f) 'let*))
	   (string-append
	     (apply string-append
		    (map (lambda (b)
			   (string-append "\tfloat " (krudd-snake (car b)) " = "
					  (krudd-math-expr (cadr b)) ";\n"))
			 (cadr f)))
	     "\n"
	     (krudd-math-emit-form (caddr f))))
	  ((and (pair? f) (eq? (car f) 'mat4-cols))
	   (krudd-math-emit-mat4 (cdr f)))
	  (else (error 'krudd-math-bad-body f))))

;; One (define-c-fn ...) -> a C function. A -> mat4 return lowers to the
;; engine's void f(struct mat4 *out, ...) convention.
(define (krudd-math-emit-fn form)
	(let* ((parts (krudd-math-fn-parts (cadr form)))
	       (name  (krudd-snake (car parts)))
	       (args  (cadr parts))
	       (ret   (caddr parts))
	       (body  (cddr form)))
	  (if (not (eq? ret 'mat4)) (error 'krudd-math-unsupported-return ret))
	  (if (not (= (length body) 1)) (error 'krudd-math-body-arity form))
	  (string-append
	    "void " name "(struct mat4 *out"
	    (apply string-append
		   (map (lambda (a) (string-append ", " (cdr a) " " (car a))) args))
	    ")\n{\n"
	    (krudd-math-emit-form (car body))
	    "}\n")))

;; Generate the native C for the monolang module at IN, writing it to C-OUT. The
;; module's mat4-valued functions call through struct mat4 (declared in the
;; hand-written math_types.h the generated file includes).
(define (krudd-emit-math-module in c-out)
	(let* ((forms (krudd-scm-forms in))
	       (fns   (krudd-filter (lambda (f)
				      (and (pair? f) (eq? (car f) 'define-c-fn)))
				    forms))
	       (label (krudd-rel-label in)))
	  (if (null? fns) (error 'krudd-math-no-fns in))
	  (call-with-output-file c-out
	    (lambda (port)
	      (write-string
		(string-append
		  "/* SPDX-License-Identifier: GPL-2.0-or-later */\n"
		  "/* Generated by krudd from " label ". Do not edit. */\n"
		  "#include \"math_types.h\"\n\n"
		  "#include <math.h>\n\n"
		  (krudd-join "\n" (map krudd-math-emit-fn fns)))
		port)))))
