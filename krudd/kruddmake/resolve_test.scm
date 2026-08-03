; SPDX-License-Identifier: GPL-2.0-or-later

(define krudd-root (or (getenv "KRUDD_ROOT") "."))

(load (string-append krudd-root "/krudd/kruddmake/ninja.scm"))

(define fail-count 0)

(define (check name ok)
  (if ok
      (display (string-append "  ok    " name "\n"))
      (begin
        (set! fail-count (+ fail-count 1))
        (display (string-append "  FAIL  " name "\n")))))

(define (subset? a b)
  (cond ((null? a) #t)
        ((member (car a) b) (subset? (cdr a) b))
        (else #f)))

(define (set=? a b) (and (subset? a b) (subset? b a)))

(define (expect-error thunk)
  (catch #t (lambda () (thunk) #f) (lambda args #t)))

;;! What a raised error actually said. s7 hands the handler (tag (arg ...)), and
;;! every error checked for its wording here carries one string argument.
(define (error-text thunk)
  (catch #t
         (lambda () (thunk) "")
         (lambda args
           (let ((info (cadr args)))
             (if (and (pair? info) (string? (car info))) (car info) "")))))

(define (contains? hay needle)
  (let ((hl (string-length hay)) (nl (string-length needle)))
    (let loop ((i 0))
      (cond ((> (+ i nl) hl) #f)
            ((string=? (substring hay i (+ i nl)) needle) #t)
            (else (loop (+ i 1)))))))

(define (index-of x lst)
  (let loop ((l lst) (i 0))
    (cond ((null? l) -1)
          ((equal? (car l) x) i)
          (else (loop (cdr l) (+ i 1))))))

(define (load-datum path) (call-with-input-file path read))

(define manifest-dirs
  (load-datum (string-append krudd-root "/krudd/kruddmake/manifest.scm")))

(define (load-spec dir)
  (load-datum (string-append krudd-root "/krudd/engine/" dir
                             "/build.scm")))

(define manifest
  (map (lambda (d) (cons d (load-spec d))) manifest-dirs))

(define table (rz-target-table manifest))

(define (inc-check name expected)
  (check (string-append "includes " name)
         (set=? (resolve-includes table name) expected)))

(display "resolver: include sets vs CMake ground truth\n")
(inc-check "log" '("base/log/include" "abi/include"))
(inc-check "log_test" '("base/log/include" "abi/include"))
(inc-check "renderer_null" '("render/null/include" "${generated}"
                             "base/log/include"
                             "abi/include" "core/include"))
(inc-check "renderer_null_test"
           '("render/null" "render/null/include" "${generated}"
             "base/log/include"
             "abi/include" "core/include"))
(inc-check "fg_test" '("render/frame_graph" "render/frame_graph/include"
                       "${generated}"
                       "render/null/include" "base/log/include"
                       "abi/include" "base/memory/include"
                       "core/include"))
(inc-check "asset_plugin" '("world/asset/include" "abi/include"
                            "base/math/include" "world/entity/include"
                            "base/log/include" "base/memory/include"
                            "core/include"))

(display "resolver: transitive link closures\n")
(let ((libs (resolve-link-libs table "renderer_null_test")))
  (check "closure renderer_null_test membership"
         (set=? libs '("renderer_null" "log" "subsystem"
                       "subsystem_manager")))
  (check "closure renderer_null before its deps"
         (and (< (index-of "renderer_null" libs) (index-of "log" libs))
              (< (index-of "renderer_null" libs)
                 (index-of "subsystem" libs)))))

(display "resolver: loud failures\n")
(let ((cyc (rz-target-table
            (list (cons "d" '((library "A" (link "B"))
                              (library "B" (link "A"))))))))
  (check "cycle in link graph errors"
         (expect-error (lambda () (resolve-includes cyc "A")))))

(let ((unk (rz-target-table
            (list (cons "d" '((library "A" (link "nonesuch"))))))))
  (check "unknown link target errors"
         (expect-error (lambda () (resolve-includes unk "A")))))

(let ((sys (rz-target-table
            (list (cons "d" '((library "A" (sources "a.c") (public "inc")
                                       (link "m"))))))))
  (check "system lib m carries no include dir and no graph edge"
         (and (null? (resolve-link-libs sys "A"))
              (member "d/inc" (resolve-includes sys "A")))))

(check "resolve-check-all over the real manifest"
       (not (expect-error (lambda () (resolve-check-all table)))))

;;! The codegen declarations that used to be a literal in ninja.scm, written out
;;! twice in two different shapes with nothing checking the two agreed (#787).
;;! Both consumers now derive from the manifest, so the literal survives here
;;! instead — as an oracle. Adding an embed has to touch this list, which is a
;;! test failing loudly, not a build silently under-declaring its regen edge the
;;! way #779 did.
(display "codegen: declarations gathered from the manifest\n")

(define codegen (resolve-codegen manifest))

(check "every generated source is declared by the module that owns it"
       (set=? (map rz-codegen-source codegen)
              '("core/version.h.in"
                "shell/web/shell.html.in"
                "core/runtime.scm"
                "world/entity/entity_script.scm"
                "world/entity/scene_script.scm"
                "world/asset/mesh_script.scm"
                "world/asset/texture_script.scm"
                "world/asset/sound_script.scm"
                "game/chess/scene.scm"
                "game/chess/rules.scm"
                "ui/kruddgui/kruddgui.scm"
                "ui/kruddboard/md_parse.scm"
                "base/math/math.scm"
                "render/shader/shader.scm"
                "render/renderer.scm")))

(define (decl-for source)
  (let loop ((l codegen))
    (cond ((null? l) #f)
          ((string=? (rz-codegen-source (car l)) source) (car l))
          (else (loop (cdr l))))))

(define (decl-check name source kind outputs)
  (let ((d (decl-for source)))
    (check name
           (and d
                (eq? (rz-codegen-kind d) kind)
                (set=? (rz-codegen-outputs d) outputs)))))

(decl-check "configure-file declares its substituted output"
            "core/version.h.in" 'configure-file '("version.h"))
(decl-check "embed declares one header, not its C symbol"
            "core/runtime.scm" 'embed '("runtime_scm.h"))
(decl-check "embed-scheme-module declares both of its outputs"
            "ui/kruddboard/md_parse.scm" 'embed-scheme-module
            '("md_parse.h" "md_parse.scm.c"))
(decl-check "emit-math-module declares the C it lowers to"
            "base/math/math.scm" 'emit-math-module '("math_gen.c"))
(decl-check "emit-interface-header declares the header it emits"
            "render/renderer.scm" 'emit-interface-header '("renderer.h"))

(check "an embed's C symbol rides along as an argument, not an output"
       (equal? (rz-codegen-args (decl-for "core/runtime.scm"))
               '("runtime_scm.h" "RUNTIME_SCM")))

(check "a declaration's input resolves against its own module"
       (string=? (rz-codegen-source (decl-for "world/asset/mesh_script.scm"))
                 "world/asset/mesh_script.scm"))

(check "resolve-check-codegen passes over the real manifest"
       (not (expect-error (lambda () (resolve-check-codegen manifest)))))

(display "codegen: loud failures\n")

;;! A form the emitter does not recognise renders nothing — which is exactly how
;;! a mistyped declaration would embed nothing and take its regen input with it.
(check "an unknown top-level form errors rather than rendering nothing"
       (expect-error
        (lambda ()
          (resolve-check-codegen
           (list (cons "d" '((embeds "a.scm" "a_scm.h" "A_SCM"))))))))

(check "a declaration with the wrong argument count errors"
       (expect-error
        (lambda ()
          (resolve-check-codegen
           (list (cons "d" '((embed "a.scm" "a_scm.h"))))))))

(check "two declarations writing the same generated file error"
       (expect-error
        (lambda ()
          (resolve-check-codegen
           (list (cons "d" '((embed "a.scm" "dup.h" "A_SCM")
                             (embed "b.scm" "dup.h" "B_SCM"))))))))

(check "a ${generated} source no declaration produces errors"
       (expect-error
        (lambda ()
          (resolve-check-codegen
           (list (cons "d" '((library "A"
                               (sources (raw "${generated}/nobody.c"))))))))))

(check "a declared ${generated} source is accepted"
       (not (expect-error
             (lambda ()
               (resolve-check-codegen
                (list (cons "d" '((emit-math-module "m.scm" "m_gen.c")
                                  (library "A"
                                    (sources (raw "${generated}/m_gen.c")))))))))))

;;! A declaration nested in (native-only ...) is still a declaration — codegen
;;! runs unconditionally, since the WASM and native builds share one generated/.
(check "declarations inside native-only/wasm-only are gathered too"
       (equal? (map rz-codegen-source
                    (resolve-codegen
                     (list (cons "d" '((native-only
                                        (embed "a.scm" "a_scm.h" "A_SCM")))))))
               '("d/a.scm")))

;;! The tier rule manifest.scm states and resolve-check-tiers enforces. A
;;! boundary check with no failing test is a boundary check that passes because
;;! it is broken, so the inverted edge below is deliberate and asserted red.
(display "tiers: the manifest order, enforced\n")

(check "the real manifest has no library-level tier inversion"
       (null? (rz-tier-inversions manifest)))

(check "resolve-check-tiers passes over the real manifest"
       (not (expect-error (lambda () (resolve-check-tiers manifest)))))

;;! abi is the root of the graph, and until #919 it was not in the graph at all
;;! — so the check above had nothing to say about the tree's highest-fan-in
;;! node. These three assert the shape that makes it the root: first in the
;;! order (so every module may reach it), a surface and no sources (so it
;;! compiles nothing), and no links of its own (so it reaches for nothing).
(check "abi is first in the tier order"
       (= (index-of "abi" manifest-dirs) 0))

(check "abi is an interface-library exporting include/, like every module"
       (let ((target (rz-lookup table "abi")))
         (and target
              (eq? (rz-field target 'kind) 'interface-library)
              (equal? (rz-field target 'public) '("abi/include"))
              (null? (rz-field target 'links)))))

(check "abi emits no build edge"
       (not (contains? (ninja-synthesize
                        manifest (string-append krudd-root "/krudd/engine"))
                       "libabi.a")))

;;! Two modules, `hi` listed above `lo`, and a library in `hi` linking down.
(define inverted-manifest
  (list (cons "hi" '((library "high" (sources "h.c") (link "low"))))
        (cons "lo" '((library "low" (sources "l.c"))))))

(check "a library linking a module listed below it errors"
       (expect-error (lambda () (resolve-check-tiers inverted-manifest))))

(check "the inversion names both modules and their manifest positions"
       (equal? (rz-tier-inversions inverted-manifest)
               '(("high" "hi" "low" "lo" 0 1))))

(check "the failure message spells out the edge and both positions"
       (let ((text (error-text (lambda ()
                                 (resolve-check-tiers inverted-manifest)))))
         (and (contains? text "hi/high links low")
              (contains? text "declared in lo")
              (contains? text "lists hi at position 0")
              (contains? text "lo at position 1"))))

;;! Nothing links an executable, so the main-module link — `index` reaching for
;;! every backend below it — is the program being assembled, not a tier
;;! inversion. This is the shape of core's `index` and `krudd_native`.
(define main-module-manifest
  (list (cons "hi" '((executable "index" (link "low"))))
        (cons "lo" '((library "low" (sources "l.c"))))))

(check "an executable linking a module listed below it is not an inversion"
       (null? (rz-tier-inversions main-module-manifest)))

(define intra-module-manifest
  (list (cons "d" '((library "a" (sources "a.c") (link "b"))
                    (library "b" (sources "b.c"))))))

(check "a link within one module is not an inversion"
       (null? (rz-tier-inversions intra-module-manifest)))

(define upward-manifest
  (list (cons "lo" '((library "low" (sources "l.c"))))
        (cons "hi" '((library "high" (link "low"))))))

(check "a link upward is what the order is for"
       (null? (rz-tier-inversions upward-manifest)))

(define syslib-manifest
  (list (cons "d" '((library "a" (sources "a.c") (link "m"))))))

(check "a system lib carries no module and no tier edge"
       (null? (rz-tier-inversions syslib-manifest)))

(display "emitter: rendered build.ninja\n")

(define (dirname path)
  (let loop ((i (- (string-length path) 1)))
    (cond ((< i 0) ".")
          ((char=? (string-ref path i) #\/) (substring path 0 i))
          (else (loop (- i 1))))))

(define ninja-out (getenv "KRUDD_NINJA_OUT"))

;;! When the harness gives us an s7 interpreter path, wire the generator edge to
;;! re-run this very script, so a `.scm` edit under raw `ninja` regenerates the
;;! codegen headers before recompiling their consumers. Without it the emitted
;;! build.ninja simply has no `regen` edge (fine for the string checks below).
(define s7bin (getenv "KRUDD_S7BIN"))
(define regen-cmd
  (if (and s7bin (> (string-length s7bin) 0)
           ninja-out (> (string-length ninja-out) 0))
      (string-append "env KRUDD_ROOT=" krudd-root
                     " KRUDD_NINJA_OUT=" ninja-out
                     " KRUDD_S7BIN=" s7bin " "
                     s7bin " " krudd-root
                     "/krudd/kruddmake/resolve_test.scm")
      #f))

(define ninja-text
  (if (and ninja-out (> (string-length ninja-out) 0))
      (ninja-synthesize manifest
                        (string-append krudd-root "/krudd/engine")
                        (dirname ninja-out)
                        regen-cmd)
      (ninja-synthesize manifest
                        (string-append krudd-root "/krudd/engine"))))

(check "header present"
       (contains? ninja-text "Generated by krudd"))
(check "log library archive stanza present"
       (contains? ninja-text "build liblog.a: ar "))
(check "log_test link stanza present"
       (contains? ninja-text "build bin/log_test: link "))
(check "log test stamp present"
       (contains? ninja-text "build test/log.stamp: run_test bin/log_test"))
(check "C plugin compiles as a WASM library object (emcc_c), no side-module rule"
       (and (contains? ninja-text
                       (string-append "build wasm-obj/edit_plugin/world/edit/"
                                      "edit_plugin.c.o: emcc_c "))
            (contains? ninja-text "build wasm/libedit_plugin.a: emar ")
            (not (contains? ninja-text "sm_cc"))
            (not (contains? ninja-text "side_module"))))
(check "C++ module compiles with emcc_cxx and its wasm-flags"
       (and (contains? ninja-text
                       (string-append "build wasm-obj/kruddgui/ui/kruddgui/"
                                      "kruddgui.cpp.o: emcc_cxx "))
            (contains? ninja-text "emcxxflags = --std=c++17")))
(check "plugin archive folds into the main module link"
       (and (contains? ninja-text
                       (string-append "main_module wasm-obj/index/core/engine.c.o "
                                      "wasm-obj/index/core/plugin_abi.c.o "))
            (contains? ninja-text "wasm/libedit_plugin.a")
            (contains? ninja-text "wasm/libkruddgui.a")))
(check "default target is native"
       (contains? ninja-text "default native"))
(check "wasm main module stanza present"
       (contains? ninja-text "build index.html | index.js index.wasm: main_module"))
(check "wasm target present"
       (contains? ninja-text "build wasm: phony "))
(check "compile rules track headers via gcc depfiles"
       (and (contains? ninja-text "deps = gcc")
            (contains? ninja-text "depfile = $out.d")))
(if regen-cmd
    (begin
      (check "regen generator edge present"
             (and (contains? ninja-text "build build.ninja: regen ")
                  (contains? ninja-text "generator = 1")))
      ;;! Every declared codegen input, not a spot check: the regen edge is
      ;;! derived from the same declarations, so this is the property #779
      ;;! violated — an embedded source the build would not regenerate for.
      (check "regen edge lists every declared codegen input"
             (let loop ((l codegen))
               (cond ((null? l) #t)
                     ((contains? ninja-text
                                 (string-append " " krudd-root "/krudd/engine/"
                                                (rz-codegen-source (car l))
                                                " "))
                      (loop (cdr l)))
                     (else #f))))))

(if (and ninja-out (> (string-length ninja-out) 0))
    (begin
      (call-with-output-file ninja-out
        (lambda (port) (write-string ninja-text port)))
      (display (string-append "wrote " ninja-out "\n"))))

(if (= fail-count 0)
    (begin (display "RESOLVE-TESTS: OK\n") (exit 0))
    (begin (display (string-append "RESOLVE-TESTS: FAIL ("
                                   (number->string fail-count) ")\n"))
           (exit 1)))
