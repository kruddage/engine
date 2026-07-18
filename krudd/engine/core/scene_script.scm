; SPDX-License-Identifier: GPL-2.0-or-later

;;! scene-script — the (scene NAME (entity ...) ...) form and its builder, the
;;! declarative twin of the (mesh ...) / (script ...) / (texture ...) DSLs. A
;;! scene is a list of entity declarations; scene-build walks the form and calls
;;! the scene-* host primitives (entity/scene_script.c) to spawn and bind each
;;! one. Same source-text-is-the-asset pattern every other asset uses: a scene is
;;! plain S7, read and evaluated against the shared image, so it needs no bespoke
;;! deserializer — loading a scene IS evaluating it.
;;!
;;!   (scene tic-tac-toe
;;!     (entity (name "board")
;;!             (mesh "builtin://mesh/plane")
;;!             (material "builtin://material/checker")
;;!             (at 0 0 0) (scale 3 3 3))
;;!     (entity (name "o")
;;!             (mesh "builtin://mesh/torus")
;;!             (material "builtin://material/pbr-metal")
;;!             (at -1 0.2 -1) (scale 0.4 0.4 0.4)))
;;!
;;! Each (entity CLAUSE ...) clause is one of — order-independent, all optional:
;;!   (name "S")        human-readable label
;;!   (mesh "PATH")     bind a mesh asset by catalog path (COMPONENT_RENDER)
;;!   (material "PATH") bind a material asset by catalog path
;;!   (script "PATH")   bind an entity behavior script by catalog path
;;!   (at X Y Z)        authored position       (default 0 0 0)
;;!   (rotate X Y Z)    authored euler degrees   (default 0 0 0)
;;!   (scale X Y Z)     authored scale          (default 1 1 1)
;;!   (children E ...)  nested (entity ...) forms, spawned under this one — their
;;!                     transforms are local to it, so a group moves as a unit
;;!                     (a mesh-less parent + two crossed bars is one X piece)

;;! (scene-vec3 xs a b c) -> the first three of XS, each defaulting to a/b/c when
;;! XS runs short — so (at 0 0 0), a bare (at), or an over-long clause all yield a
;;! well-formed triple and scene-xform! always gets its nine numbers.
(define (scene-vec3 xs a b c)
  (list (if (pair? xs) (car xs) a)
        (if (and (pair? xs) (pair? (cdr xs))) (cadr xs) b)
        (if (and (pair? xs) (pair? (cdr xs)) (pair? (cddr xs))) (caddr xs) c)))

;;! (scene-parse src) -> flat pre-order list of (clauses . parent-idx), or #f on
;;! a malformed form. Pure — no scene-* host calls, no world mutation — so it can
;;! run once up front and the resulting plan replayed a node or a few at a time
;;! across frames by the C-side chunked builder (scene_script_build_tick), rather
;;! than spawning an entire scene inside one call. parent-idx is the index of the
;;! node's already-flattened parent in this same list, or -1 for a root; children
;;! clauses are consumed here (they exist only to shape the walk) and do not
;;! appear in the flattened CLAUSES.
(define (scene-parse src)
  (catch #t
    (lambda ()
      (let ((form (with-input-from-string src (lambda () (read))))
            (flat '())
            (next 0))
        (define (children-of clauses)
          (let ((kv (assq 'children clauses)))
            (if kv (cdr kv) '())))
        (define (walk entities parent)
          (for-each
            (lambda (e)
              (when (and (pair? e) (eq? (car e) 'entity))
                (let ((idx next))
                  (set! flat (cons (cons (cdr e) parent) flat))
                  (set! next (+ next 1))
                  (walk (children-of (cdr e)) idx))))
            entities))
        (if (and (pair? form) (eq? (car form) 'scene))
            (begin (walk (cddr form) -1) (reverse flat))
            (begin (krudd-log 2 "scene: not a (scene ...) form") #f))))
    (lambda args (krudd-log 2 "scene: parse fault") #f)))

;;! (scene-plan-apply! parent-id clauses) -> new entity id, or -1 on a
;;! per-entity fault (logged, never aborting the caller's loop — the shape
;;! mesh-script-run uses). Spawns one entity under PARENT-ID (a live world id,
;;! or -1 for a root) and applies its non-tree clauses; CLAUSES is one flattened
;;! node's data from scene-parse, so children are never present here.
(define (scene-plan-apply! parent-id clauses)
  (catch #t
    (lambda ()
      (let ((id (scene-spawn parent-id)) (pos '()) (rot '()) (scl '()))
        (for-each
          (lambda (c)
            (when (pair? c)
              (case (car c)
                ((name)     (scene-name!     id (cadr c)))
                ((mesh)     (scene-mesh!     id (cadr c)))
                ((material) (scene-material! id (cadr c)))
                ((script)   (scene-script!   id (cadr c)))
                ((at)       (set! pos (cdr c)))
                ((rotate)   (set! rot (cdr c)))
                ((scale)    (set! scl (cdr c)))
                (else #f))))
          clauses)
        (apply scene-xform! id (append (scene-vec3 pos 0 0 0)
                                       (scene-vec3 rot 0 0 0)
                                       (scene-vec3 scl 1 1 1)))
        id))
    (lambda args (krudd-log 2 "scene: entity build fault") -1)))

;;! (scene-entity-build e parent) -> subtree entity count: spawn one
;;! (entity CLAUSE ...) immediately under PARENT (an id, or -1 for a root),
;;! recursing into any (children ...) right away rather than through the
;;! chunked plan scene-build uses. For a runtime dynamic spawn outside the
;;! initial scene build — a handful of entities at a time, not hundreds — such
;;! as a game placing one mark per click (rules.scm's ttt-place/ttt-strike).
(define (scene-entity-build e parent)
  (let* ((clauses (cdr e))
         (id (scene-plan-apply! parent clauses))
         (count 1))
    (for-each
      (lambda (k)
        (when (and (pair? k) (eq? (car k) 'entity))
          (set! count (+ count (scene-entity-build k id)))))
      (let ((kv (assq 'children clauses))) (if kv (cdr kv) '())))
    count))

;;! (scene-build src) -> entity count. Parse SRC (a (scene NAME (entity ...) ...)
;;! form as text) and spawn its entities into the world the host has bound, all
;;! in one call — the synchronous convenience over scene-parse + scene-plan-apply!
;;! for a caller (native builds, tests) that has no per-frame tick to chunk
;;! across. A malformed form, or a per-entity fault, is caught and logged, never
;;! taking the rest of the build down.
(define (scene-build src)
  (let ((plan (scene-parse src)))
    (if plan
        (let ((ids (make-vector (length plan) -1)) (i 0) (n 0))
          (for-each
            (lambda (node)
              (let* ((parent-idx (cdr node))
                     (parent-id (if (>= parent-idx 0) (vector-ref ids parent-idx) -1))
                     (id (scene-plan-apply! parent-id (car node))))
                (vector-set! ids i id)
                (when (>= id 0) (set! n (+ n 1)))
                (set! i (+ i 1))))
            plan)
          n)
        0)))
