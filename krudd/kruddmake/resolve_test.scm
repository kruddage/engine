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

(define (load-spec dir) (load-datum (rz-spec-path krudd-root dir)))

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
                "shell/web/sw.js.in"
                "core/runtime.scm"
                "world/entity/entity_script.scm"
                "world/entity/scene_script.scm"
                "world/asset/mesh_script.scm"
                "world/asset/texture_script.scm"
                "world/asset/sound_script.scm"
                "world/asset/material_script.scm"
                "game/project/project.scm"
                "projects/default/default.scm"
                "projects/chess/chess.scm"
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
(decl-check "staged-project declares the one fixed header it embeds under"
            "projects/default/default.scm" 'staged-project
            '("staged_project_scm.h"))

;;! The staged slot is single-occupancy, and since #1019 that is a rule of its
;;! own rather than a side effect of the header name being fixed. Both are still
;;! true — the duplicate output is what resolve-check-codegen sees — so the pair
;;! below checks each mechanism separately. The one that matters is
;;! resolve-check-staged: it is the one that survives the embed ever growing a
;;! per-project symbol, and the one whose message names the rule.
(check "two staged projects collide on the fixed header, as two embeds would"
       (expect-error
        (lambda ()
          (resolve-check-codegen
           (list (cons "a" '((staged-project "a.scm")))
                 (cons "b" '((staged-project "b.scm"))))))))

(check "two staged projects fail the rule of their own"
       (expect-error
        (lambda ()
          (resolve-check-staged
           (list (cons "a" '((staged-project "a.scm")))
                 (cons "b" '((staged-project "b.scm"))))))))

;;! The message is the point of the dedicated check: a reader who declared the
;;! second one needs to be told which two directories claim the slot and that
;;! shipping is the unlimited thing they probably wanted.
(check "the duplicate message names both projects and points at project-source"
       (let ((msg (error-text
                   (lambda ()
                     (resolve-check-staged
                      (list (cons "a" '((staged-project "a.scm")))
                            (cons "b" '((staged-project "b.scm")))))))))
         (and (contains? msg "a/a.scm")
              (contains? msg "b/b.scm")
              (contains? msg "(project-source ...)"))))

;;! Zero is the other direction, and it is an error rather than a permitted
;;! configuration: core/engine.c includes the generated header unconditionally,
;;! so a manifest with no staged project fails at the compiler with a missing
;;! file rather than here with a reason.
(check "no staged project at all is an error, not a permitted build"
       (expect-error
        (lambda ()
          (resolve-check-staged
           (list (cons "a" '((project-source "a.scm"))))))))

(check "resolve-check-staged passes over the real manifest"
       (not (expect-error (lambda () (resolve-check-staged manifest)))))

(check "a staged-project takes no arguments beyond its source"
       (expect-error
        (lambda ()
          (resolve-check-codegen
           (list (cons "d" '((staged-project "a.scm" "A_SCM"))))))))

(check "resolve-staged-projects finds exactly what the manifest staged"
       (equal? (map rz-codegen-source (resolve-staged-projects manifest))
               '("projects/default/default.scm")))

;;! Shipped is the larger set, and the distinction is the one that decides
;;! whether a project is reachable from the page at all. A project-source
;;! declares no generated/ output, so any number may be declared — checked here
;;! rather than trusted, because "the staged one is the only one served" is
;;! exactly the bug this pair was introduced to fix.
(check "a project-source declares no generated output, so many may be shipped"
       (and (null? (rz-codegen-outputs
                    (rz-form->codegen "d" '(project-source "a.scm"))))
            (resolve-check-codegen
             (list (cons "a" '((project-source "a.scm")))
                   (cons "b" '((project-source "b.scm")))
                   (cons "c" '((staged-project "c.scm")))))))

(check "a project-source takes no arguments beyond its source"
       (expect-error
        (lambda ()
          (resolve-check-codegen
           (list (cons "d" '((project-source "a.scm" "A_SCM"))))))))

(check "resolve-shipped-projects finds every project, staged or not"
       (equal? (map rz-codegen-source (resolve-shipped-projects manifest))
               '("projects/default/default.scm"
                 "projects/chess/chess.scm")))

(check "an embed's C symbol rides along as an argument, not an output"
       (equal? (rz-codegen-args (decl-for "core/runtime.scm"))
               '("runtime_scm.h" "RUNTIME_SCM")))

;;! --- the three source roots ----------------------------------------------
;;!
;;! A project's directory resolves against the repository root, an engine
;;! module's against the engine root. The prefix is the whole of that rule, so
;;! what is checked here is that the prefix is read off the RESOLVED PATH and
;;! not just the manifest entry — that is what lets a project's own sources take
;;! the project root while the `(root …)` engine sources it compiles in keep
;;! taking the engine's, and it is the case that would break silently by
;;! emitting `$reporoot/world/entity/entity.c`.

(check "a project directory is a project path"
       (and (rz-project-path? "projects/chess")
            (rz-project-path? "projects/chess/chess.scm")))

(check "an engine module is not, wherever it is named from"
       (and (not (rz-project-path? "game/project"))
            (not (rz-project-path? "world/entity/entity.c"))
            ;;! The one that matters: `(root "world/entity/entity.c")` written
            ;;! inside projects/ducks/build.scm still resolves engine-relative.
            (not (rz-project-path? (rz-path "projects/ducks"
                                            '(root "world/entity/entity.c"))))))

(check "a project's own source resolves against its project directory"
       (rz-project-path? (rz-path "projects/ducks" "ducks_test.c")))

(check "rz-spec-path sends each directory to its own root"
       (and (string=? (rz-spec-path "/r" "projects/chess")
                      "/r/projects/chess/build.scm")
            (string=? (rz-spec-path "/r" "game/project")
                      "/r/krudd/engine/game/project/build.scm")))

(check "the emitter prefixes each path with its own root variable"
       (and (string=? (ninja-ref "projects/ducks/ducks_test.c")
                      "$reporoot/projects/ducks/ducks_test.c")
            (string=? (ninja-ref "world/entity/entity.c")
                      "$srcroot/world/entity/entity.c")
            ;;! A `${generated}` path belongs to neither root.
            (string=? (ninja-ref "${generated}") "generated")))

;;! The third root, and the half of it that matters most here: it is INERT. This
;;! repository never sets KRUDD_SDK_PREFIX, so the suite runs — and the tree
;;! builds — with the two roots it has always had, which is what makes the SDK
;;! prefix something a consumer opts into rather than something this build has
;;! to opt out of (#1035).
(check "with no SDK prefix set the engine root is the sibling engine tree"
       (and (not rz-sdk-prefix)
            (string=? (rz-engine-root "/r") "/r/krudd/engine")))

;;! Set, it displaces the engine root and nothing else. A consuming repository
;;! has no `krudd/engine/` for an engine-relative path to land in, so an engine
;;! module's spec comes out of the unpacked prefix — while a project's still
;;! comes out of the checkout, because the SDK answers where the ENGINE is and
;;! says nothing about where a project is. `set!` rather than the environment
;;! because s7 has no setenv: the variable is read once at load, which is the
;;! same property the generator relies on.
(define (with-sdk-prefix prefix thunk)
  (let ((saved rz-sdk-prefix))
    (set! rz-sdk-prefix prefix)
    (let ((result (thunk)))
      (set! rz-sdk-prefix saved)
      result)))

(check "an SDK prefix moves the engine root out of the checkout entirely"
       (with-sdk-prefix "/opt/krudd-sdk"
                        (lambda ()
                          (string=? (rz-engine-root "/r") "/opt/krudd-sdk"))))

(check "under an SDK prefix an engine spec comes from the prefix"
       (with-sdk-prefix "/opt/krudd-sdk"
                        (lambda ()
                          (string=? (rz-spec-path "/r" "game/project")
                                    "/opt/krudd-sdk/game/project/build.scm"))))

;;! The one that would break silently: a consuming repository is projects and
;;! nothing else, so if the SDK prefix reached them too its whole manifest would
;;! resolve into the unpacked engine and read no spec it actually owns.
(check "under an SDK prefix a project spec still comes from the repository"
       (with-sdk-prefix "/opt/krudd-sdk"
                        (lambda ()
                          (string=? (rz-spec-path "/r" "projects/chess")
                                    "/r/projects/chess/build.scm"))))

(check "the project predicate is no business of the SDK prefix"
       (with-sdk-prefix "/opt/krudd-sdk"
                        (lambda ()
                          (and (rz-project-path? "projects/chess/chess.scm")
                               (not (rz-project-path? "world/entity/entity.c"))
                               (string=? (ninja-root-var "world/entity/entity.c")
                                         "$srcroot/")))))

(check "with-sdk-prefix leaves the suite's own root where it found it"
       (and (not rz-sdk-prefix)
            (string=? (rz-spec-path "/r" "game/project")
                      "/r/krudd/engine/game/project/build.scm")))

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
       (not (contains? (ninja-synthesize manifest (rz-engine-root krudd-root))
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

;;! The half of projects/README.md that is checked rather than trusted: a
;;! project declares no library. Asserted red below, because the real manifest
;;! passing proves nothing on its own — nothing in projects/ has ever declared
;;! one, which is exactly the silence this check exists to replace.
(display "projects: the contract, the enforced half\n")

(check "resolve-check-projects passes over the real manifest"
       (not (expect-error (lambda () (resolve-check-projects manifest)))))

(define project-library-manifest
  (list (cons "projects/chess" '((library "chess_rules" (sources "rules.c"))))))

(check "a library declared in projects/ errors"
       (expect-error
        (lambda () (resolve-check-projects project-library-manifest))))

;;! An interface-library declares no sources and emits no build edge, so it
;;! reads as harmless — but it is still a name an engine module could link, and
;;! include directories it would inherit, which is the whole of what the rule
;;! about reaching into a project is.
(define project-interface-manifest
  (list (cons "projects/ducks"
              '((interface-library "ducks_api" (interface "include"))))))

(check "an interface-library in projects/ errors for the same reason"
       (expect-error
        (lambda () (resolve-check-projects project-interface-manifest))))

(check "the failure names the project, the declaration and the contract"
       (let ((text (error-text
                    (lambda ()
                      (resolve-check-projects project-library-manifest)))))
         (and (contains? text "projects/chess")
              (contains? text "(library \"chess_rules\")")
              (contains? text "projects/README.md"))))

;;! The two things the rule must NOT catch. Every project declares an executable
;;! for its test — that is why all three are in the manifest — and an engine
;;! module declaring a library is the ordinary case the rest of the build is.
(check "a project's test executable is not a library"
       (not (expect-error
             (lambda ()
               (resolve-check-projects
                (list (cons "projects/chess"
                            '((executable "chess_test"
                                          (sources "chess_test.c"))))))))))

(check "a library outside projects/ is nothing to do with this rule"
       (not (expect-error
             (lambda ()
               (resolve-check-projects
                (list (cons "game/host"
                            '((library "game" (sources "game.c"))))))))))

;;! The rule beside it, which this one is what holds up: with no library to
;;! name, an engine module has nothing to link, so the tier check has no edge to
;;! see. Stated as a test so the reasoning in resolve-check-projects is a fact
;;! about the tree rather than an argument about it.
(check "no project declares a library, so no tier edge can point at one"
       (null? (rz-project-libraries manifest)))

(display "emitter: rendered build.ninja\n")

(define (dirname path)
  (let loop ((i (- (string-length path) 1)))
    (cond ((< i 0) ".")
          ((char=? (string-ref path i) #\/) (substring path 0 i))
          (else (loop (- i 1))))))

;;! The single line of TEXT that starts with PREFIX, or "" if none does — used
;;! below to check what one ninja phony edge lists without also matching
;;! PREFIX's target name where it appears as some other edge's dependency.
(define (line-starting-with text prefix)
  (let* ((tlen (string-length text)) (plen (string-length prefix)))
    (let scan ((start 0))
      (cond ((> (+ start plen) tlen) "")
            ((string=? (substring text start (+ start plen)) prefix)
             (let find-end ((i start))
               (cond ((>= i tlen) (substring text start i))
                     ((char=? (string-ref text i) #\newline)
                      (substring text start i))
                     (else (find-end (+ i 1))))))
            (else (scan (+ start 1)))))))

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

;;! The engine root comes from rz-engine-root here for the same reason
;;! kruddmake/build.scm's `src-root` does: this is the second entry point into
;;! the generator, and an entry point that spelled the root out for itself is
;;! one that can come to disagree with the other about which engine it is
;;! building (#1035).
(define ninja-text
  (if (and ninja-out (> (string-length ninja-out) 0))
      (ninja-synthesize manifest
                        (rz-engine-root krudd-root)
                        (dirname ninja-out)
                        regen-cmd)
      (ninja-synthesize manifest (rz-engine-root krudd-root))))

(check "header present"
       (contains? ninja-text "Generated by krudd"))
(check "both source roots are declared in the preamble"
       (and (contains? ninja-text "reporoot = ")
            (contains? ninja-text "srcroot = ")))
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
;;! EVERY shipped project is copied into assets/ under its own name and named on
;;! the wasm target so a site build actually stages it — not only the staged one.
;;! The unstaged three are the point of the check: they are what a page offers
;;! beyond the project it booted into, and before (project-source ...) existed
;;! they built, tested green and were unreachable. The staged one is copied too,
;;! being embedded and shipped both, which is why it is checked alongside them.
(check "every shipped project is copied into assets/ under its own name"
       (and (contains? ninja-text
                       (string-append "build assets/default.scm: copy "
                                      "$reporoot/projects/default/default.scm"))
            (contains? ninja-text
                       (string-append "build assets/chess.scm: copy "
                                      "$reporoot/projects/chess/chess.scm"))
            (contains? ninja-text " assets/default.scm")
            (contains? ninja-text " assets/chess.scm")))
;;! The property #1035 asks for: `archives` is the libraries, and it is never
;;! the test binaries or their run_test stamps that also ride in `native`.
(check "archives target carries the native libraries, and only them"
       (let ((line (line-starting-with ninja-text "build archives: phony ")))
         (and (contains? line "liblog.a")
              (not (contains? line "bin/"))
              (not (contains? line "test/")))))
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
      ;;! Each is named through ninja-source-path rather than by pasting a root
      ;;! on, so a project's source is looked for where a project's source
      ;;! actually is — a check that spelled one root out would pass by never
      ;;! having asked about the other.
      (check "regen edge lists every declared codegen input"
             (let loop ((l codegen))
               (cond ((null? l) #t)
                     ((contains? ninja-text
                                 (string-append
                                  " "
                                  (ninja-source-path
                                   (rz-engine-root krudd-root)
                                   (rz-codegen-source (car l)))
                                  " "))
                      (loop (cdr l)))
                     (else #f))))))

;;! The index the page reads to learn what this build shipped — the shell cannot
;;! list a directory over HTTP and must not be told a filename, so the build
;;! writes down what it copied. Generated beside the copies during the
;;! synthesize above, so it is already on disk here.
;;!
;;! All four, in manifest order — the staged one first, since it is the one a
;;! bare URL opens and the picker lists it like any other. This is also the
;;! assertion that a project which is shipped but not staged still reaches the
;;! Load Project control, which is the whole difference between the two
;;! declarations.
(if (and ninja-out (> (string-length ninja-out) 0))
    (check "assets/projects.json names every shipped project"
           (string=? (krudd-slurp (string-append (dirname ninja-out)
                                                 "/assets/projects.json"))
                     (string-append "[\"default.scm\",\"chess.scm\"]\n"))))

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
