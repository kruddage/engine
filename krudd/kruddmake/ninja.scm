; SPDX-License-Identifier: GPL-2.0-or-later

(load (string-append (or (getenv "KRUDD_ROOT") ".")
                     "/krudd/kruddmake/resolve.scm"))
(load (string-append (or (getenv "KRUDD_ROOT") ".")
                     "/krudd/kruddmake/introspect.scm"))

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

(define (ninja-ref path)
  (if (ninja-has-dollar? path)
      (ninja-resolve-var path)
      (string-append "$srcroot/" path)))

(define (ninja-include-flags dirs)
  (ninja-join " " (map (lambda (d) (string-append "-I" (ninja-ref d)))
                       dirs)))

(define (ninja-resolve-var p)
  (krudd-replace p "${generated}" "generated"))

(define (ninja-wasm-ref path)
  (if (ninja-has-dollar? path)
      (ninja-resolve-var path)
      (string-append "$srcroot/" path)))

(define (ninja-wasm-include-flags dirs)
  (ninja-join " " (map (lambda (d) (string-append "-I" (ninja-wasm-ref d)))
                       dirs)))

(define (ninja-compile-rule src)
  (if (or (ninja-suffix? src ".cpp") (ninja-suffix? src ".cc"))
      "cxx" "cc"))

(define (ninja-obj name treepath)
  (string-append "obj/" name "/" treepath ".o"))

(define ninja-lines '())
(define ninja-native '())
(define ninja-wasm '())

(define (ninja-emit line) (set! ninja-lines (cons line ninja-lines)))
(define (ninja-emit* lines) (for-each ninja-emit lines))
(define (ninja-native! out) (set! ninja-native (cons out ninja-native)))
(define (ninja-wasm! out) (set! ninja-wasm (cons out ninja-wasm)))

(define (ninja-obj-clean p)
  (krudd-replace p "${generated}" "generated"))

(define (ninja-emit-compile name dir includes-flags src-spec)
  (let* ((treepath (rz-path dir src-spec))
         (clean (ninja-resolve-var treepath))
         (obj (ninja-obj name clean)))
    (ninja-emit (string-append "build " obj ": "
                               (ninja-compile-rule clean)
                               " " (ninja-ref treepath)))
    (ninja-emit (string-append "  includes = " includes-flags))
    obj))

(define (ninja-sources clauses)
  (let ((c (rz-clause 'sources clauses))) (if c (cdr c) '())))

;;! Native Dawn is an external artifact (a ~38 MB libwebgpu_dawn.a built out of
;;! tree — see tools/dawn-smoke/README.md), so a `(dawn)` target is OPT-IN:
;;! without KRUDD_DAWN_PREFIX in the environment it is left out of the native
;;! graph entirely and `krudd build` is byte-for-byte what it was. That is what
;;! keeps CI — which has no Dawn checkout — building green. The WASM target is
;;! unaffected either way: there Dawn arrives through --use-port=emdawnwebgpu,
;;! so `(dawn)` is a native-only concern and the wasm emitters ignore it.
(define (dawn-prefix) (getenv "KRUDD_DAWN_PREFIX"))

(define (ninja-dawn? clauses) (if (rz-clause 'dawn clauses) #t #f))

;;! A `(dawn)` target is skipped natively when no prefix is configured.
(define (ninja-dawn-skip? clauses)
  (and (ninja-dawn? clauses) (not (dawn-prefix))))

(define (ninja-dawn-includes clauses base)
  (if (ninja-dawn? clauses)
      (string-append base " $dawnincludes")
      base))

(define (ninja-emit-library table dir form)
  (let* ((name (cadr form))
         (clauses (cddr form)))
    (if (ninja-dawn-skip? clauses)
        #t
        (let* ((includes (ninja-dawn-includes clauses
                                              (ninja-include-flags
                                               (resolve-includes table name))))
               (objs (map (lambda (s)
                            (ninja-emit-compile name dir includes s))
                          (ninja-sources clauses)))
               (lib (string-append "lib" name ".a")))
          (ninja-emit (string-append "build " lib ": ar "
                                     (ninja-join " " objs)))
          (ninja-emit "")
          (ninja-native! lib)))))

(define (ninja-emit-executable table dir form)
  (let* ((name (cadr form))
         (clauses (cddr form)))
    (if (ninja-dawn-skip? clauses)
        #t
        (let* ((dawn (ninja-dawn? clauses))
               (includes (ninja-dawn-includes clauses
                                              (ninja-include-flags
                                               (resolve-includes table name))))
               (objs (map (lambda (s)
                            (ninja-emit-compile name dir includes s))
                          (ninja-sources clauses)))
               (libs (map (lambda (l) (string-append "lib" l ".a"))
                          (resolve-link-libs table name)))
               (syslibs (resolve-syslibs table name))
               (ldlibs (append (map (lambda (l) (string-append "-l" l))
                                    syslibs)
                               (if dawn (list "$dawnlibs") '())))
               (bin (string-append "bin/" name)))
          ;;! s7 is a prebuilt archive (kruddage/s7 release, fetched by
          ;;! third_party/sync.sh) rather than an object baked into libscript.a.
          ;;! It rides after the engine archives on every executable link: as a
          ;;! static archive the linker pulls only the members a target actually
          ;;! references, so binaries that never call s7 are byte-for-byte
          ;;! unchanged, and it must come last because libscript.a references it.
          (ninja-emit (string-append "build " bin ": "
                                     (if dawn "link_cxx" "link") " "
                                     (ninja-join " " (append objs libs
                                                             (list "$s7nativelib")))))
          (if (pair? ldlibs)
              (ninja-emit (string-append "  ldlibs = "
                                         (ninja-join " " ldlibs))))
          (ninja-emit "")
          ;;! Ordinary executables are pulled into the `native` target by the
          ;;! (test ...) edge that runs them. A `(dawn)` binary has none — it
          ;;! needs a real GPU adapter, so it must not become a CI test — and it
          ;;! is itself the deliverable, so name it directly or nothing would
          ;;! ever build it.
          (if dawn (ninja-native! bin))))))

(define (ninja-emit-test form)
  (let* ((name (cadr form))
         (cmd (caddr form))
         (stamp (string-append "test/" name ".stamp")))
    (ninja-emit (string-append "build " stamp ": run_test bin/" cmd))
    (ninja-emit "")
    (ninja-native! stamp)))

(define (ninja-emit-form table dir form)
  (case (car form)
    ((library) (ninja-emit-library table dir form))
    ((interface-library) #t)
    ((executable) (ninja-emit-executable table dir form))
    ((test) (ninja-emit-test form))
    ((wasm-only) #t)
    ((native-only)
     (for-each (lambda (f) (ninja-emit-form table dir f)) (cdr form)))
    (else #t)))

;;! Native builds pick up the host compiler and any extra flags from the
;;! environment so a sanitizer or coverage build is a first-class variant of
;;! the ordinary native build rather than a bolt-on that rewrites this file:
;;!   KRUDD_CC / KRUDD_CXX        override the native C / C++ compiler (gcc, …)
;;!   KRUDD_EXTRA_CFLAGS          extra compile flags (e.g. -fsanitize=…, --coverage)
;;!   KRUDD_EXTRA_LDFLAGS         extra link flags (the same, on the link step)
;;! All default to empty / cc / c++, so a plain `krudd build` is byte-for-byte
;;! unchanged. Only the native cc/cxx/link rules honor them — the WASM (emcc)
;;! path is deliberately left uninstrumented, and s7 is a prebuilt archive so it
;;! is inherently uninstrumented (and, being third-party, excluded from coverage).
(define (ninja-preamble srcroot)
  (let ((native-cc (or (getenv "KRUDD_CC") "cc"))
        (native-cxx (or (getenv "KRUDD_CXX") "c++"))
        (extracflags (or (getenv "KRUDD_EXTRA_CFLAGS") ""))
        (extraldflags (or (getenv "KRUDD_EXTRA_LDFLAGS") "")))
    (list
     "# Generated by krudd — do not edit by hand."
     "# Source of truth: krudd/engine/**/build.scm, rendered by"
     "# krudd/kruddmake/ninja.scm."
     "# Regenerate: see krudd/kruddmake/run-tests.sh"
     ""
     "ninja_required_version = 1.10"
     (string-append "srcroot = " srcroot)
     (string-append "cc = " native-cc)
     (string-append "cxx = " native-cxx)
     "ar = ar"
     "emcc = emcc"
     "empp = em++"
     "emar = emar"
     (string-append "extracflags = " extracflags)
     (string-append "extraldflags = " extraldflags)
     ;;! Empty unless KRUDD_DAWN_PREFIX is set — and when it is unset nothing
     ;;! references these, because the `(dawn)` targets that would are skipped.
     ;;! See ninja-dawn-skip?.
     (string-append "dawnprefix = " (or (dawn-prefix) ""))
     "dawnincludes = -I$dawnprefix/include"
     ;;! One monolithic archive (DAWN_BUILD_MONOLITHIC_LIBRARY=STATIC) so a
     ;;! non-CMake consumer links one artifact instead of reproducing Dawn's
     ;;! dependency graph here. It lands after the engine archives because
     ;;! ninja-emit-executable emits $ldlibs last, which is what static
     ;;! linking requires.
     (string-append "dawnlibs = $dawnprefix/lib/libwebgpu_dawn.a "
                    "-lz -ldl -lpthread -lm")
     ;;! -fPIC on every object, because the SDK toolchain links executables as
     ;;! PIE by default. A non-PIC object that references an exported *data*
     ;;! symbol from a shared library (libc's stdout/stderr, say) forces the
     ;;! linker to emit a COPY relocation: the symbol is duplicated into the
     ;;! executable's .bss, and the shared library's own internal reference no
     ;;! longer sees writes to the executable's copy. -fPIC routes the access
     ;;! through the GOT instead, so there is a single shared copy and no
     ;;! divergence. Cheap insurance for the C sources (issue #715).
     "cflags = -std=gnu11 -fPIC -Wall -Werror -Wpedantic"
     ;;! C++20 (not gnu11 — that was a stale copy of $cflags from before any
     ;;! native .cpp source existed to compile with it) so a C++ source can use
     ;;! designated initializers, which in standard C++ rather than as a GNU
     ;;! extension need C++20 — the same `.field = value` struct init the
     ;;! engine's C sources use, so a .cpp reads as the same table as its C
     ;;! neighbours rather than a differently-shaped one.
     "cxxflags = -std=c++20 -fPIC -Wall -Werror -Wpedantic"
     ;;! --use-port=emdawnwebgpu enables the WebGPU (Dawn) headers + JS glue;
     ;;! emscripten requires it at both compile and link, so it rides on the
     ;;! wasm C compile flags here and the main-module link flags below.
     "emcflags = -std=gnu11 -Wall -Werror -Wpedantic --use-port=emdawnwebgpu"
     ;;! s7 ships as prebuilt static archives from the kruddage/s7 release,
     ;;! fetched + checksum-verified into ../third_party by sync.sh (see
     ;;! s7.artifact / VENDOR.md). s7.h lands in the same directory, already on
     ;;! the include path via each build.scm's (private (raw "../third_party")).
     "s7nativelib = $srcroot/../third_party/libs7-linux-x86_64.a"
     "s7wasmlib = $srcroot/../third_party/libs7-wasm32.a"
     ;;! The exported surface is mirrored by @kruddage/engine's
     ;;! ENGINE_EXPORTED_FUNCTIONS, which its build script checks back against
     ;;! the linked module — so this list and that one are checked to agree
     ;;! rather than trusted to. _malloc/_free are here because the Load Project
     ;;! path passes a variable-length string INTO the module (every other
     ;;! JS bridge in the tree passes out), and a buffer for it has to come from
     ;;! somewhere the module owns; project_host.c's EM_JS bridge is the only
     ;;! caller, and it frees what it allocates in the same call.
     (string-append "mainflags = -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 "
                    "-sGROWABLE_ARRAYBUFFERS=0 -sMALLOC=mimalloc "
                    "-sFETCH=1 -sMAX_WEBGL_VERSION=2 --use-port=emdawnwebgpu "
                    "-sEXPORTED_FUNCTIONS=_main,_krudd_load_game,"
                    "_krudd_load_project,_malloc,_free")
     ""
     "rule cc"
     "  command = $cc $cflags $extracflags $includes -MMD -MF $out.d -c $in -o $out"
     "  depfile = $out.d"
     "  deps = gcc"
     "  description = CC $out"
     ""
     "rule cxx"
     "  command = $cxx $cxxflags $extracflags $includes -MMD -MF $out.d -c $in -o $out"
     "  depfile = $out.d"
     "  deps = gcc"
     "  description = CXX $out"
     ""
     "rule ar"
     "  command = rm -f $out && $ar rcs $out $in"
     "  description = AR $out"
     ""
     "rule link"
     "  command = $cc $extraldflags $in $ldlibs -o $out"
     "  description = LINK $out"
     ""
     ;;! libwebgpu_dawn.a is C++, so a `(dawn)` executable needs a C++ driver
     ;;! for the final link. Deliberately a separate rule rather than flipping
     ;;! `link` over: the native *_test binaries are pure C and should not grow
     ;;! a libstdc++ dependency for nothing.
     "rule link_cxx"
     "  command = $cxx $extraldflags $in $ldlibs -o $out"
     "  description = LINK(c++) $out"
     ""
     "rule run_test"
     "  command = $in && touch $out"
     "  description = TEST $out"
     ""
     "rule emcc_c"
     "  command = $emcc $emcflags $includes -MMD -MF $out.d -c $in -o $out"
     "  depfile = $out.d"
     "  deps = gcc"
     "  description = EMCC $out"
     ""
     "rule emcc_cxx"
     "  command = $empp -O2 $emcxxflags $includes -MMD -MF $out.d -c $in -o $out"
     "  depfile = $out.d"
     "  deps = gcc"
     "  description = EMCXX $out"
     ""
     "rule emar"
     "  command = rm -f $out && $emar rcs $out $in"
     "  description = EMAR $out"
     ""
     "rule main_module"
     "  command = $empp $mainflags $extraflags $in -o $out"
     "  description = LINK(wasm) $out"
     ""
     "rule copy"
     "  command = cp $in $out"
     "  description = COPY $out"
     ""
     ;;! Re-run the generator so that editing a `.scm`/`.in` input (a build
     ;;! spec, or any Scheme module embedded into a `*_scm.h`) regenerates
     ;;! build.ninja and the codegen outputs before the rest of the build. The
     ;;! regenerated headers then flow to their consumers through the gcc
     ;;! depfiles on the compile rules above.
     "rule regen"
     "  command = $regen_cmd"
     "  generator = 1"
     "  description = REGEN $out"
     "")))

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

(define (ninja-wasm-compile-rule src)
  (if (or (ninja-suffix? src ".cpp") (ninja-suffix? src ".cc"))
      "emcc_cxx" "emcc_c"))

(define (ninja-emit-wasm-lib table libmap name)
  (let* ((entry (assoc name libmap))
         (dir (cadr entry))
         (clauses (cddr entry))
         (sources (ninja-sources clauses))
         (cxxflags (let ((c (rz-clause 'wasm-flags clauses)))
                     (if c (ninja-join " " (cdr c)) "")))
         (includes (ninja-wasm-include-flags (resolve-includes table name)))
         (objs (map (lambda (s)
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
                    sources))
         (lib (string-append "wasm/lib" name ".a")))
    (ninja-emit (string-append "build " lib ": emar "
                               (ninja-join " " objs)))
    (ninja-emit "")
    lib))

(define (ninja-emit-main-module table libmap)
  (let* ((dir "core")
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
    ;;! The prebuilt wasm s7 archive (kruddage/s7 release, fetched by
    ;;! third_party/sync.sh) links into the single wasm module here, the same
    ;;! role $s7nativelib plays for native binaries. It carries the wasm-only
    ;;! KRUDD-LOCAL PATCH that used to live in this repo's s7.c.
    (ninja-emit (string-append
                 "build index.html | index.js index.wasm: main_module "
                 (ninja-join " " (append objs libs (list "$s7wasmlib")))))
    (ninja-emit (string-append "  extraflags = --extern-pre-js "
                               "$srcroot/shell/web/error_overlay.js "
                               "--shell-file generated/shell.html"))
    (ninja-emit "")
    (ninja-wasm! "index.html")))

;;! PWA static assets served alongside index.html — plain copies from
;;! core/ into the build root, so the staging step (packages/site, via
;;! @kruddage/engine's artifact contract) can pick them up next to the hashed
;;! JS/WASM outputs. Unlike those, these filenames aren't hashed, so the
;;! service worker itself must tolerate that (see sw.js).
(define (ninja-emit-static-assets srcroot)
  (for-each
   (lambda (name)
     (ninja-emit (string-append "build " name ": copy "
                                (string-append srcroot "/shell/web/" name)))
     (ninja-wasm! name))
   (list "manifest.webmanifest" "sw.js" "icon-192.png" "icon-512.png"))
  (ninja-emit ""))

;;! The runtime asset directory served next to index.html, and the index the
;;! shell reads out of it. @kruddage/engine copies this directory wholesale into
;;! its dist/ and packages/site stages it to the site (ENGINE_ASSET_DIR), so
;;! anything landing here is reachable from the page as `assets/<name>`.
(define ninja-assets-dir "assets")

(define ninja-project-index "projects.json")

;;! Copy every shipped project's source into assets/ under its own name — the
;;! staged one and every served one alike. For the staged project this is the
;;! other half of the declaration whose embed ninja-run-codegen handles: the same
;;! file, reachable at runtime by fetch instead of compiled into the module. For
;;! a served project it is the whole of the declaration. Either way the shell's
;;! Load Project control opens it by the same door a file off the user's disk
;;! comes in by, which is what keeps a shipped project from being a special case
;;! in the engine.
(define (ninja-emit-project-assets manifest)
  (for-each
   (lambda (decl)
     (let* ((src (rz-codegen-source decl))
            (out (string-append ninja-assets-dir "/" (krudd-basename src))))
       (ninja-emit (string-append "build " out ": copy $srcroot/" src))
       (ninja-wasm! out)))
   (resolve-shipped-projects manifest))
  (ninja-emit ""))

;;! Write assets/projects.json: the shipped project filenames, as a JSON array,
;;! staged one first.
;;!
;;! The shell cannot list a directory over HTTP, and it must not be told a
;;! filename either — a page carrying a project's filename is a generic shell
;;! that knows a game (#976). So the build, which is the only layer that knows
;;! what it shipped, writes down what it shipped and the page reads it. Written
;;! here at generation time rather than as a ninja edge for the same reason the
;;! generated/ headers are: its content is a fact about the manifest, fixed the
;;! moment the manifest is read, and the regen edge already re-runs the
;;! generator when a build.scm changes.
(define (ninja-generate-project-index manifest builddir)
  (let ((dir   (string-append builddir "/" ninja-assets-dir))
        (names (map (lambda (decl)
                      (krudd-basename (rz-codegen-source decl)))
                    (resolve-shipped-projects manifest))))
    (system (string-append "mkdir -p \"" dir "\""))
    (call-with-output-file (string-append dir "/" ninja-project-index)
      (lambda (port)
        (write-string
         (string-append "["
                        (ninja-join ","
                                    (map (lambda (n)
                                           (string-append "\"" n "\""))
                                         names))
                        "]\n")
         port)))))

(define (ninja-codegen-input srcroot decl)
  (string-append srcroot "/" (rz-codegen-source decl)))

;;! Dispatch one declaration onto its introspect.scm generator. Outputs are
;;! named relative to `generated/` in the declaration, since that is the one
;;! directory codegen writes to and every consumer reaches it the one way, as
;;! `(raw "${generated}")`.
(define (ninja-run-codegen srcroot gen decl)
  (let ((in   (ninja-codegen-input srcroot decl))
        (args (rz-codegen-args decl)))
    (define (out n) (string-append gen "/" n))
    (case (rz-codegen-kind decl)
      ((configure-file) (krudd-configure-file in (out (car args))))
      ((embed) (krudd-embed-file in (out (car args)) (cadr args)))
      ;;! The same embed under a fixed header and symbol. The copy into assets/
      ;;! the one declaration also buys is a ninja edge, emitted below by
      ;;! ninja-emit-project-assets.
      ((staged-project)
       (krudd-embed-file in (out rz-staged-project-header)
                         rz-staged-project-symbol))
      ;;! A served project generates nothing — it is the staged declaration with
      ;;! the embed taken away, so the copy edge ninja-emit-project-assets emits
      ;;! is the whole of it. It passes through here rather than being filtered
      ;;! out upstream so that the `else` below keeps meaning "a kind resolve.scm
      ;;! knows and this dispatch forgot", which is the typo it is here to catch.
      ((served-project) #t)
      ((embed-scheme-module)
       (krudd-embed-scheme-module in (out (car args)) (out (cadr args))))
      ((emit-math-module) (krudd-emit-math-module in (out (car args))))
      ((emit-interface-header)
       (krudd-emit-interface-header in (out (car args))))
      (else (error 'ninja-unknown-codegen-kind (rz-codegen-kind decl))))))

;;! Run the code generation each module declares in its own build.scm. Nothing
;;! is listed here: what gets generated is exactly what the manifest declares,
;;! which is also exactly what the `regen` edge below watches.
(define (ninja-generate-codegen manifest srcroot builddir)
  (let ((gen (string-append builddir "/generated")))
    (system (string-append "mkdir -p \"" gen "\""))
    (for-each (lambda (decl) (ninja-run-codegen srcroot gen decl))
              (resolve-codegen manifest))))

;;! The `.scm`/`.in` inputs that feed code generation, derived from the same
;;! declarations that drive it. When any of these change the `regen` edge below
;;! re-runs the generator, which rewrites build.ninja and the codegen outputs
;;! (`generated/*`). A source can no longer be generated from but left unwatched:
;;! it takes one declaration to be both, so the list cannot fall behind (#779).
(define (ninja-generator-inputs manifest srcroot)
  (append
   (map (lambda (decl) (ninja-codegen-input srcroot decl))
        (resolve-codegen manifest))
   (map (lambda (p) (string-append (krudd-repo-root)
                                   "/krudd/kruddmake/" p))
        (list "ninja.scm" "introspect.scm" "resolve.scm"
              "build.scm" "manifest.scm"))
   (map (lambda (pair)
          (string-append srcroot "/" (car pair) "/build.scm"))
        manifest)))

;;! Emit the generator edge. `regen-cmd` is the exact shell command that
;;! regenerates this build.ninja (and, as a side effect, the codegen outputs);
;;! each entry point supplies its own, since the build dir and interpreter
;;! differ between `krudd build` and the kruddmake test harness.
(define (ninja-emit-regen manifest srcroot regen-cmd)
  (if (and (string? regen-cmd) (> (string-length regen-cmd) 0))
      (begin
        (ninja-emit (string-append
                     "build build.ninja: regen "
                     (ninja-join " "
                                 (ninja-generator-inputs manifest srcroot))))
        (ninja-emit (string-append "  regen_cmd = " regen-cmd))
        (ninja-emit ""))))

(define (ninja-synthesize manifest srcroot . rest)
  (let ((builddir (if (pair? rest) (car rest) #f))
        (regen-cmd (if (and (pair? rest) (pair? (cdr rest)))
                       (cadr rest) #f)))
    (set! ninja-lines '())
    (set! ninja-native '())
    (set! ninja-wasm '())
    (let ((table (rz-target-table manifest))
          (libmap (ninja-build-libmap manifest)))
      (resolve-check-all table)
      (resolve-check-codegen manifest)
      (resolve-check-tiers manifest)
      (ninja-emit* (ninja-preamble srcroot))
      (ninja-emit-regen manifest srcroot regen-cmd)
      (for-each
       (lambda (pair)
         (for-each (lambda (form)
                     (ninja-emit-form table (car pair) form))
                   (cdr pair)))
       manifest)
      (ninja-emit "# --- WASM (Emscripten) main module ---")
      (ninja-emit "")
      (if builddir
          (begin
            (ninja-generate-codegen manifest srcroot builddir)
            (ninja-generate-project-index manifest builddir)))
      (ninja-emit-static-assets srcroot)
      (ninja-emit-project-assets manifest)
      (ninja-emit-main-module table libmap)
      (ninja-emit (string-append "build native: phony "
                                 (ninja-join " " (reverse ninja-native))))
      (ninja-emit (string-append "build wasm: phony "
                                 (ninja-join " " (reverse ninja-wasm))))
      (ninja-emit "default native")
      (ninja-emit "")
      (ninja-join "\n" (reverse ninja-lines)))))
