; SPDX-License-Identifier: GPL-2.0-or-later
;
; ninja.scm — the Ninja synthesizer, krudd's build backend.
;
; It reads the directory-spec forms (library, interface-library, executable,
; test, and the native-only/wasm-only toolchain gates) that describe each owned
; directory and renders a single build.ninja from them, driven by
; krudd/build/build.scm. Ninja has no notion of directories or transitive usage
; requirements, so this emitter renders the whole manifest into one build.ninja
; and leans on resolve.scm to flatten the include graph that CMake's
; target_link_libraries once did implicitly.
;
; Coverage of the spec forms:
;   library / executable   compile each source to an object, then archive/link
;   interface-library      no build output — only include dirs, via resolve.scm
;   test                   a stamp rule that runs the test executable
;   native-only / wasm-only
;                          a toolchain gate: the forms inside build only under
;                          that toolchain. A wasm-only library (imgui, the
;                          markdown board, the scene renderer) has no native
;                          archive; it is compiled with emcc/em++ and folded
;                          into the single WASM module. There are no standalone
;                          plugin .wasm outputs and no dynamic loading — every
;                          module is one static archive in the main module.
;
; Object files live at obj/<target>/<source>.o — namespaced per target because a
; source compiled into two targets sees different include flags (exactly as
; CMake gives each target its own CMakeFiles/<target>.dir). Libraries archive to
; lib<name>.a and executables link to bin/<name>, both at the build root.
;
; The default target, `native`, groups everything the native (non-Emscripten)
; toolchain builds — every static library and every test stamp — so a bare
; `ninja` builds and is buildable without Emscripten. The `wasm` target is the
; single Emscripten module (index.{html,js,wasm}); the plugin objects reach it
; transitively, so it is built only when named explicitly.

(load (string-append (or (getenv "KRUDD_ROOT") ".")
		     "/krudd/build/ninja/resolve.scm"))
(load (string-append (or (getenv "KRUDD_ROOT") ".")
		     "/krudd/build/introspect.scm"))

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

;; ---------------------------------------------------------------------------
;; Path references. A spec path resolved by resolve.scm is either tree-relative
;; (compiled/included under $srcroot) or a raw literal carrying one of krudd's
;; own path tokens (${imgui}, ${generated}). Tree-relative paths get the
;; $srcroot prefix; the tokens resolve to real Ninja paths the same way for
;; native and WASM, so a native target (the md_parse Scheme shim) can include
;; the generated headers too.
;; ---------------------------------------------------------------------------

(define (ninja-ref path)
	(if (ninja-has-dollar? path)
	    (ninja-resolve-var path)
	    (string-append "$srcroot/" path)))

(define (ninja-include-flags dirs)
	(ninja-join " " (map (lambda (d) (string-append "-I" (ninja-ref d)))
			     dirs)))

;; The WASM build resolves the krudd-owned path tokens the side-module specs
;; reference (${imgui} from the fetch, ${generated} for the configure_file /
;; embed headers) to real Ninja paths, rather than escaping them for native
;; output.
(define (ninja-resolve-var p)
	(krudd-replace
	  (krudd-replace p "${imgui}" "$imgui")
	  "${generated}" "generated"))

(define (ninja-wasm-ref path)
	(if (ninja-has-dollar? path)
	    (ninja-resolve-var path)
	    (string-append "$srcroot/" path)))

(define (ninja-wasm-include-flags dirs)
	(ninja-join " " (map (lambda (d) (string-append "-I" (ninja-wasm-ref d)))
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
(define ninja-wasm '())

(define (ninja-emit line) (set! ninja-lines (cons line ninja-lines)))
(define (ninja-emit* lines) (for-each ninja-emit lines))
(define (ninja-native! out) (set! ninja-native (cons out ninja-native)))
(define (ninja-wasm! out) (set! ninja-wasm (cons out ninja-wasm)))

;; Object-path form of a resolved source path: the krudd path tokens become
;; plain directory names (not the $imgui/generated Ninja vars ninja-wasm-ref
;; produces) so obj/<target>/... stays a real relative filesystem path.
(define (ninja-obj-clean p)
	(krudd-replace (krudd-replace p "${imgui}" "imgui")
		       "${generated}" "generated"))

;; Compile one source of TARGET (in DIR) with INCLUDES; emit the build stanza
;; and return the object path.
;; A source spec may carry a krudd path token (${generated}/md_parse.scm.c —
;; the generated Scheme shim compiled into a native library). Resolve it for the
;; object path too, so obj/<target>/generated/... stays a real filesystem path
;; rather than an unexpanded ${generated} segment.
(define (ninja-emit-compile name dir includes-flags src-spec)
	(let* ((treepath (rz-path dir src-spec))
	       (clean (ninja-resolve-var treepath))
	       (obj (ninja-obj name clean)))
		(ninja-emit (string-append "build " obj ": "
					   (ninja-compile-rule clean)
					   " " (ninja-ref treepath)))
		(ninja-emit (string-append "  includes = " includes-flags))
		obj))

;; The source path specs of a target's (sources ...) clause.
(define (ninja-sources clauses)
	(let ((c (rz-clause 'sources clauses))) (if c (cdr c) '())))

;; The "script" library folds the third-party s7 amalgamation into its archive,
;; compiled with the relaxed s7 rules. Emitted once per toolchain (native/WASM)
;; so the object lands beside the archive that carries it.
(define (ninja-emit-s7-obj rule obj)
	(ninja-emit (string-append "build " obj ": " rule " $s7dir/s7.c"))
	obj)

(define (ninja-with-s7 name rule obj objs)
	(if (string=? name "script")
	    (append objs (list (ninja-emit-s7-obj rule obj)))
	    objs))

;; library: compile sources, archive to lib<name>.a. An interface-library
;; carries no sources and produces nothing here.
(define (ninja-emit-library table dir form)
	(let* ((name (cadr form))
	       (clauses (cddr form))
	       (includes (ninja-include-flags (resolve-includes table name)))
	       (objs (ninja-with-s7 name "cc_s7" "obj/s7/s7.c.o"
			(map (lambda (s)
			       (ninja-emit-compile name dir includes s))
			     (ninja-sources clauses))))
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

;; Dispatch one form. Non-target scaffolding forms (verbatim/set/subdirs/…) are
;; the root spec's project bootstrap, not this emitter's job, and are skipped.
;; wasm-only forms emit nothing on this (native) walk: their libraries are
;; recompiled with emcc/em++ and folded into the main module through the WASM
;; library map (ninja-build-libmap descends into wasm-only for that), so a
;; native `ninja` never tries to build them.
(define (ninja-emit-form table dir form)
	(case (car form)
	  ((library) (ninja-emit-library table dir form))
	  ((interface-library) #t)   ; include-only, handled by resolve.scm
	  ((executable) (ninja-emit-executable table dir form))
	  ((test) (ninja-emit-test form))
	  ((wasm-only) #t)           ; WASM toolchain only — see ninja-emit-main-module
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
	  "# Source of truth: krudd/build/ninja/**/build.scm, rendered by"
	  "# krudd/build/ninja/ninja.scm."
	  "# Regenerate: see krudd/build/ninja/run-tests.sh"
	  ""
	  "ninja_required_version = 1.10"
	  (string-append "srcroot = " srcroot)
	  "cc = cc"
	  "cxx = c++"
	  "ar = ar"
	  "emcc = emcc"
	  "empp = em++"
	  "emar = emar"
	  "cflags = -std=gnu11 -Wall -Werror -Wpedantic"
	  "cxxflags = -std=gnu11 -Wall -Werror -Wpedantic"
	  ;; The WASM compile flags match the native ones; the Emscripten-specific
	  ;; flags live on the emcc_cxx (C++ modules) and main_module rules, captured
	  ;; explicitly here rather than inherited from an emcmake toolchain file.
	  "emcflags = -std=gnu11 -Wall -Werror -Wpedantic"
	  ;; The embedded s7 interpreter (krudd/third_party/s7.c) is a third-party
	  ;; amalgamation compiled the way krudd.sh compiles it for the build tool:
	  ;; warnings off and the same feature switches. It cannot go through the
	  ;; -Werror -Wpedantic project rules, so it gets its own dir and rules,
	  ;; native and WASM, and is folded into the "script" archive.
	  "s7dir = $srcroot/../../third_party"
	  "s7flags = -O2 -w -DWITH_C_LOADER=0 -DWITH_MAIN=0 -I$s7dir"
	  (string-append "imgui = " (krudd-fetch-dir "imgui"))
	  ;; Main-module link flags — the emscripten bootstrap for the index
	  ;; target, owned here rather than in the directory spec. Two flags carry
	  ;; hard-won rationale:
	  ;;   -sGROWABLE_ARRAYBUFFERS=0  Emscripten 6.0.2 flipped this to default
	  ;;     =1, backing the heap with a real resizable ArrayBuffer. Web APIs
	  ;;     reject views over a resizable buffer — Firefox's
	  ;;     crypto.getRandomValues (reached at startup via WASI random_get)
	  ;;     throws "Argument 1 can't be a resizable ArrayBuffer...". Opting out
	  ;;     keeps memory growth (copy into a fresh plain ArrayBuffer on grow),
	  ;;     which the Web Crypto API accepts.
	  ;;   -sMALLOC=mimalloc  the one allocator: libc, FETCH, and
	  ;;     modules/memory (which allocates through libc) all share this
	  ;;     single heap. Two allocators over one growable heap corrupted on
	  ;;     growth.
	  ;; The engine is a single WASM module: every plugin is compiled straight
	  ;; into it (no -sMAIN_MODULE / side modules, no dynamic linking).
	  (string-append "mainflags = -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 "
			 "-sGROWABLE_ARRAYBUFFERS=0 -sMALLOC=mimalloc "
			 "-sFETCH=1 -sMAX_WEBGL_VERSION=2 "
			 "-sEXPORTED_FUNCTIONS=_main")
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
	  "rule cc_s7"
	  "  command = $cc $s7flags -c $in -o $out"
	  "  description = CC(s7) $out"
	  ""
	  "rule emcc_s7"
	  "  command = $emcc $s7flags -c $in -o $out"
	  "  description = EMCC(s7) $out"
	  ""
	  "rule link"
	  "  command = $cc $in $ldlibs -o $out"
	  "  description = LINK $out"
	  ""
	  "rule run_test"
	  "  command = $in && touch $out"
	  "  description = TEST $out"
	  ""
	  ;; WASM rules — the whole WASM path goes through explicit emcc/em++ calls,
	  ;; no emcmake. C sources take emcc_c with the project flags; C++ sources
	  ;; (the imgui and markdown-board modules, plus third-party imgui) take
	  ;; emcc_cxx, whose per-library $emcxxflags carries the C++ standard and the
	  ;; -fno-exceptions/-fno-rtti + no-Werror the third-party sources need.
	  "rule emcc_c"
	  "  command = $emcc $emcflags $includes -c $in -o $out"
	  "  description = EMCC $out"
	  ""
	  "rule emcc_cxx"
	  "  command = $empp -O2 $emcxxflags $includes -c $in -o $out"
	  "  description = EMCXX $out"
	  ""
	  "rule emar"
	  "  command = rm -f $out && $emar rcs $out $in"
	  "  description = EMAR $out"
	  ""
	  ;; The single module: em++ links engine + every module archive (core plus
	  ;; each plugin) into index.{html,js,wasm}. No -sMAIN_MODULE — nothing is
	  ;; loaded dynamically.
	  "rule main_module"
	  "  command = $empp $mainflags $extraflags $in -o $out"
	  "  description = LINK(wasm) $out"
	  ""))

;; ---------------------------------------------------------------------------
;; WASM main module. The Ninja backend owns the whole WASM path: the main module
;; (index.html/.js/.wasm) links WASM-compiled copies of the same libraries the
;; native tests use, so those are archived a second time with emcc. The
;; emscripten-only source (plugin_abi.c) and the shell/pre-js live here.
;; ---------------------------------------------------------------------------

;; name -> (dir . clauses) for every library, so the WASM path can recompile a
;; library's sources with emcc/em++. Descends into both native-only and
;; wasm-only: a native-only library (md_parse) never reaches the main module's
;; link closure so it is simply never emitted, while a wasm-only library (imgui,
;; the board, the scene renderer) is emitted here and nowhere else.
(define (ninja-build-libmap manifest)
	(let ((out '()))
		(define (walk dir forms)
			(for-each
			  (lambda (f)
			    (cond ((eq? (car f) 'library)
				   (set! out (cons (cons (cadr f)
							 (cons dir (cddr f)))
						   out)))
				  ((memq (car f) '(native-only wasm-only))
				   (walk dir (cdr f)))
				  (else #t)))
			  forms))
		(for-each (lambda (p) (walk (car p) (cdr p))) manifest)
		out))

(define (ninja-wasm-obj name treepath)
	(string-append "wasm-obj/" name "/" treepath ".o"))

;; The WASM compile rule for a source, by extension: C++ takes emcc_cxx (which
;; carries the per-library $emcxxflags), everything else emcc_c.
(define (ninja-wasm-compile-rule src)
	(if (or (ninja-suffix? src ".cpp") (ninja-suffix? src ".cc"))
	    "emcc_cxx" "emcc_c"))

;; Compile a library's sources with emcc/em++ and archive to wasm/lib<name>.a.
;; A library may carry (wasm-flags ...) — the C++ standard and warning switches
;; its C++ (and third-party) translation units need; they ride on $emcxxflags,
;; so they reach the emcc_cxx sources only, never the plain-C emcc_c ones.
(define (ninja-emit-wasm-lib table libmap name)
	(let* ((entry (assoc name libmap))
	       (dir (cadr entry))
	       (clauses (cddr entry))
	       (sources (ninja-sources clauses))
	       (cxxflags (let ((c (rz-clause 'wasm-flags clauses)))
			   (if c (ninja-join " " (cdr c)) "")))
	       (includes (ninja-wasm-include-flags (resolve-includes table name)))
	       (objs (ninja-with-s7 name "emcc_s7" "wasm-obj/s7/s7.c.o"
			(map (lambda (s)
			       (let* ((tp (rz-path dir s))
				      (clean (ninja-resolve-var tp))
				      (obj (ninja-wasm-obj name
					     (ninja-obj-clean tp)))
				      (rule (ninja-wasm-compile-rule clean)))
				 (ninja-emit (string-append "build " obj ": " rule
						    " " (ninja-wasm-ref tp)))
				 (if (string=? rule "emcc_cxx")
				     (ninja-emit (string-append "  emcxxflags = "
							cxxflags)))
				 (ninja-emit (string-append "  includes = " includes))
				 obj))
			     sources)))
	       (lib (string-append "wasm/lib" name ".a")))
		(ninja-emit (string-append "build " lib ": emar "
					   (ninja-join " " objs)))
		(ninja-emit "")
		lib))

;; The main module: engine.c + the emscripten-only plugin_abi.c, compiled with
;; emcc and linked with em++ against the whole WASM library closure — the core
;; libraries the executable links plus every plugin module named by its
;; (wasm-modules ...) clause — to index.html/.js/.wasm.
(define (ninja-emit-main-module table libmap)
	(let* ((dir "modules/core")
	       (srcs (list "engine.c" "plugin_abi.c"))
	       (includes (ninja-wasm-include-flags
			   (resolve-includes table "index")))
	       (objs (map (lambda (s)
			    (let* ((tp (rz-path dir s))
				   (obj (ninja-wasm-obj "index" tp)))
			      (ninja-emit (string-append "build " obj ": emcc_c "
						 (ninja-wasm-ref tp)))
			      (ninja-emit (string-append "  includes = " includes))
			      obj))
			  srcs))
	       (libs (map (lambda (l) (ninja-emit-wasm-lib table libmap l))
			  (resolve-wasm-module-libs table "index"))))
		;; emcc -o index.html also emits index.js and index.wasm; declare
		;; them as implicit outputs so $out stays just index.html. Each
		;; module is one archive, ordered dependents-first (the order
		;; resolve-wasm-module-libs returns), so a plugin's undefined symbols
		;; resolve from the core archives that follow it; each plugin's
		;; <name>_plugin_entry is called from engine.c, so its archive member
		;; is pulled in and wasm-ld's --gc-sections drops only what nothing
		;; reaches.
		(ninja-emit (string-append
			      "build index.html | index.js index.wasm: main_module "
			      (ninja-join " " (append objs libs))))
		(ninja-emit (string-append "  extraflags = --extern-pre-js "
			      "$srcroot/modules/core/error_overlay.js "
			      "--shell-file generated/shell.html"))
		(ninja-emit "")
		(ninja-wasm! "index.html")))

;; The configure_file / embed outputs the WASM build compiles against,
;; generated into <builddir>/generated at synthesis time (as CMake ran
;; configure_file / the embed script at configure time).
(define (ninja-generate-codegen srcroot builddir)
	(let ((gen     (string-append builddir "/generated"))
	      (mdscm   (string-append (krudd-repo-root)
				      "/krudd/build/modules/md_parse.scm"))
	      (mathscm (string-append (krudd-repo-root)
				      "/krudd/build/modules/math.scm")))
		(system (string-append "mkdir -p \"" gen "\""))
		(krudd-configure-file
		  (string-append srcroot "/modules/core/version.h.in")
		  (string-append gen "/version.h"))
		(krudd-configure-file
		  (string-append srcroot "/modules/core/shell.html.in")
		  (string-append gen "/shell.html"))
		(krudd-embed-file
		  (string-append srcroot "/modules/core/runtime.scm")
		  (string-append gen "/runtime_scm.h") "RUNTIME_SCM")
		;; md_parse.scm (under krudd/build/modules/, outside the ninja
		;; tree) carries its own C ABI declaration; krudd's binding
		;; generator emits the whole C seam from it — the md_parse.h
		;; header and the md_parse.scm.c marshaling shim (image + generated
		;; marshalers + driver). The .scm is the only ABI artifact in git.
		(krudd-embed-scheme-module
		  mdscm
		  (string-append gen "/md_parse.h")
		  (string-append gen "/md_parse.scm.c"))
		;; math.scm (also under krudd/build/modules/) is the monolang: its
		;; (define-c-fn ...) arithmetic bodies are transpiled straight to
		;; native C — no s7 image, no runtime marshaling. The generated
		;; math_gen.c compiles into math_test beside the hand-written
		;; math.c; math_types.h stays a hand-written header in the tree.
		(krudd-emit-math-module
		  mathscm
		  (string-append gen "/math_gen.c"))))

;; Render the whole manifest to build.ninja text. MANIFEST is a list of
;; (DIR . SPEC) pairs; SRCROOT is the absolute path of the tree root
;; (krudd/build/ninja/) the paths resolve against. When BUILDDIR is given, the
;; configure_file / embed outputs are generated into it so `ninja wasm` can
;; compile against them.
(define (ninja-synthesize manifest srcroot . rest)
	(let ((builddir (if (pair? rest) (car rest) #f)))
		(set! ninja-lines '())
		(set! ninja-native '())
		(set! ninja-wasm '())
		(let ((table (rz-target-table manifest))
		      (libmap (ninja-build-libmap manifest)))
			(resolve-check-all table)   ; fail loud on cycle / unknown
			(ninja-emit* (ninja-preamble srcroot))
			(for-each
			  (lambda (pair)
			    (for-each (lambda (form)
					(ninja-emit-form table (car pair) form))
				      (cdr pair)))
			  manifest)
			(ninja-emit "# --- WASM (Emscripten) main module ---")
			(ninja-emit "")
			(if builddir (ninja-generate-codegen srcroot builddir))
			(ninja-emit-main-module table libmap)
			(ninja-emit (string-append "build native: phony "
					   (ninja-join " " (reverse ninja-native))))
			(ninja-emit (string-append "build wasm: phony "
					   (ninja-join " " (reverse ninja-wasm))))
			(ninja-emit "default native")
			(ninja-emit "")
			(ninja-join "\n" (reverse ninja-lines)))))
