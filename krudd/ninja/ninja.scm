; SPDX-License-Identifier: GPL-2.0-or-later
;
; ninja.scm — the Ninja synthesizer.
;
; The sibling of krudd/cmake/cmake.scm: it reads the same directory-spec forms
; (library, interface-library, executable, test, side-module) and renders a
; build.ninja instead of a CMakeLists.txt tree. cmake.scm emits one file per
; directory and lets CMake stitch them together; Ninja has no notion of
; directories or transitive usage requirements, so this emitter renders the
; whole manifest into a single build.ninja and leans on resolve.scm to flatten
; the include graph that CMake's target_link_libraries did implicitly.
;
; This lands side-by-side with cmake.scm and is not wired into krudd/build.scm:
; the CMake path still drives real builds. The emitter is exercised on its own
; (see resolve_test.scm) — generate a build.ninja and let ninja(1) build it.
;
; Coverage of the spec forms:
;   library / executable   compile each source to an object, then archive/link
;   interface-library      no build output — only include dirs, via resolve.scm
;   test                   a stamp rule that runs the test executable
;   side-module            one emcc/em++ invocation to a .wasm (the WASM path;
;                          emitted for completeness, not part of the native
;                          default target since it needs the Emscripten toolchain)
;
; Object files live at obj/<target>/<source>.o — namespaced per target because a
; source compiled into two targets sees different include flags (exactly as
; CMake gives each target its own CMakeFiles/<target>.dir). Libraries archive to
; lib<name>.a and executables link to bin/<name>, both at the build root.
;
; The default target, `native`, groups everything the native (non-Emscripten)
; toolchain builds — every static library and every test stamp — so a bare
; `ninja` builds and is buildable without Emscripten. Side modules build only
; when named explicitly.

(load (string-append (or (getenv "KRUDD_ROOT") ".")
		     "/krudd/ninja/resolve.scm"))

;; ---------------------------------------------------------------------------
;; String helpers (s7 has no string-join / string-suffix? we can lean on).
;; ---------------------------------------------------------------------------

(define (ninja-join sep lst)
	(cond ((null? lst) "")
	      ((null? (cdr lst)) (car lst))
	      (else (string-append (car lst) sep (ninja-join sep (cdr lst))))))

(define (ninja-suffix? s suf)
	(let ((ls (string-length s)) (lf (string-length suf)))
		(and (>= ls lf) (string=? (substring s (- ls lf)) suf))))

(define (ninja-has-dollar? s)
	(let loop ((i 0))
		(cond ((>= i (string-length s)) #f)
		      ((char=? (string-ref s i) #\$) #t)
		      (else (loop (+ i 1))))))

;; Double every $ so a literal (raw ...) path with a ${VAR} in it survives
;; ninja's own variable syntax unexpanded.
(define (ninja-escape s)
	(let loop ((i 0) (out ""))
		(if (>= i (string-length s))
		    out
		    (let ((c (string-ref s i)))
			(loop (+ i 1)
			      (string-append out
					     (if (char=? c #\$) "$$"
						 (string c))))))))

;; ---------------------------------------------------------------------------
;; Path references. A spec path resolved by resolve.scm is either tree-relative
;; (compiled/included under $srcroot) or a raw literal carrying its own
;; variables. Tree-relative paths get the $srcroot prefix; raw literals pass
;; through with their $ escaped.
;; ---------------------------------------------------------------------------

(define (ninja-ref path)
	(if (ninja-has-dollar? path)
	    (ninja-escape path)
	    (string-append "$srcroot/" path)))

(define (ninja-include-flags dirs)
	(ninja-join " " (map (lambda (d) (string-append "-I" (ninja-ref d)))
			     dirs)))

;; The compile rule for a source, by extension: C++ sources use cxx, all else
;; cc. Native targets are C-only; this keeps the side-module path honest.
(define (ninja-compile-rule src)
	(if (or (ninja-suffix? src ".cpp") (ninja-suffix? src ".cc"))
	    "cxx" "cc"))

(define (ninja-obj name treepath)
	(string-append "obj/" name "/" treepath ".o"))

;; ---------------------------------------------------------------------------
;; The emitter accumulates output lines and the list of native default-target
;; outputs as it walks the manifest.
;; ---------------------------------------------------------------------------

(define ninja-lines '())
(define ninja-native '())

(define (ninja-emit line) (set! ninja-lines (cons line ninja-lines)))
(define (ninja-emit* lines) (for-each ninja-emit lines))
(define (ninja-native! out) (set! ninja-native (cons out ninja-native)))

;; Compile one source of TARGET (in DIR) with INCLUDES; emit the build stanza
;; and return the object path.
(define (ninja-emit-compile name dir includes-flags src-spec)
	(let* ((treepath (rz-path dir src-spec))
	       (obj (ninja-obj name treepath)))
		(ninja-emit (string-append "build " obj ": "
					   (ninja-compile-rule treepath)
					   " " (ninja-ref treepath)))
		(ninja-emit (string-append "  includes = " includes-flags))
		obj))

;; The source path specs of a target's (sources ...) clause.
(define (ninja-sources clauses)
	(let ((c (rz-clause 'sources clauses))) (if c (cdr c) '())))

;; library: compile sources, archive to lib<name>.a. An interface-library
;; carries no sources and produces nothing here.
(define (ninja-emit-library table dir form)
	(let* ((name (cadr form))
	       (clauses (cddr form))
	       (includes (ninja-include-flags (resolve-includes table name)))
	       (objs (map (lambda (s)
			    (ninja-emit-compile name dir includes s))
			  (ninja-sources clauses)))
	       (lib (string-append "lib" name ".a")))
		(ninja-emit (string-append "build " lib ": ar "
					   (ninja-join " " objs)))
		(ninja-emit "")
		(ninja-native! lib)))

;; executable: compile sources, then link against the transitive closure of the
;; libraries it links (dependents-first, so a single-pass static link resolves),
;; plus any system libraries as -l flags.
(define (ninja-emit-executable table dir form)
	(let* ((name (cadr form))
	       (clauses (cddr form))
	       (includes (ninja-include-flags (resolve-includes table name)))
	       (objs (map (lambda (s)
			    (ninja-emit-compile name dir includes s))
			  (ninja-sources clauses)))
	       (libs (map (lambda (l) (string-append "lib" l ".a"))
			  (resolve-link-libs table name)))
	       (syslibs (resolve-syslibs table name))
	       (bin (string-append "bin/" name)))
		(ninja-emit (string-append "build " bin ": link "
					   (ninja-join " " (append objs libs))))
		(if (pair? syslibs)
		    (ninja-emit (string-append "  ldlibs = "
					       (ninja-join " "
						 (map (lambda (l)
							(string-append "-l" l))
						      syslibs)))))
		(ninja-emit "")))

;; test: run the named executable, touch a stamp on success.
(define (ninja-emit-test form)
	(let* ((name (cadr form))
	       (cmd (caddr form))
	       (stamp (string-append "test/" name ".stamp")))
		(ninja-emit (string-append "build " stamp ": run_test bin/" cmd))
		(ninja-emit "")
		(ninja-native! stamp)))

;; side-module: one emcc/em++ call to ${name}.wasm. Uses the form's own include
;; and source lists verbatim (the WASM builds spell out every -I explicitly);
;; resolve.scm is a native-link concept and does not apply. Not added to the
;; native default target — it needs the Emscripten toolchain.
(define (ninja-side-field clauses head deflt)
	(let ((c (rz-clause head clauses))) (if c (cadr c) deflt)))

(define (ninja-emit-side-module dir form)
	(let* ((name (cadr form))
	       (clauses (cddr form))
	       (compiler (if (eq? (ninja-side-field clauses 'compiler 'c) 'cxx)
			     "$empp" "$emcc"))
	       (flags (let ((c (rz-clause 'flags clauses)))
			(if c (cdr c) '())))
	       (includes (let ((c (rz-clause 'includes clauses)))
			   (if c (cdr c) '())))
	       (sources (let ((c (rz-clause 'sources clauses)))
			  (if c (cdr c) '())))
	       (wasm (string-append name ".wasm")))
		(ninja-emit (string-append "build " wasm ": side_module "
			      (ninja-join " "
				(map (lambda (s) (ninja-ref (rz-path dir s)))
				     sources))))
		(ninja-emit (string-append "  smcc = " compiler))
		(ninja-emit (string-append "  smflags = " (ninja-join " " flags)))
		(ninja-emit (string-append "  includes = "
			      (ninja-join " "
				(map (lambda (i)
				       (string-append "-I"
					 (ninja-ref (rz-path dir i))))
				     includes))))
		(ninja-emit "")))

;; Dispatch one form. Non-target scaffolding forms (verbatim/set/subdirs/…) are
;; the root spec's project bootstrap, not this emitter's job, and are skipped.
(define (ninja-emit-form table dir form)
	(case (car form)
	  ((library) (ninja-emit-library table dir form))
	  ((interface-library) #t)   ; include-only, handled by resolve.scm
	  ((executable) (ninja-emit-executable table dir form))
	  ((test) (ninja-emit-test form))
	  ((side-module) (ninja-emit-side-module dir form))
	  ((native-only)
	   (for-each (lambda (f) (ninja-emit-form table dir f)) (cdr form)))
	  (else #t)))

;; ---------------------------------------------------------------------------
;; Preamble: the build variables and rules, emitted once. srcroot is left as a
;; variable the caller fills so the generated file is relocatable.
;; ---------------------------------------------------------------------------

(define (ninja-preamble srcroot)
	(list
	  "# Generated by krudd — do not edit by hand."
	  "# Source of truth: krudd/cmake/**/CMakeLists.scm, rendered by"
	  "# krudd/ninja/ninja.scm."
	  "# Regenerate: see krudd/ninja/run-tests.sh"
	  ""
	  "ninja_required_version = 1.10"
	  (string-append "srcroot = " srcroot)
	  "cc = cc"
	  "cxx = c++"
	  "ar = ar"
	  "emcc = emcc"
	  "empp = em++"
	  "cflags = -std=gnu11 -Wall -Werror -Wpedantic"
	  "cxxflags = -std=gnu11 -Wall -Werror -Wpedantic"
	  "smbase = -sSIDE_MODULE=1 -O2"
	  ""
	  "rule cc"
	  "  command = $cc $cflags $includes -c $in -o $out"
	  "  description = CC $out"
	  ""
	  "rule cxx"
	  "  command = $cxx $cxxflags $includes -c $in -o $out"
	  "  description = CXX $out"
	  ""
	  "rule ar"
	  "  command = rm -f $out && $ar rcs $out $in"
	  "  description = AR $out"
	  ""
	  "rule link"
	  "  command = $cc $in $ldlibs -o $out"
	  "  description = LINK $out"
	  ""
	  "rule side_module"
	  "  command = $smcc $smbase $smflags $includes -o $out $in"
	  "  description = SIDE_MODULE $out"
	  ""
	  "rule run_test"
	  "  command = $in && touch $out"
	  "  description = TEST $out"
	  ""))

;; Render the whole manifest to build.ninja text. MANIFEST is a list of
;; (DIR . SPEC) pairs; SRCROOT is the absolute path of the tree root
;; (krudd/cmake/) the paths resolve against.
(define (ninja-synthesize manifest srcroot)
	(set! ninja-lines '())
	(set! ninja-native '())
	(let ((table (rz-target-table manifest)))
		(resolve-check-all table)   ; fail loud on cycle / unknown target
		(ninja-emit* (ninja-preamble srcroot))
		(for-each
		  (lambda (pair)
		    (for-each (lambda (form)
				(ninja-emit-form table (car pair) form))
			      (cdr pair)))
		  manifest)
		(ninja-emit (string-append "build native: phony "
					   (ninja-join " " (reverse ninja-native))))
		(ninja-emit "default native")
		(ninja-emit "")
		(ninja-join "\n" (reverse ninja-lines))))
