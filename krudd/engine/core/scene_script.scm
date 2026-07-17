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

;;! (scene-vec3 xs a b c) -> the first three of XS, each defaulting to a/b/c when
;;! XS runs short — so (at 0 0 0), a bare (at), or an over-long clause all yield a
;;! well-formed triple and scene-xform! always gets its nine numbers.
(define (scene-vec3 xs a b c)
  (list (if (pair? xs) (car xs) a)
        (if (and (pair? xs) (pair? (cdr xs))) (cadr xs) b)
        (if (and (pair? xs) (pair? (cdr xs)) (pair? (cddr xs))) (caddr xs) c)))

;;! (scene-entity-build e) -> id: spawn one (entity CLAUSE ...) and apply its
;;! clauses. Transform clauses accumulate into pos/rot/scl and land in a single
;;! scene-xform! after the walk; binding clauses take effect immediately. An
;;! unknown clause is ignored, so a newer scene degrades gracefully on an older
;;! engine rather than faulting the whole build.
(define (scene-entity-build e)
  (let ((id  (scene-spawn))
        (pos '()) (rot '()) (scl '()))
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
      (cdr e))
    (apply scene-xform! id (append (scene-vec3 pos 0 0 0)
                                   (scene-vec3 rot 0 0 0)
                                   (scene-vec3 scl 1 1 1)))
    id))

;;! (scene-build src) -> entity count. Parse SRC (a (scene NAME (entity ...) ...)
;;! form as text) and spawn its entities into the world the host has bound. A
;;! malformed form, or a per-entity fault, is caught and logged, never taking the
;;! frame or the rest of the build down — the shape mesh-script-run uses.
(define (scene-build src)
  (catch #t
    (lambda ()
      (let ((form (with-input-from-string src (lambda () (read)))))
        (if (and (pair? form) (eq? (car form) 'scene))
            (let ((n 0))
              (for-each
                (lambda (c)
                  (when (and (pair? c) (eq? (car c) 'entity))
                    (catch #t
                      (lambda () (scene-entity-build c) (set! n (+ n 1)))
                      (lambda args (krudd-log 2 "scene: entity build fault")))))
                (cddr form))
              n)
            (begin (krudd-log 2 "scene: not a (scene ...) form") 0))))
    (lambda args (krudd-log 2 "scene: build fault") 0)))
