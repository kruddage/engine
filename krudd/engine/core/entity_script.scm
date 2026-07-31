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

;;! A (params ...) clause declares authored data, not a lifecycle hook; strip it
;;! out so it never becomes a bogus (params . lambda) pair. script-params reads
;;! it separately, straight from the source form.
(define (entity-script-hook-clauses clauses)
  (cond ((null? clauses) '())
        ((eq? (caar clauses) 'params)
         (entity-script-hook-clauses (cdr clauses)))
        (else (cons (car clauses)
                    (entity-script-hook-clauses (cdr clauses))))))

;;! The (script NAME clause ...) form evaluates to its hook alist — each hook
;;! clause becomes a (hook-symbol . procedure) pair over the clause's parameters.
;;! NAME is kept only as a human-readable label; it is NOT a registry key, so two
;;! scripts that share a name — a clone and the original it was copied from, say
;;! — never collide, and an edited clone runs its own hooks rather than silently
;;! inheriting its name-twin's. A (params ...) clause is data, not a hook, and is
;;! skipped here (see script-params).
(define-macro (script name . clauses)
  `(list ,@(map (lambda (c)
                  `(cons ',(car c) (lambda ,(cadr c) ,@(cddr c))))
                (entity-script-hook-clauses clauses))))

;;! --- script parameter introspection ---
;;!
;;! A script may carry a (params (NAME TYPE [(edit ...)]) ...) clause — the
;;! CPU-side twin of a shader's Material uniform block. The host and the editor
;;! derive a script's authorable parameters from it exactly as they derive a
;;! material's from the shader. Unlike the shader block this is packed TIGHT (no
;;! std140 padding): there is no GPU layout to satisfy, so a vec3 is 12 bytes
;;! flush against its neighbour. (script-params SRC) reports the same shape as
;;! (shader-material-params SRC) so one C marshaller and one set of editor widgets
;;! serve both.

;;! Bytes a tight-packed param type occupies (0 = not an editable scalar type).
(define (script-param-size t)
  (case t
    ((float int) 4) ((vec2) 8) ((vec3) 12) ((vec4) 16)
    (else 0)))

;;! Float components the editor exposes for a type (matches shader-type-components).
(define (script-param-components t)
  (case t
    ((float int) 1) ((vec2) 2) ((vec3) 3) ((vec4) 4)
    (else 0)))

;;! The edit hint of a field as (KIND MIN MAX): ("none" 0 0), ("color" 0 0) or
;;! ("range" MIN MAX). A field is (NAME TYPE) or (NAME TYPE (edit ...)).
(define (script-field-edit f)
  (let ((e (assq 'edit (cddr f))))
    (cond ((not e) (list "none" 0 0))
          ((eq? (cadr e) 'color) (list "color" 0 0))
          ((eq? (cadr e) 'range) (list "range" (caddr e) (cadddr e)))
          (else (list "none" 0 0)))))

;;! The authored default of a field as a list of component values, or '() when it
;;! declares none. A field may carry (default V ...) alongside its (edit ...) to
;;! seed its un-overridden value independently of the edit hint — so a range
;;! param can rest at an interior value (a wobble that ships at 15°) yet still
;;! slide down to its min. '() means "fall back to the edit-hint default".
(define (script-field-default f)
  (let ((d (assq 'default (cddr f))))
    (if d (cdr d) '())))

;;! The (params ...) fields of a (script ...) form, or '() when it declares none.
(define (script-params-fields form)
  (let ((p (assq 'params (cddr form))))
    (if p (cdr p) '())))

;;! Walk the params clause into (TOTAL-SIZE (NAME TYPE OFFSET SIZE COMPONENTS
;;! EDIT-KIND EDIT-MIN EDIT-MAX DEFAULT) ...), tight-packed. DEFAULT is the
;;! field's authored (default V ...) values or '() for none. Total is 0 with no
;;! params.
(define (script-params-form form)
  (let loop ((fields (script-params-fields form)) (off 0) (acc '()))
    (if (null? fields)
        (cons off (reverse acc))
        (let* ((f  (car fields))
               (ty (cadr f))
               (sz (script-param-size ty))
               (ed (script-field-edit f)))
          (loop (cdr fields)
                (+ off sz)
                (cons (list (symbol->string (car f))
                            (symbol->string ty)
                            off sz
                            (script-param-components ty)
                            (car ed) (cadr ed) (caddr ed)
                            (script-field-default f))
                      acc))))))

;;! Entry point mirrored by the C seam (script_entity_params) and the editor.
(define (script-params src)
  (script-params-form (with-input-from-string src (lambda () (read)))))

;;! The authored parameter values in scope only for the span of a scripted
;;! entity's hook calls — or a mesh script's generate call: an ((name . value)
;;! ...) alist (value a number or a list of numbers) the host resolves and binds
;;! around the call. Shared between the entity-script and mesh-script drivers so
;;! (param NAME) means the same thing in both — they never run concurrently
;;! (both are synchronous s7 calls), so one slot serves both.
(define *params* '())

;;! (param NAME) -> the current script/mesh's value for parameter NAME (a
;;! symbol), or #f when nothing in scope declares it. A clause or generator reads
;;! its authored inputs through this, staying stateless: an entity pose is a pure
;;! function of the clock, the rest pose, and these params; a mesh's geometry a
;;! pure function of these params.
(define (param name)
  (let ((p (assq name *params*)))
    (and p (cdr p))))

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

;;! Render a caught s7 error as the one-line message the interpreter itself
;;! would have printed. A catch #t handler is handed (TYPE (FORMAT-STRING .
;;! FORMAT-ARGS)) for an engine error — a read of an undeclared (param ...) that
;;! reaches arithmetic, say, arrives as ("~A ~:D argument, ~S, is ~A ..." ...) —
;;! so rebuilding the message is just applying format to that pair. Anything
;;! that does not fit the shape (a raw throw carrying a non-standard payload)
;;! falls back to its printed form, so the detail is never empty.
(define (entity-script-error->string args)
  (if (and (pair? args) (pair? (cdr args)) (pair? (cadr args))
           (string? (caadr args)))
      (apply format #f (caadr args) (cdadr args))
      (object->string args)))

;;! Run one frame for entity ID bound to script source SRC at clock T. Fires
;;! on-begin the first frame, then on-tick every frame. A fault in one entity's
;;! script is caught and logged — with the interpreter's own error message, so
;;! the log says WHAT threw, not merely that something did — never taking the
;;! frame down (mirrors the graceful-degradation contract of the krudd-log
;;! primitive).
(define (entity-script-tick id src t params)
  (catch #t
    (lambda ()
      (let ((hooks (entity-script-resolve id src)))
        (when hooks
          (set! *params* params)
          (unless (hash-table-ref *entity-script-begun* id)
            (let ((h (entity-script-clause hooks 'on-begin)))
              (when h (h id)))
            (hash-table-set! *entity-script-begun* id #t))
          (let ((h (entity-script-clause hooks 'on-tick)))
            (when h (h id t)))
          (set! *params* '()))))
    (lambda args
      (set! *params* '())
      (krudd-log 2 (string-append "entity-script: fault on entity "
                                  (number->string id) ": "
                                  (entity-script-error->string args)))
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
