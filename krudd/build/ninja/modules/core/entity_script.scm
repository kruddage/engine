; SPDX-License-Identifier: GPL-2.0-or-later

;;! entity-script — the (script ...) form and the per-entity dispatcher.
;;!
;;! An entity script is one (script NAME clause ...) S-expression, mirroring the
;;! shader DSL's (shader NAME ...). Each clause is a lifecycle hook:
;;!
;;!   (script spinner
;;!     (on-begin (self)   ...)   ; once, the first tick after the script binds
;;!     (on-tick  (self t) ...)   ; every frame; t = seconds since boot
;;!     (on-destroy (self) ...))  ; once, when the script is unbound
;;!
;;! `self` is the entity id (an integer). Clauses drive the entity through the
;;! host primitives the engine registers — entity-base-position / -scale (READ
;;! the authored rest pose) and entity-set-position! / -scale! / -euler! (WRITE
;;! the animated render pose). Reading rest + writing animated keeps scripts
;;! stateless yet drift-free: the pose is a pure function of `t` and the base.
;;!
;;! The engine's entity-script driver (entity_script.c) calls entity-script-tick
;;! once per bound entity per frame, handing it the entity id, the script's
;;! source text (the bytes of the bound ASSET_TYPE_SCRIPT), and the clock. The
;;! source is parsed and registered once per source text; results are cached per
;;! id.

;;! Registry and per-entity caches:
;;!   *entity-scripts*      src -> ((hook . proc) ...)  scripts parsed by source
;;!   *entity-script-bound* id  -> (src . hooks)        cached per source text
;;!   *entity-script-begun* id  -> #t                   set once on-begin runs
(define *entity-scripts*       (make-hash-table))
(define *entity-script-bound*  (make-hash-table))
(define *entity-script-begun*  (make-hash-table))

;;! The (script NAME clause ...) form evaluates to its hook alist — each clause
;;! becomes a (hook-symbol . procedure) pair over the clause's parameters. NAME
;;! is kept only as a human-readable label; it is NOT a registry key, so two
;;! scripts that share a name — a clone and the original it was copied from, say
;;! — never collide, and an edited clone runs its own hooks rather than silently
;;! inheriting its name-twin's.
(define-macro (script name . clauses)
  `(list ,@(map (lambda (c)
                  `(cons ',(car c) (lambda ,(cadr c) ,@(cddr c))))
                clauses)))

;;! The procedure a script binds to hook KEY, or #f when it defines no such hook.
(define (entity-script-clause hooks key)
  (let ((p (assq key hooks)))
    (and p (cdr p))))

;;! Parse (script ...) source text and register it the first time this exact
;;! source is seen; return its hook alist. Keying on the source text (not the
;;! form's NAME) means identical sources still parse only once — the shared-
;;! script fast path — while any change to the text, including a clone the
;;! author has started editing, resolves to its own fresh hooks.
(define (entity-script-register-source src)
  (or (hash-table-ref *entity-scripts* src)
      (let ((hooks (eval (with-input-from-string src (lambda () (read)))
                         (rootlet))))
        (hash-table-set! *entity-scripts* src hooks)
        hooks)))

;;! Hooks bound to entity ID, resolving+caching from SRC. A cache hit requires
;;! SRC to match what's cached, so rebinding the entity to a different script
;;! asset at runtime (a different source text) re-resolves instead of silently
;;! keeping the old hooks: the old script's on-destroy fires, the begun flag
;;! clears so the new script's on-begin fires on its first tick, and the new
;;! (src . hooks) pair is cached.
(define (entity-script-resolve id src)
  (let ((cached (hash-table-ref *entity-script-bound* id)))
    (if (and cached (string=? (car cached) src))
        (cdr cached)
        (let ((hooks (entity-script-register-source src)))
          (when cached
            (let ((h (entity-script-clause (cdr cached) 'on-destroy)))
              (when h (h id))))
          (hash-table-set! *entity-script-bound* id (cons src hooks))
          (hash-table-set! *entity-script-begun* id #f)
          hooks))))

;;! Run one frame for entity ID bound to script source SRC at clock T. Fires
;;! on-begin the first frame, then on-tick every frame. A fault in one entity's
;;! script is caught and logged, never taking the frame down (mirrors the
;;! graceful-degradation contract of the krudd-log primitive).
(define (entity-script-tick id src t)
  (catch #t
    (lambda ()
      (let ((hooks (entity-script-resolve id src)))
        (when hooks
          (unless (hash-table-ref *entity-script-begun* id)
            (let ((h (entity-script-clause hooks 'on-begin)))
              (when h (h id)))
            (hash-table-set! *entity-script-begun* id #t))
          (let ((h (entity-script-clause hooks 'on-tick)))
            (when h (h id t))))))
    (lambda args
      (krudd-log 2 (string-append "entity-script: fault on entity "
                                  (number->string id)))
      #f)))

;;! Run the on-destroy hook (if any) for entity ID and forget its cached
;;! binding, so a later rebind re-resolves and re-fires on-begin.
(define (entity-script-end id)
  (let ((cached (hash-table-ref *entity-script-bound* id)))
    (when cached
      (let ((h (entity-script-clause (cdr cached) 'on-destroy)))
        (when h (h id)))))
  (hash-table-set! *entity-script-bound* id #f)
  (hash-table-set! *entity-script-begun* id #f))
