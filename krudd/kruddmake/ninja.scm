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

;;! Qt is the native windowing toolkit for the `(qt)` clause — a system
;;! dependency (headers + libQt6Widgets/Gui/Core), never a target, so like
;;! `(dawn)` it rides preamble variables rather than the `(link)` list
;;! resolve.scm validates. Qt is never on a default include path (its headers
;;! live in framework-style per-module directories), so there is no usable
;;! cflags default — KRUDD_QT_CFLAGS must be set, normally from
;;! `pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core`. Like `(dawn)`, it is
;;! OPT-IN: a `(qt)` target is left out of the native graph unless KRUDD_QT is
;;! set, so a plain build — and a KRUDD_DAWN_PREFIX build that only wants the
;;! offscreen krudd_native — is byte-for-byte unchanged and needs no Qt
;;! installed. `./krudd.sh editor` sets KRUDD_QT for you.
(define (qt-configured?) (getenv "KRUDD_QT"))

(define (ninja-qt? clauses) (if (rz-clause 'qt clauses) #t #f))

(define (ninja-qt-skip? clauses)
  (and (ninja-qt? clauses) (not (qt-configured?))))

(define (ninja-qt-includes clauses base)
  (if (ninja-qt? clauses)
      (string-append base " $qtcflags")
      base))

;;! Vulkan is the native editor's GPU backend (SteamOS / the Deck / Windows).
;;! Its loader and headers (<vulkan/vulkan.h>, -lvulkan) are a system
;;! dependency, so — like `(dawn)` and `(qt)` — a `(vulkan)` target is OPT-IN:
;;! without KRUDD_VULKAN in the environment it is left out of the native graph
;;! entirely, so a plain `krudd build` is byte-for-byte unchanged and CI (which
;;! installs no Vulkan) stays green. The WASM/web target never references a
;;! `(vulkan)` library, so the browser keeps WebGL and WebGPU untouched.
;;! `./krudd.sh editor` sets KRUDD_VULKAN for you. Vulkan headers are normally on
;;! the default include path, so $vulkanincludes is empty unless
;;! KRUDD_VULKAN_CFLAGS overrides it; $vulkanlibs defaults to -lvulkan.
(define (vulkan-configured?) (getenv "KRUDD_VULKAN"))

(define (ninja-vulkan? clauses) (if (rz-clause 'vulkan clauses) #t #f))

(define (ninja-vulkan-skip? clauses)
  (and (ninja-vulkan? clauses) (not (vulkan-configured?))))

(define (ninja-vulkan-includes clauses base)
  (if (ninja-vulkan? clauses)
      (string-append base " $vulkanincludes")
      base))

(define (ninja-emit-library table dir form)
  (let* ((name (cadr form))
         (clauses (cddr form)))
    (if (or (ninja-dawn-skip? clauses) (ninja-vulkan-skip? clauses))
        #t
        (let* ((includes (ninja-vulkan-includes
                          clauses
                          (ninja-dawn-includes clauses
                                               (ninja-include-flags
                                                (resolve-includes table name)))))
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
    (if (or (ninja-dawn-skip? clauses) (ninja-qt-skip? clauses)
            (ninja-vulkan-skip? clauses))
        #t
        (let* ((dawn (ninja-dawn? clauses))
               (qt (ninja-qt? clauses))
               (vulkan (ninja-vulkan? clauses))
               (includes (ninja-vulkan-includes
                          clauses
                          (ninja-qt-includes
                           clauses
                           (ninja-dawn-includes clauses
                                                (ninja-include-flags
                                                 (resolve-includes table name))))))
               (objs (map (lambda (s)
                            (ninja-emit-compile name dir includes s))
                          (ninja-sources clauses)))
               (libs (map (lambda (l) (string-append "lib" l ".a"))
                          (resolve-link-libs table name)))
               (syslibs (resolve-syslibs table name))
               (ldlibs (append (map (lambda (l) (string-append "-l" l))
                                    syslibs)
                               (if dawn (list "$dawnlibs") '())
                               (if vulkan (list "$vulkanlibs") '())
                               (if qt (list "$qtlibs") '())))
               (bin (string-append "bin/" name)))
          ;;! s7 is a prebuilt archive (kruddage/s7 release, fetched by
          ;;! third_party/sync.sh) rather than an object baked into libscript.a.
          ;;! It rides after the engine archives on every executable link: as a
          ;;! static archive the linker pulls only the members a target actually
          ;;! references, so binaries that never call s7 are byte-for-byte
          ;;! unchanged, and it must come last because libscript.a references it.
          (ninja-emit (string-append "build " bin ": "
                                     (if (or dawn qt) "link_cxx" "link") " "
                                     (ninja-join " " (append objs libs
                                                             (list "$s7nativelib")))))
          (if (pair? ldlibs)
              (ninja-emit (string-append "  ldlibs = "
                                         (ninja-join " " ldlibs))))
          (ninja-emit "")
          ;;! Ordinary executables are pulled into the `native` target by the
          ;;! (test ...) edge that runs them. A `(dawn)`, `(vulkan)` or `(qt)`
          ;;! binary has none — it needs a real GPU adapter and/or a window to
          ;;! open, so it must not become a CI test — and it is itself the
          ;;! deliverable, so name it directly or nothing would ever build it.
          (if (or dawn qt vulkan) (ninja-native! bin))))))

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
     ;;! Qt (the `(qt)` clause, the editor shell's windowing toolkit). Qt has
     ;;! no default include/link path worth guessing at —
     ;;! KRUDD_QT_CFLAGS is required, not optional, normally from
     ;;!   KRUDD_QT_CFLAGS="$(pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core)"
     ;;!   KRUDD_QT_LIBS="$(pkg-config --libs Qt6Widgets Qt6Gui Qt6Core)"
     ;;! The libs default guesses the common `-lQt6Foo` shape so a system with
     ;;! Qt on its default library search path only needs KRUDD_QT_CFLAGS.
     ;;! Empty of consequence unless a `(qt)` target is in the graph, which
     ;;! only happens when KRUDD_QT is set (see ninja-qt-skip?).
     (string-append "qtcflags = " (or (getenv "KRUDD_QT_CFLAGS") ""))
     (string-append "qtlibs = " (or (getenv "KRUDD_QT_LIBS")
                                    "-lQt6Widgets -lQt6Gui -lQt6Core"))
     ;;! Vulkan (the `(vulkan)` clause, the native editor's GPU backend). The
     ;;! loader and headers are an ordinary system dependency: the headers are
     ;;! on the default include path (so $vulkanincludes is empty unless
     ;;! KRUDD_VULKAN_CFLAGS overrides it, e.g. for an SDK in a non-standard
     ;;! prefix), and $vulkanlibs defaults to the loader, -lvulkan. Empty of
     ;;! consequence unless a `(vulkan)` target is in the graph, which only
     ;;! happens when KRUDD_VULKAN is set (see ninja-vulkan-skip?).
     (string-append "vulkanincludes = " (or (getenv "KRUDD_VULKAN_CFLAGS") ""))
     (string-append "vulkanlibs = " (or (getenv "KRUDD_VULKAN_LIBS")
                                        "-lvulkan"))
     ;;! -fPIC on every object, because the SDK toolchain links executables as
     ;;! PIE by default. A non-PIC object that references an exported *data*
     ;;! symbol from a shared library (e.g. QCoreApplication::self, or libc's
     ;;! stdout/stderr) forces the linker to emit a COPY relocation: the symbol
     ;;! is duplicated into the executable's .bss, and the shared library's own
     ;;! internal reference no longer sees writes to the executable's copy. For
     ;;! Qt that desyncs QCoreApplication::self — QApplication's ctor sets one
     ;;! copy while screen setup reads the other (still null), a null-deref at
     ;;! offset 8 deep in libQt6Core during construction (issue #715). -fPIC
     ;;! routes the access through the GOT instead, so there is a single shared
     ;;! copy and no crash. Cheap insurance for the C sources; mandatory once a
     ;;! `(qt)` executable is in the graph.
     "cflags = -std=gnu11 -fPIC -Wall -Werror -Wpedantic"
     ;;! C++20 (not gnu11 — that was a stale copy of $cflags from before any
     ;;! native .cpp source existed to compile with it) so a `(qt)` source can
     ;;! use Qt6 (which requires C++17) and designated initializers (which,
     ;;! in standard C++ rather than as a GNU extension, need C++20) — the
     ;;! same `.field = value` struct init the engine's C sources use, kept
     ;;! for krudd_qt.cpp so it reads as the same table as the engine's C
     ;;! sources, not a differently-shaped one. -Wpedantic is unforgiving of
     ;;! Qt's own headers on some toolchains; if a Qt roll ever trips it, narrow the
     ;;! flag to the qt-only compile rule rather than loosening it globally.
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
     (string-append "mainflags = -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 "
                    "-sGROWABLE_ARRAYBUFFERS=0 -sMALLOC=mimalloc "
                    "-sFETCH=1 -sMAX_WEBGL_VERSION=2 --use-port=emdawnwebgpu "
                    "-sEXPORTED_FUNCTIONS=_main,_krudd_load_game")
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
                               "$srcroot/core/error_overlay.js "
                               "--shell-file generated/shell.html"))
    (ninja-emit "")
    (ninja-wasm! "index.html")))

;;! PWA static assets served alongside index.html — plain copies from
;;! core/ into the build root, so GitHub Pages (and stage-site.sh) can pick
;;! them up next to the hashed JS/WASM outputs. Unlike those, these filenames
;;! aren't content-hashed, so the service worker itself must tolerate that
;;! (see sw.js).
(define (ninja-emit-static-assets srcroot)
  (for-each
   (lambda (name)
     (ninja-emit (string-append "build " name ": copy "
                                (string-append srcroot "/core/" name)))
     (ninja-wasm! name))
   (list "manifest.webmanifest" "sw.js" "icon-192.png" "icon-512.png"))
  (ninja-emit ""))

(define (ninja-generate-codegen srcroot builddir)
  (let ((gen      (string-append builddir "/generated"))
        (mdscm    (string-append (krudd-repo-root)
                                 "/krudd/engine/ui/kruddboard/md_parse.scm"))
        (mathscm  (string-append (krudd-repo-root)
                                 "/krudd/engine/math/math.scm"))
        (shaderscm (string-append (krudd-repo-root)
                                  "/krudd/engine/shader/shader.scm"))
        (rendscm  (string-append (krudd-repo-root)
                                 "/krudd/engine/render/renderer.scm")))
    (system (string-append "mkdir -p \"" gen "\""))
    (krudd-configure-file
     (string-append srcroot "/core/version.h.in")
     (string-append gen "/version.h"))
    (krudd-configure-file
     (string-append srcroot "/core/shell.html.in")
     (string-append gen "/shell.html"))
    (krudd-embed-file
     (string-append srcroot "/core/runtime.scm")
     (string-append gen "/runtime_scm.h") "RUNTIME_SCM")
    (krudd-embed-file
     (string-append srcroot "/core/entity_script.scm")
     (string-append gen "/entity_script_scm.h") "ENTITY_SCRIPT_SCM")
    (krudd-embed-file
     (string-append srcroot "/core/mesh_script.scm")
     (string-append gen "/mesh_script_scm.h") "MESH_SCRIPT_SCM")
    (krudd-embed-file
     (string-append srcroot "/core/texture_script.scm")
     (string-append gen "/texture_script_scm.h") "TEXTURE_SCRIPT_SCM")
    (krudd-embed-file
     (string-append srcroot "/core/sound_script.scm")
     (string-append gen "/sound_script_scm.h") "SOUND_SCRIPT_SCM")
    (krudd-embed-file
     (string-append srcroot "/core/scene_script.scm")
     (string-append gen "/scene_script_scm.h") "SCENE_SCRIPT_SCM")
    (krudd-embed-file
     (string-append srcroot "/games/tictactoe/scene.scm")
     (string-append gen "/tictactoe_scene_scm.h")
     "TICTACTOE_SCENE_SCM")
    (krudd-embed-file
     (string-append srcroot "/games/tictactoe/rules.scm")
     (string-append gen "/tictactoe_rules_scm.h")
     "TICTACTOE_RULES_SCM")
    (krudd-embed-file
     (string-append srcroot "/games/chess/scene.scm")
     (string-append gen "/chess_scene_scm.h")
     "CHESS_SCENE_SCM")
    (krudd-embed-file
     (string-append srcroot "/games/chess/rules.scm")
     (string-append gen "/chess_rules_scm.h")
     "CHESS_RULES_SCM")
    (krudd-embed-file
     (string-append srcroot "/ui/kruddgui/kruddgui.scm")
     (string-append gen "/kruddgui_scm.h") "KRUDDGUI_SCM")
    (krudd-embed-scheme-module
     mdscm
     (string-append gen "/md_parse.h")
     (string-append gen "/md_parse.scm.c"))
    (krudd-emit-math-module
     mathscm
     (string-append gen "/math_gen.c"))
    (krudd-embed-file
     shaderscm
     (string-append gen "/shader_scm.h") "SHADER_SCM")
    (krudd-emit-interface-header
     rendscm
     (string-append gen "/renderer.h"))))

;;! The `.scm`/`.in` inputs that feed code generation. When any of these change
;;! the `regen` edge below re-runs the generator, which rewrites build.ninja and
;;! the codegen outputs (`generated/*`). Kept a deliberate superset so a stale
;;! header can never outlive an edit to its source.
(define (ninja-generator-inputs manifest srcroot)
  (append
   (map (lambda (p) (string-append srcroot "/" p))
        (list "core/version.h.in"
              "core/shell.html.in"
              "core/runtime.scm"
              "core/entity_script.scm"
              "core/mesh_script.scm"
              "core/texture_script.scm"
              "core/sound_script.scm"
              "ui/kruddgui/kruddgui.scm"
              "ui/kruddboard/md_parse.scm"
              "math/math.scm"
              "shader/shader.scm"
              "render/renderer.scm"))
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
      (if builddir (ninja-generate-codegen srcroot builddir))
      (ninja-emit-static-assets srcroot)
      (ninja-emit-main-module table libmap)
      (ninja-emit (string-append "build native: phony "
                                 (ninja-join " " (reverse ninja-native))))
      (ninja-emit (string-append "build wasm: phony "
                                 (ninja-join " " (reverse ninja-wasm))))
      (ninja-emit "default native")
      (ninja-emit "")
      (ninja-join "\n" (reverse ninja-lines)))))
