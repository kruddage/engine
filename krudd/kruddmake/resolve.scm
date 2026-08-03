; SPDX-License-Identifier: GPL-2.0-or-later

(define (rz-filter pred lst)
  (cond ((null? lst) '())
        ((pred (car lst)) (cons (car lst) (rz-filter pred (cdr lst))))
        (else (rz-filter pred (cdr lst)))))

(define (rz-dedup lst)
  (let loop ((l lst) (seen '()) (out '()))
    (cond ((null? l) (reverse out))
          ((member (car l) seen) (loop (cdr l) seen out))
          (else (loop (cdr l) (cons (car l) seen)
                      (cons (car l) out))))))

(define (rz-prefix? s pfx)
  (let ((ls (string-length s)) (lp (string-length pfx)))
    (and (>= ls lp) (string=? (substring s 0 lp) pfx))))

(define (rz-clause head clauses)
  (cond ((null? clauses) #f)
        ((and (pair? (car clauses)) (eq? (caar clauses) head))
         (car clauses))
        (else (rz-clause head (cdr clauses)))))

;;! TWO SOURCE ROOTS, not one. An engine module's directory is relative to
;;! `krudd/engine`; a project's is relative to the repository root, because a
;;! project is not an engine module. #976 made a project a single `.scm` the
;;! engine loads at runtime, and the last thing still saying otherwise was where
;;! chess sat on disk — inside the engine tree, in the manifest between `ui/`
;;! and `shell/`, as though it were a tier. `projects/` at the top level is that
;;! sentence written into the layout, where it is legible from `ls` rather than
;;! only from this file.
;;!
;;! One prefix distinguishes the two, and it is tested against the RESOLVED PATH
;;! rather than only against the manifest directory. That is what lets a
;;! project's own sources take the project root while the `(root …)` paths it
;;! reaches back into the engine for keep taking the engine's — a project links
;;! engine libraries and compiles a few engine sources into its test, and those
;;! are engine-relative exactly as they are from anywhere else. Everything
;;! downstream — include flags, object paths, codegen inputs, the regen edge —
;;! derives from this one predicate rather than carrying its own idea of where a
;;! file lives.
(define rz-project-prefix "projects/")

(define (rz-project-path? p) (rz-prefix? p rz-project-prefix))

;;! The on-disk `build.scm` a manifest entry names. Both entry points into the
;;! generator — `kruddmake/build.scm` and `resolve_test.scm` — read their specs
;;! through here, so the two cannot come to disagree about where a spec lives
;;! the way they would if each spelled the root out for itself.
(define (rz-spec-path krudd-root dir)
  (string-append krudd-root
                 (if (rz-project-path? dir) "" "/krudd/engine")
                 "/" dir "/build.scm"))

(define rz-system-libs (list "m"))

(define (rz-system-lib? name) (member name rz-system-libs))

(define (rz-join dir sub)
  (cond ((or (string=? sub "") (string=? sub ".")) dir)
        ((string=? dir "") sub)
        (else (string-append dir "/" sub))))

(define (rz-path dir p)
  (cond ((string? p) (rz-join dir p))
        ((and (pair? p) (eq? (car p) 'root)) (cadr p))
        ((and (pair? p) (eq? (car p) 'current))
         (if (null? (cdr p)) dir (rz-join dir (cadr p))))
        ((and (pair? p) (eq? (car p) 'raw)) (cadr p))
        (else (error 'rz-bad-path p))))

(define (rz-paths dir specs) (map (lambda (p) (rz-path dir p)) specs))

(define (rz-clause-dirs dir clauses head)
  (let ((c (rz-clause head clauses)))
    (if c (rz-paths dir (cdr c)) '())))

(define (rz-make-target name dir kind public private links wasm-modules)
  (list name (cons 'dir dir) (cons 'kind kind)
        (cons 'public public) (cons 'private private)
        (cons 'links links) (cons 'wasm-modules wasm-modules)))

(define (rz-field target key) (cdr (assq key (cdr target))))

(define (rz-form->target dir form)
  (case (car form)
    ((library executable)
     (let ((clauses (cddr form)))
       (rz-make-target
        (cadr form) dir (car form)
        (rz-clause-dirs dir clauses 'public)
        (rz-clause-dirs dir clauses 'private)
        (let ((c (rz-clause 'link clauses))) (if c (cdr c) '()))
        (let ((c (rz-clause 'wasm-modules clauses)))
          (if c (cdr c) '())))))
    ((interface-library)
     (rz-make-target
      (cadr form) dir 'interface-library
      (rz-clause-dirs dir (cddr form) 'interface)
      '() '() '()))
    (else #f)))

(define (rz-spec-targets dir spec)
  (let loop ((forms spec) (out '()))
    (cond ((null? forms) (reverse out))
          ((memq (caar forms) '(native-only wasm-only))
           (loop (cdr forms)
                 (append (reverse (rz-spec-targets dir (cdar forms)))
                         out)))
          (else
           (let ((t (rz-form->target dir (car forms))))
             (loop (cdr forms) (if t (cons t out) out)))))))

(define (rz-target-table manifest)
  (apply append
         (map (lambda (pair) (rz-spec-targets (car pair) (cdr pair)))
              manifest)))

;;! Code generation is declared per-module, in the same build.scm as the rest of
;;! that module's build facts, and both consumers derive from that one
;;! declaration: ninja-generate-codegen runs the generators, and
;;! ninja-generator-inputs lists the same sources on the `regen` edge so editing
;;! one re-runs them. Neither carries a literal list, so the two cannot drift
;;! apart the way the hand-maintained pair did (#779, #787).
;;!
;;! The kinds map one-to-one onto the introspect.scm entry points and stay
;;! distinct clauses rather than one clause with a mode argument, because they
;;! take genuinely different arguments — an `(embed)` names a C symbol, an
;;! `(embed-scheme-module)` writes two files, the rest write one:
;;!
;;!   (configure-file IN OUT)                @VAR@ substitution
;;!   (embed IN OUT SYMBOL)                  the bytes as a C array
;;!   (embed-scheme-module IN HEADER SHIM)   ABI header + s7 image shim
;;!   (emit-math-module IN OUT)              (define-c-fn) bodies lowered to C
;;!   (emit-interface-header IN OUT)         the backend interface header
;;!   (project-source IN)                    a project this build ships
;;!   (staged-project IN)                    the project it also boots into
;;!
;;! IN resolves against the declaring module like a source path; every output is
;;! named relative to `generated/`. The number paired with each kind below is how
;;! many arguments follow IN, which is what makes a typo'd declaration an arity
;;! error rather than a silently different one.
;;!
;;! --- shipping a project, versus booting into one ---------------------------
;;!
;;! The last two are the pair, and the distinction between them is the whole of
;;! what a project's build.scm has to decide.
;;!
;;! `project-source` SHIPS a project: the `.scm` is copied into `assets/` beside
;;! index.html and named in `assets/projects.json`, which is the list the shell's
;;! Load Project control offers. It generates nothing — it is a copy edge and an
;;! index entry, which is why its output list is empty. Any number of directories
;;! may declare one, and every project that wants to be reachable from the page
;;! MUST, because the page cannot list a directory over HTTP and must not carry a
;;! filename: what the build wrote down is the only way it learns what exists.
;;!
;;! `staged-project` is that plus one thing: the source is also embedded into the
;;! image under the fixed symbol below, which core/engine.c evaluates at boot so
;;! the page opens on a playable scene with no network round trip. It implies
;;! project-source — a project the image boots into is self-evidently one the
;;! build ships — so a directory declares one or the other, never both.
;;!
;;! Exactly one directory may be the staged one. Nothing limits how many are
;;! shipped, and the asymmetry is a decision rather than a leftover — see the
;;! note on resolve-check-staged below, which is where it is argued and where it
;;! is enforced.
;;!
;;! Both are named for what the source IS to the build rather than for whichever
;;! game claimed the slot, since the C that evaluates it must not learn which
;;! project it got (#976). And in both cases the served copy cannot drift from
;;! whatever else is made of the file, because there is only ever one source.
(define rz-codegen-kinds
  '((configure-file . 1)
    (embed . 2)
    (embed-scheme-module . 2)
    (emit-math-module . 1)
    (emit-interface-header . 1)
    (project-source . 0)
    (staged-project . 0)))

;;! The generated header and C symbol a (staged-project ...) embeds under. Named
;;! for what the source IS to the build — the project this image ships staged —
;;! rather than for whichever game claimed the slot, since the C that evaluates
;;! it must not learn which project it got (#976).
(define rz-staged-project-header "staged_project_scm.h")
(define rz-staged-project-symbol "STAGED_PROJECT_SCM")

(define (rz-codegen-arity kind)
  (let ((c (assq kind rz-codegen-kinds))) (and c (cdr c))))

(define (rz-codegen-form? form)
  (and (pair? form) (symbol? (car form))
       (rz-codegen-arity (car form)) #t))

;;! A declaration is (kind dir input . args) — `dir` is kept so the input path
;;! resolves against the declaring module the same way a source does.
(define (rz-form->codegen dir form)
  (let ((want (+ 1 (rz-codegen-arity (car form)))))
    (if (not (= (length (cdr form)) want))
        (error 'rz-codegen-arity (list dir form 'expects want 'arguments)))
    (cons (car form) (cons dir (cdr form)))))

(define (rz-codegen-kind decl) (car decl))
(define (rz-codegen-source decl) (rz-path (cadr decl) (caddr decl)))
(define (rz-codegen-args decl) (cdddr decl))

;;! The generated/ files a declaration writes. For `embed` the second argument
;;! is a C symbol rather than a file, so it is not an output; `staged-project`
;;! carries no arguments at all and writes the one fixed header above; and
;;! `project-source` writes nothing — it is a copy edge and an index entry, so
;;! it has no generated/ output to collide with anything, which is exactly why
;;! any number of directories may declare one.
(define (rz-codegen-outputs decl)
  (case (rz-codegen-kind decl)
    ((embed) (list (car (rz-codegen-args decl))))
    ((staged-project) (list rz-staged-project-header))
    ((project-source) '())
    (else (rz-codegen-args decl))))

(define (rz-spec-codegen dir spec)
  (let loop ((forms spec) (out '()))
    (cond ((null? forms) (reverse out))
          ((memq (caar forms) '(native-only wasm-only))
           (loop (cdr forms)
                 (append (reverse (rz-spec-codegen dir (cdar forms)))
                         out)))
          ((rz-codegen-form? (car forms))
           (loop (cdr forms)
                 (cons (rz-form->codegen dir (car forms)) out)))
          (else (loop (cdr forms) out)))))

(define (resolve-codegen manifest)
  (apply append
         (map (lambda (pair) (rz-spec-codegen (car pair) (cdr pair)))
              manifest)))

;;! Every project this build SHIPS, in manifest order — the sources copied into
;;! `assets/` and named in `assets/projects.json`. Both kinds count: a staged
;;! project is a shipped one that is additionally embedded, so a page that
;;! offered only the staged one would be hiding the projects the build had
;;! already put beside it. Read off the same declaration list every other
;;! consumer reads, so the copies and the index cannot disagree about what
;;! shipped.
(define (resolve-shipped-projects manifest)
  (rz-filter (lambda (decl)
               (memq (rz-codegen-kind decl) '(project-source staged-project)))
             (resolve-codegen manifest)))

;;! The (staged-project ...) declaration — the one project the image also boots
;;! into. A subset of the shipped ones, and the only kind that embeds.
(define (resolve-staged-projects manifest)
  (rz-filter (lambda (decl) (eq? (rz-codegen-kind decl) 'staged-project))
             (resolve-codegen manifest)))

(define (rz-lookup table name) (assoc name table))

(define (rz-direct-deps table name)
  (let ((target (rz-lookup table name)))
    (if (not target)
        '()
        (rz-filter
         (lambda (dep) dep)
         (map (lambda (link)
                (cond ((rz-lookup table link) link)
                      ((rz-system-lib? link) #f)
                      (else (error 'rz-unknown-link-target
                                   (list 'in name 'links link)))))
              (rz-field target 'links))))))

(define (rz-closure table roots)
  (let ((state '())
        (out '()))
    (define (mark name tag) (set! state (cons (cons name tag) state)))
    (define (status name)
      (let ((s (assoc name state))) (and s (cdr s))))
    (define (visit name path)
      (case (status name)
        ((done) #t)
        ((active) (error 'rz-link-cycle (reverse (cons name path))))
        (else
         (mark name 'active)
         (for-each (lambda (dep) (visit dep (cons name path)))
                   (rz-direct-deps table name))
         (mark name 'done)
         (set! out (cons name out)))))
    (for-each (lambda (r) (visit r '())) roots)
    out))

(define (resolve-link-libs table name)
  (rz-closure table (rz-direct-deps table name)))

(define (resolve-wasm-module-libs table name)
  (let ((target (rz-lookup table name)))
    (rz-closure table
                (append (if target (rz-field target 'wasm-modules) '())
                        (rz-direct-deps table name)))))

(define (rz-target-syslibs table name)
  (let ((target (rz-lookup table name)))
    (if target
        (rz-filter rz-system-lib? (rz-field target 'links))
        '())))

(define (resolve-syslibs table name)
  (rz-dedup
   (apply append
          (map (lambda (n) (rz-target-syslibs table n))
               (cons name (resolve-link-libs table name))))))

(define (resolve-includes table name)
  (let ((target (rz-lookup table name)))
    (if (not target)
        (error 'rz-unknown-target name)
        (rz-dedup
         (append
          (rz-field target 'public)
          (rz-field target 'private)
          (apply append
                 (map (lambda (lib)
                        (rz-field (rz-lookup table lib) 'public))
                      (resolve-link-libs table name))))))))

(define (resolve-check-all table)
  (for-each (lambda (target) (resolve-includes table (car target)))
            table)
  #t)

(define rz-target-forms
  '(library interface-library executable test))

;;! Every top-level form a build.scm may carry. The generator's emitter ignores
;;! what it does not recognise (a `(test)` renders no ninja edge, for instance),
;;! which is exactly what would let a mistyped `(embeds …)` declare nothing and
;;! be absorbed in silence — the same failure mode #779 was. So the head of
;;! every form is checked here, and an unknown one is a hard error.
(define (rz-check-forms dir spec)
  (for-each
   (lambda (f)
     (cond ((not (pair? f)) (error 'rz-bad-form (list dir f)))
           ((memq (car f) '(native-only wasm-only))
            (rz-check-forms dir (cdr f)))
           ((memq (car f) rz-target-forms) #t)
           ((rz-codegen-form? f) (rz-form->codegen dir f))
           (else (error 'rz-unknown-form (list dir (car f))))))
   spec))

(define rz-generated-prefix "${generated}/")

(define (rz-spec-source-paths dir spec)
  (let loop ((forms spec) (out '()))
    (cond ((null? forms) out)
          ((memq (caar forms) '(native-only wasm-only))
           (loop (cdr forms)
                 (append (rz-spec-source-paths dir (cdar forms)) out)))
          ((memq (caar forms) '(library executable))
           (let ((c (rz-clause 'sources (cddar forms))))
             (loop (cdr forms)
                   (if c (append (rz-paths dir (cdr c)) out) out))))
          (else (loop (cdr forms) out)))))

;;! Two ways a declaration can be wrong that the build would otherwise absorb in
;;! silence, so both are hard errors:
;;!   - two declarations writing the same generated/ file: whichever runs last
;;!     wins and the other's consumer compiles against a header it did not ask
;;!     for;
;;!   - a `(sources (raw "${generated}/x"))` no declaration produces: a renamed
;;!     or deleted declaration whose consumer still expects the old output.
(define (resolve-check-codegen manifest)
  (let ((outs (apply append (map rz-codegen-outputs (resolve-codegen manifest)))))
    (let loop ((l outs) (seen '()))
      (cond ((null? l) #t)
            ((member (car l) seen)
             (error 'rz-codegen-duplicate-output (car l)))
            (else (loop (cdr l) (cons (car l) seen)))))
    (for-each
     (lambda (pair)
       (rz-check-forms (car pair) (cdr pair))
       (for-each
        (lambda (p)
          (if (and (rz-prefix? p rz-generated-prefix)
                   (not (member (substring p (string-length rz-generated-prefix))
                                outs)))
              (error 'rz-codegen-undeclared-source (list (car pair) p))))
        (rz-spec-source-paths (car pair) (cdr pair))))
     manifest)
    #t))

;;! The tier rule manifest.scm opens with: the directories are listed in
;;! dependency order, and a module may only reach for one listed above it. Until
;;! now nothing read that back, so the one library link that inverted it —
;;! core/script -> base/log — landed in a green build and stayed there, with no
;;! way to tell from inside the tree whether the list was wrong or the link was
;;! (#923).
;;!
;;! The check lives here rather than in tools/barriers/check-barriers.mjs because
;;! kruddmake already holds both halves of it: manifest.scm's order is the list
;;! this very generator is driven by, and rz-target-table already records the
;;! module each library was declared in. Reading them from JS would mean a
;;! second Scheme reader for manifest.scm and 22 build.scm files, and would put
;;! node in the path of a rule about C. It runs from ninja-synthesize, so it
;;! fails at generation — before a compile, on every `krudd build` and every
;;! run-tests.sh run — and there is still exactly one tier list in the repo.
;;!
;;! Only library links are walked. Nothing links an executable: it is where the
;;! program is assembled, so `index` linking every backend is the main-module
;;! link rather than a tier reaching downward. A library linking downward is the
;;! edge that makes the order a fiction, and it is the only one flagged. Links
;;! within one module are a module's own business, and a system lib ("m") has no
;;! module at all.
(define (rz-position dirs dir)
  (let loop ((l dirs) (i 0))
    (cond ((null? l) -1)
          ((string=? (car l) dir) i)
          (else (loop (cdr l) (+ i 1))))))

(define (rz-library-target? target)
  (and (memq (rz-field target 'kind) '(library interface-library)) #t))

;;! One inversion, as (name dir dep-name dep-dir position dep-position).
(define (rz-target-inversions dirs table target)
  (if (not (rz-library-target? target))
      '()
      (let ((dir (rz-field target 'dir)))
        (rz-filter
         (lambda (x) x)
         (map (lambda (link)
                (let ((dep (rz-lookup table link)))
                  (and dep
                       (let* ((ddir (rz-field dep 'dir))
                              (at (rz-position dirs dir))
                              (dep-at (rz-position dirs ddir)))
                         (and (not (string=? ddir dir))
                              (>= dep-at at)
                              (list (car target) dir link ddir at dep-at))))))
              (rz-field target 'links))))))

(define (rz-tier-inversions manifest)
  (let ((dirs (map car manifest))
        (table (rz-target-table manifest)))
    (apply append
           (map (lambda (target) (rz-target-inversions dirs table target))
                table))))

(define (rz-inversion-message inv)
  (string-append
   "  " (list-ref inv 1) "/" (list-ref inv 0) " links " (list-ref inv 2)
   ", declared in " (list-ref inv 3) ".\n"
   "    manifest.scm lists " (list-ref inv 1) " at position "
   (number->string (list-ref inv 4)) " and " (list-ref inv 3) " at position "
   (number->string (list-ref inv 5))
   " — a module may only link one listed above it.\n"))

(define (resolve-check-tiers manifest)
  (let ((bad (rz-tier-inversions manifest)))
    (if (pair? bad)
        (error 'rz-tier-inversion
               (string-append
                "kruddmake/manifest.scm tier order violated by "
                (number->string (length bad))
                (if (= (length bad) 1) " library link:\n" " library links:\n")
                (apply string-append (map rz-inversion-message bad))))
        #t)))

;;! A project declares no library. projects/README.md is the contract this comes
;;! from, and this is the one rule in it worth enforcing rather than trusting.
;;!
;;! It is what holds the rule beside it — that no engine module links a project.
;;! resolve-check-tiers above would catch such a link, since projects/* is last
;;! in manifest.scm and a link into it inverts the order by definition. But it
;;! catches it for a reason that could evaporate: the check walks LINKS, and a
;;! link needs a name to reach for. No project declares a library, so there is
;;! no name, so there is no edge, so the tier check has nothing to say. The door
;;! is standing open rather than shut, and it is shut here — at the declaration,
;;! naming the project and the rule, rather than one manifest edit later at a
;;! link that is harder to read the intent of.
;;!
;;! Both library kinds count. An interface-library declares no sources and emits
;;! no build edge, so it looks harmless, but it is a name an engine module can
;;! link and a set of include directories it would then inherit — which is the
;;! whole of what rule 2 is about. Executables are untouched: every project has
;;! one for its test, nothing links an executable, and that is why all three
;;! projects are in the manifest at all.
(define (rz-project-libraries manifest)
  (rz-filter (lambda (target)
               (and (rz-library-target? target)
                    (rz-project-path? (rz-field target 'dir))))
             (rz-target-table manifest)))

(define (rz-project-library-message target)
  (string-append
   "  " (rz-field target 'dir) " declares ("
   (symbol->string (rz-field target 'kind)) " \"" (car target) "\").\n"))

(define (resolve-check-projects manifest)
  (let ((bad (rz-project-libraries manifest)))
    (if (pair? bad)
        (error 'rz-project-library
               (string-append
                "a project may not declare a library:\n"
                (apply string-append
                       (map rz-project-library-message bad))
                "    A project is a single .scm the engine loads at runtime, so"
                " it reaches for the\n"
                "    engine and nothing reaches for it. A library is a name an"
                " engine module could\n"
                "    link, which would put a project inside the tier order"
                " manifest.scm lists it\n"
                "    last to stay out of. Compile the sources into the"
                " project's own test instead.\n"
                "    See projects/README.md.\n"))
        #t)))

;;! --- the staged slot is single-occupancy, on purpose (#1019) ---------------
;;!
;;! Exactly one project may be the staged one, and this is the rule rather than
;;! a consequence of one. It used to be a consequence: rz-staged-project-header
;;! is a fixed name rather than a per-declaration one, so a second
;;! `staged-project` wrote a generated/ file that was already spoken for and
;;! resolve-check-codegen refused it as a duplicate output. That enforced the
;;! right thing for the wrong reason — it failed naming a header rather than the
;;! rule, and it would have stopped enforcing anything the moment someone gave
;;! the embed a per-project symbol, which is exactly the change a reader who
;;! thought the collision was an accident would reach for first.
;;!
;;! #1013 asked whether the rule itself should survive three projects. It should,
;;! and the argument is not the mechanism:
;;!
;;!   - A boot opens ONE project. Embedding is what buys "the page opens on a
;;!     playable scene with no network round trip" (#976 kept it deliberately),
;;!     and it buys it by putting the source in the WASM image every visitor
;;!     downloads. Embedding a second pays that cost again to serve a boot that
;;!     cannot use it.
;;!   - Since #1030 the page already offers every SHIPPED project and a pick is
;;!     a `?game=` away, so the staged slot does not decide which projects exist
;;!     or which are reachable — `project-source` does, and it is unlimited. It
;;!     decides only what the bare URL does, and a default is one thing by
;;!     definition.
;;!   - The alternatives that would need more than one embedded — boot into the
;;!     last opened, boot into a launcher over embedded sources — either put a
;;!     storage read in front of the first frame or make the bare URL answer
;;!     differently per visitor, and a shared link resolving to whatever that
;;!     visitor opened last is worse than the thing being solved.
;;!
;;! What would reopen it: a boot path that must reach a second project without a
;;! fetch. Nothing wants that today, and `?game=` costs a fetch by design.
;;!
;;! Exactly one, not at most one: core/engine.c includes the generated header
;;! unconditionally, so a build with none is a missing-header compile error a
;;! long way from the declaration that should have existed. Both directions are
;;! worth a sentence naming the rule.
(define (rz-staged-message decls)
  (apply string-append
         (map (lambda (d)
                (string-append "  " (rz-codegen-source d) "\n"))
              decls)))

(define (resolve-check-staged manifest)
  (let* ((staged (resolve-staged-projects manifest))
         (n (length staged)))
    (cond
     ((= n 1) #t)
     ((= n 0)
      (error 'rz-staged-project-missing
             (string-append
              "no directory declares (staged-project ...).\n"
              "    Exactly one project is embedded into the image and evaluated"
              " at boot, so that\n"
              "    the page opens on a playable scene with no network round"
              " trip. Without one,\n"
              "    core/engine.c has no " rz-staged-project-symbol " to"
              " evaluate.\n")))
     (else
      (error 'rz-staged-project-duplicate
             (string-append
              (number->string n)
              " directories declare (staged-project ...):\n"
              (rz-staged-message staged)
              "    Exactly one may. Being staged is not what makes a project"
              " reachable from the\n"
              "    page — (project-source ...) is, and any number may declare"
              " one. Staged is the\n"
              "    single default the bare URL boots into.\n"))))))
