; SPDX-License-Identifier: GPL-2.0-or-later

;;! project — the (project ...) form, and the image half of the host that runs
;;! it. A project is a game written as data: a display name for the launcher,
;;! the scene to build when it is chosen, the rules that make it playable, and
;;! the hooks the engine calls while it is the loaded one. The C half
;;! (game/project/project_host.c) owns the launcher entry, the active-project
;;! gate and the world binding; everything below is what a project's own source
;;! is read and run by. The reasoning behind the hook set is in
;;! include/project/project_host.h — this file is its implementation.
;;!
;;!   (project "Demo"
;;!     (rules
;;!      (define *demo-picks* 0)
;;!      (define (demo-reset . ignored) (set! *demo-picks* 0))
;;!      (define (demo-picked id) (set! *demo-picks* (+ *demo-picks* 1))))
;;!     (scene demo
;;!            (entity (name "board") (mesh "builtin://mesh/plane")))
;;!     (on-load demo-reset)
;;!     (on-selected demo-picked))
;;!
;;! Clauses, all optional and order-independent:
;;!
;;!   (rules FORM ...)  forms evaluated at registration time, at top level, in
;;!                     order — a project's procedures and its state. This is
;;!                     the load-the-rules-into-the-image step a game's C plugin
;;!                     ran from its subsystem init, moved inside the form. A
;;!                     project whose file would rather hold its rules as
;;!                     ordinary top-level definitions may do that instead and
;;!                     omit the clause: project-eval evaluates every form in a
;;!                     source, and (project ...) is one of them.
;;!   (scene NAME (entity ...) ...)
;;!                     the scene form built on load. Literally a (scene ...)
;;!                     form nested in the project — the same text a standalone
;;!                     scene asset carries, so a project's scene is authored
;;!                     the one way scenes are authored anywhere.
;;!   (on-load EXPR)    called after the scene is built: a fresh game.
;;!   (on-tick EXPR)    called every frame while this project is loaded.
;;!   (on-selected EXPR)
;;!                     called with an entity id each time the engine's
;;!                     selection CHANGES to a live entity — one call per click,
;;!                     not one per frame the click is held.
;;!
;;! A hook clause holds an EXPRESSION, evaluated once at registration (after the
;;! rules, so it may name them), not a parameter list and a body the way an
;;! entity script's (on-tick (self t) ...) clause does. The two DSLs differ
;;! because their surroundings do: an entity script is an asset with no file
;;! around it, so the body has nowhere else to live, while a project's hooks are
;;! procedures its own rules already define and a bare name is the whole
;;! wiring. An inline (lambda (id) ...) is still an expression, so the other
;;! shape is not lost; a parameter list could not have named an existing
;;! procedure without a forwarding lambda.
;;!
;;! An unknown clause is ignored, the way scene-build ignores one, so a project
;;! written against a newer engine still loads on an older one. Everything else
;;! that is wrong REFUSES: see project-register.

;;! Every registered project, an alist of (LAUNCHER-INDEX . record). The
;;! launcher index is the host's key for a project — it is what game_load hands
;;! back through game_active_index(), so it is what the C half names a project
;;! by when it dispatches into here.
(define *projects* '())

;;! The launcher index the last (project ...) form registered, or -1 if it was
;;! refused. project-eval reads it back out: a project form is evaluated for its
;;! effect, so this is how the C caller learns whether the effect happened.
(define *project-last-index* -1)

;;! How many (project ...) forms have been read since the image loaded. Only the
;;! difference across one project-eval is ever used, and only to tell "the
;;! source declared no project at all" — which nothing else reports — from "it
;;! declared one and it was refused", which project-register has already logged
;;! the reason for.
(define *project-forms-seen* 0)

;;! A project record. The last-selection slot is the one mutable field: it is
;;! the baseline the on-selected edge is measured against, re-armed on every
;;! load (see project-open) so a project reloaded after a spell away never
;;! starts out holding a selection from the last time it ran.
(define (project-record name scene on-load on-tick on-selected)
  (vector name scene on-load on-tick on-selected -1))

(define (project-name p) (vector-ref p 0))
(define (project-scene p) (vector-ref p 1))
(define (project-on-load p) (vector-ref p 2))
(define (project-on-tick p) (vector-ref p 3))
(define (project-on-selected p) (vector-ref p 4))
(define (project-last-sel p) (vector-ref p 5))
(define (project-set-last-sel! p v) (vector-set! p 5 v))

;;! (project-clause clauses head) -> the first clause whose head is HEAD, or #f.
;;! Written out rather than reached for with assq because CLAUSES is user input:
;;! assq errors on a list holding a non-pair, and a stray atom in a project form
;;! should cost that clause, not the whole registration.
(define (project-clause clauses head)
  (cond ((not (pair? clauses)) #f)
        ((and (pair? (car clauses)) (eq? (caar clauses) head)) (car clauses))
        (else (project-clause (cdr clauses) head))))

;;! (project-forget idx alist) -> ALIST without the entry for IDX. Re-evaluating
;;! a project's source is how a project reloads, and the launcher hands the
;;! reload the slot it already had (see game-register!), so the old record must
;;! go rather than shadowing the new one from further down the list.
(define (project-forget idx alist)
  (cond ((not (pair? alist)) '())
        ((eqv? (caar alist) idx) (project-forget idx (cdr alist)))
        (else (cons (car alist) (project-forget idx (cdr alist))))))

;;! (project-warn text) — one refusal, on the log, in the engine's voice. Every
;;! way a project form can be wrong ends up here: a bad project is user input,
;;! so it is reported and dropped, never faulted through.
(define (project-warn text)
  (krudd-log 2 (string-append "project: " text)))

;;! (project-run-rules clauses) -> #t when the (rules ...) clause evaluated
;;! cleanly (or there was none), #f when a form in it faulted. Evaluated in the
;;! rootlet, not in this procedure's own environment: a rules clause is a
;;! project's top-level definitions, and definitions made in a local environment
;;! would vanish the moment registration returned.
(define (project-run-rules clauses)
  (let ((c (project-clause clauses 'rules)))
    (if (not (pair? c))
        #t
        (catch #t
               (lambda ()
                 (for-each (lambda (f) (eval f (rootlet))) (cdr c))
                 #t)
               (lambda args
                 (project-warn "a form in (rules ...) faulted")
                 #f)))))

;;! (project-hook clauses head) -> the procedure the HEAD clause names, #f when
;;! the form carries no such clause, or the symbol project-fault when it carries
;;! one that does not yield a procedure. A named hook that is missing or
;;! misspelled is refused rather than dropped: the clause is a project saying
;;! "call this", and silently calling nothing is the failure that looks like a
;;! dead game with a clean log.
(define (project-hook clauses head)
  (let ((c (project-clause clauses head)))
    (cond
     ((not (pair? c)) #f)
     ((not (pair? (cdr c)))
      (project-warn (string-append "empty (" (symbol->string head)
                                   " ...) clause"))
      'project-fault)
     (else
      (let ((v (catch #t
                      (lambda () (eval (cadr c) (rootlet)))
                      (lambda args 'project-fault))))
        (if (procedure? v)
            v
            (begin
              (project-warn (string-append "(" (symbol->string head)
                                           " ...) names no procedure"))
              'project-fault)))))))

;;! (project-call hook args what) — run one of a project's hooks, catching a
;;! fault so a bad frame logs and passes instead of taking the engine down. The
;;! guard is the host's rather than each project's for the same reason the edge
;;! detection is: every project would otherwise write the same catch, and the
;;! one that forgot would lose the frame loop to its own typo.
(define (project-call hook args what)
  (when (procedure? hook)
    (catch #t
           (lambda () (apply hook args))
           (lambda a (project-warn (string-append what " hook fault")) #f))))

;;! (project-open p) — the launcher's load callback for project P, run with the
;;! live world bound. The three steps a game plugin's load did, in the same
;;! order: empty whatever scene was showing, build this project's own, and start
;;! a fresh game. Between the build and the reset the selection baseline is
;;! re-armed from the world itself (a cleared world selects nothing), so the
;;! first click after a load is an edge no matter what was selected before.
(define (project-open p)
  (scene-clear!)
  (let ((form (project-scene p)))
    (when (pair? form)
      (when (< (scene-build! (object->string form)) 0)
        (project-warn (string-append (project-name p)
                                     ": (scene ...) clause did not build")))))
  (project-set-last-sel! p (scene-selected))
  (project-call (project-on-load p) '() "on-load")
  0)

;;! (project-poll-selection p) — the engine's selection is a LEVEL: (scene-
;;! selected) says what is selected now, and says the same thing again next
;;! frame while the player holds a piece. This is where that becomes the event a
;;! project asked for, by remembering the last value and firing only on a change
;;! to a live entity. A change to nothing (-1, a cleared or dismissed selection)
;;! moves the baseline and calls nothing: it is the release of a click, not
;;! another one.
(define (project-poll-selection p)
  (let ((sel (scene-selected)))
    (when (not (= sel (project-last-sel p)))
      (project-set-last-sel! p sel)
      (when (>= sel 0)
        (project-call (project-on-selected p) (list sel) "on-selected")))))

;;! (project-host-tick idx) — one frame of the project at launcher index IDX,
;;! called by the C half with the live world bound, and only while that project
;;! is the loaded one (project_host.c owns the gate). An index that names no
;;! project does nothing, which is what a frame between a launcher click and a
;;! registration would be.
;;!
;;! Input before frame: the selection edge fires first, so a project's on-tick
;;! sees this frame's click rather than last frame's.
(define (project-host-tick idx)
  (let ((entry (assv idx *projects*)))
    (when (pair? entry)
      (project-poll-selection (cdr entry))
      (project-call (project-on-tick (cdr entry)) '() "on-tick")))
  0)

;;! (project-enter name scene on-load on-tick on-selected) -> the launcher index
;;! the project took, or -1 when the launcher refused it. The load callback
;;! handed to game-register! is variadic because the host reaches a Scheme
;;! procedure through the one door that binds the live world for the call, and
;;! that door passes an argument (see project_host.c); a project's own hooks
;;! never see it.
(define (project-enter name scene on-load on-tick on-selected)
  (let* ((p   (project-record name scene on-load on-tick on-selected))
         (idx (game-register! name (lambda args (project-open p)))))
    (if (< idx 0)
        (begin
          (project-warn (string-append "the launcher refused \"" name "\""))
          -1)
        (begin
          (set! *projects* (cons (cons idx p) (project-forget idx *projects*)))
          (set! *project-last-index* idx)
          (krudd-log 1 (string-append "project: registered \"" name "\""))
          idx))))

;;! (project-register spec) -> the launcher index, or -1. SPEC is a (project
;;! ...) form's arguments: its display name followed by its clauses.
;;!
;;! What is refused, registering nothing: a form with no string display name, a
;;! (rules ...) clause whose forms fault, and a hook clause that names no
;;! procedure. What is tolerated: every clause being absent (a project that only
;;! puts a name on the launcher is a legal, if dull, project), and an unknown
;;! clause. The rules run BEFORE the hooks are resolved, since the hooks are
;;! ordinarily names the rules just defined.
(define (project-register spec)
  (set! *project-last-index* -1)
  (set! *project-forms-seen* (+ *project-forms-seen* 1))
  (if (or (not (pair? spec)) (not (string? (car spec))))
      (begin
        (project-warn "a (project ...) form opens with its display name")
        -1)
      (let ((clauses (cdr spec)))
        (if (not (project-run-rules clauses))
            -1
            (let ((on-load     (project-hook clauses 'on-load))
                  (on-tick     (project-hook clauses 'on-tick))
                  (on-selected (project-hook clauses 'on-selected)))
              (if (or (eq? on-load 'project-fault)
                      (eq? on-tick 'project-fault)
                      (eq? on-selected 'project-fault))
                  -1
                  (project-enter (car spec)
                                 (project-clause clauses 'scene)
                                 on-load on-tick on-selected)))))))

;;! The (project NAME clause ...) form itself. A macro, because its clauses are
;;! data — a (scene ...) clause is a scene to build later, not an expression to
;;! evaluate now — and because it registers as an effect: a project source is
;;! evaluated, and this is the form in it that does something.
(define-macro (project . spec)
  `(project-register ',spec))

;;! (project-eval src) -> the launcher index the source registered, or -1.
;;!
;;! Evaluates every top-level form in SRC, so a project file may hold its rules
;;! as ordinary definitions around its (project ...) form as readily as inside a
;;! (rules ...) clause. Everything runs inside a catch: a project source is user
;;! input — the whole point of this module is that a game need not be compiled
;;! in — so text that does not read, or that faults on the way past, is a log
;;! line and a -1, never a lost frame loop.
(define (project-eval src)
  (let ((seen *project-forms-seen*)
        (read-through #t))
    (set! *project-last-index* -1)
    (catch #t
           (lambda ()
             (with-input-from-string src
               (lambda ()
                 (let loop ((form (read)))
                   (when (not (eof-object? form))
                     (eval form (rootlet))
                     (loop (read)))))))
           (lambda args
             (set! read-through #f)
             (project-warn "source fault")
             #f))
    (when (and read-through (= seen *project-forms-seen*))
      (project-warn "source declared no (project ...) form"))
    *project-last-index*))
