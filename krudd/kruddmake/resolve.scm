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
;;!   (staged-project IN)                    the project this image ships
;;!   (served-project IN)                    a project served beside it
;;!
;;! IN resolves against the declaring module like a source path; every output is
;;! named relative to `generated/`. The number paired with each kind below is how
;;! many arguments follow IN, which is what makes a typo'd declaration an arity
;;! error rather than a silently different one.
;;!
;;! `staged-project` is the one that takes none, and that is the point of it. It
;;! names a `.scm` twice over — embedded into the image under the fixed symbol
;;! below (what core/engine.c evaluates at boot, so the page opens on a playable
;;! scene with no network round trip) and copied into `assets/` beside
;;! index.html (what the shell's Load Project control offers, so the staged
;;! project is reachable by the same door a file off the user's disk comes in
;;! by). Two destinations, one declaration, one file: the served copy cannot
;;! drift from the embedded one because there is only ever one source. The
;;! header name being fixed rather than per-declaration is what makes "exactly
;;! one directory may claim the staged slot" a build error — a second
;;! declaration writes the same generated/ file, which resolve-check-codegen
;;! already refuses.
;;!
;;! `served-project` is the staged declaration with the embed taken away: the
;;! source is copied into `assets/` and named in the index, and that is all. It
;;! is what a second, third, tenth project uses, since only one may hold the
;;! staged slot and only one CAN — the slot is "the project this image boots
;;! into", of which there is exactly one. A served project is offered by the
;;! shell's Load Project control alongside the staged one, opened by the same
;;! door, and costs the image nothing: nothing is compiled in, so a build may
;;! ship as many as it likes without growing the wasm by a byte. It writes no
;;! generated/ file at all, which is why it is the one kind with no outputs —
;;! and why the collision it CAN have (two projects landing on one assets/ name)
;;! is checked for by name below rather than falling out of the duplicate-output
;;! rule the way two staged projects do.
(define rz-codegen-kinds
  '((configure-file . 1)
    (embed . 2)
    (embed-scheme-module . 2)
    (emit-math-module . 1)
    (emit-interface-header . 1)
    (staged-project . 0)
    (served-project . 0)))

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
;;! carries no arguments at all and writes the one fixed header above, and
;;! `served-project` writes nothing — it is a copy into assets/ and an entry in
;;! the index, neither of which is generated C.
(define (rz-codegen-outputs decl)
  (case (rz-codegen-kind decl)
    ((embed) (list (car (rz-codegen-args decl))))
    ((staged-project) (list rz-staged-project-header))
    ((served-project) '())
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

;;! The (staged-project ...) declarations, in manifest order. Read off the same
;;! list every other consumer reads, so the source copied into `assets/` is by
;;! construction the source embedded into the image — the whole reason the two
;;! are one declaration.
(define (resolve-staged-projects manifest)
  (rz-filter (lambda (decl) (eq? (rz-codegen-kind decl) 'staged-project))
             (resolve-codegen manifest)))

;;! Every project this build puts in `assets/`, staged and served alike, in
;;! manifest order. This is the list the copy edges and the project index are
;;! both rendered from, so a project reaches the page by being declared and by
;;! nothing else — and the two cannot disagree about what shipped, since one
;;! list answers both.
;;!
;;! Staged first is not cosmetic: the index is what the shell offers, in order,
;;! and the project the image already booted into belongs at the head of that
;;! list rather than wherever the manifest's tier order happened to put its
;;! directory.
(define (resolve-shipped-projects manifest)
  (append (resolve-staged-projects manifest)
          (rz-filter (lambda (decl)
                       (eq? (rz-codegen-kind decl) 'served-project))
                     (resolve-codegen manifest))))

;;! The name a shipped project lands under in `assets/` — its source's basename,
;;! since that is what the copy edge writes and what the index tells the shell to
;;! fetch.
(define (rz-shipped-project-name decl)
  (let* ((src (rz-codegen-source decl))
         (n   (string-length src)))
    (let loop ((i (- n 1)))
      (cond ((< i 0) src)
            ((char=? (string-ref src i) #\/) (substring src (+ i 1)))
            (else (loop (- i 1)))))))

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

;;! (rz-check-shipped-project-names manifest) — no two shipped projects may land
;;! on the same name in `assets/`. The copy edges are keyed by basename, so two
;;! projects called `game.scm` in different directories would be one ninja output
;;! built two ways and one entry in the index naming whichever won — a game
;;! silently serving another game's source. Staged projects cannot collide with
;;! each other (there is only ever one), which is why this check arrives with
;;! `served-project` rather than having been here all along.
(define (rz-check-shipped-project-names manifest)
  (let loop ((l (map rz-shipped-project-name
                     (resolve-shipped-projects manifest)))
             (seen '()))
    (cond ((null? l) #t)
          ((member (car l) seen)
           (error 'rz-shipped-project-duplicate-name (car l)))
          (else (loop (cdr l) (cons (car l) seen))))))

;;! Three ways a declaration can be wrong that the build would otherwise absorb
;;! in silence, so all three are hard errors:
;;!   - two declarations writing the same generated/ file: whichever runs last
;;!     wins and the other's consumer compiles against a header it did not ask
;;!     for;
;;!   - a `(sources (raw "${generated}/x"))` no declaration produces: a renamed
;;!     or deleted declaration whose consumer still expects the old output;
;;!   - two shipped projects claiming one name in `assets/` (see above).
(define (resolve-check-codegen manifest)
  (let ((outs (apply append (map rz-codegen-outputs (resolve-codegen manifest)))))
    (let loop ((l outs) (seen '()))
      (cond ((null? l) #t)
            ((member (car l) seen)
             (error 'rz-codegen-duplicate-output (car l)))
            (else (loop (cdr l) (cons (car l) seen)))))
    (rz-check-shipped-project-names manifest)
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
