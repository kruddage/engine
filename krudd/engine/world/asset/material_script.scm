; SPDX-License-Identifier: GPL-2.0-or-later

;;! material-script — the (material NAME ...) asset source form.
;;!
;;! A material is the odd one out among the engine's asset sources. A mesh, a
;;! texture, a sound and a script all STORE the text they were authored as, and
;;! bake it on demand; a material stores a packed binary — a shader-ref followed
;;! by that shader's std140 Material uniform block, optionally followed by a
;;! texture slot — because that is what the renderer uploads to the GPU
;;! verbatim, per draw, with no interpretation at all. Until now nothing
;;! produced those bytes but C seeders (world/asset/asset_plugin.c) and the
;;! editor, so a project could not bring a material of its own.
;;!
;;! This is the authored form the host packs from:
;;!
;;!   (material ivory
;;!     (shader "builtin://shader/pbr")
;;!     (base_color 0.90 0.85 0.74 1.0)
;;!     (metallic 0.0)
;;!     (roughness 0.32)
;;!     (subsurface 0.6))
;;!
;;! The (shader "PATH") clause is required and names the shader whose Material
;;! block IS this material's schema — a material has no fields of its own, which
;;! is the same fact the editor's material inspector is built on. Every other
;;! clause names one field of that block and gives its components in order.
;;!
;;! What this file deliberately does NOT do is let a source say anything about
;;! std140 — no offsets, no padding, no field order, no block size. Those come
;;! out of shader-material-params (render/shader/shader.scm), the one place the
;;! layout math lives and the same one the renderer sizes a Material UBO with.
;;! An authored source names fields; the layout is derived. That is what keeps a
;;! project's material from being able to get the packing wrong at all.
;;!
;;! What it CAN get wrong is naming: a field the shader does not declare, a
;;! component count that does not match the field's type, a non-number where a
;;! number belongs, a missing or malformed (shader ...) clause, or text that is
;;! not a (material ...) form. Every one of those is REFUSED — material-script-
;;! fields answers #f, and the host registers nothing — rather than defaulted or
;;! dropped, and each refusal names its reason through krudd-log on the way out.
;;! A typo in a field name is the case that matters: silently ignoring it would
;;! leave a material that renders with a wrong value and explains nothing, which
;;! is exactly the failure this form exists to make impossible.
;;!
;;! A field the source simply OMITS is not an error: it takes the shader's own
;;! authored (default V ...), zero-filled for any component that default does
;;! not supply. That is a declared value, not a silent zero — omission is how a
;;! material says "whatever the shader means by unset", and it is what makes a
;;! two-line material source legal.

;;! (material-script-form src) -> the read (material NAME CLAUSE ...) form, or #f
;;! for text that does not read at all or reads as some other form. Every entry
;;! point below goes through this, so malformed text is one answer (#f) and not
;;! a throw out of a host primitive.
(define (material-script-form src)
  (catch #t
         (lambda ()
           (let ((form (with-input-from-string src (lambda () (read)))))
             (and (pair? form)
                  (eq? (car form) 'material)
                  (pair? (cdr form))
                  form)))
         (lambda args #f)))

;;! Log WHY a source was refused and answer #f, so the refusal reaches the author
;;! rather than only the return value. A material-define! that quietly reports 0
;;! is the one thing worse than the wrong blob it prevents: the project sees no
;;! material and no reason. Level 2 is the warn level the other script images
;;! log their faults at.
(define (material-script-refuse text)
  (krudd-log 2 (string-append "material: " text))
  #f)

;;! N as "1 component" / "3 components", so a refusal reads as a sentence.
(define (material-script-plural n word)
  (string-append (number->string n) " " word (if (= n 1) "" "s")))

;;! (material-script-shader src) -> the catalog path of the shader SRC names, or
;;! #f when the form is malformed or carries no (shader "PATH") clause. The host
;;! asks this first: it has to resolve the shader and read its source before it
;;! can know the block layout to pack against — so this is also where the two
;;! coarsest refusals are caught, and it says which one it was.
(define (material-script-shader src)
  (let ((form (material-script-form src)))
    (if (not form)
        (material-script-refuse "not a (material ...) form")
        (let ((c (assq 'shader (cddr form))))
          (if (and (pair? c) (pair? (cdr c)) (string? (cadr c)))
              (cadr c)
              (material-script-refuse "no (shader \"PATH\") clause"))))))

;;! (material-script-texture src) -> (TEXTURE-PATH WIDTH HEIGHT) for a source
;;! carrying a (texture "PATH" W H) clause, or #f for one that does not. The slot
;;! the renderer reads after the Material block (material_texture in
;;! render/scene_renderer/scene_renderer.c): a material naming a procedural
;;! texture asset and the pixel size it wants that texture baked at. Absent is
;;! the common case and not a refusal — most materials are pure parametric — so
;;! this one answers #f quietly where the readers above explain themselves.
(define (material-script-texture src)
  (let ((form (material-script-form src)))
    (and form
         (let ((c (assq 'texture (cddr form))))
           (and (pair? c)
                (= (length c) 4)
                (string? (cadr c))
                (integer? (caddr c))
                (integer? (cadddr c))
                (cdr c))))))

;;! The Material-block field descriptor named by symbol S, or #f when the block
;;! declares none. Descriptors are shader-material-params tuples, whose first
;;! element is the field name as a string.
(define (material-script-field fields s)
  (let loop ((fs fields))
    (cond ((null? fs) #f)
          ((string=? (car (car fs)) (symbol->string s)) (car fs))
          (else (loop (cdr fs))))))

;;! How many float components a descriptor is authorable in — its COMPONENTS
;;! element. 0 means the type is not scalar-editable (a matrix), which is also
;;! the way this form spells "not authorable": a clause naming such a field is
;;! refused rather than packed from values it cannot place.
(define (material-script-components f) (list-ref f 4))

;;! #t when CLAUSE is a well-formed value clause against FIELDS: it names a
;;! declared, authorable field and supplies exactly that field's component count
;;! in numbers. The (shader ...) and (texture ...) clauses are bindings, not
;;! values, and are checked by their own readers above. Anything else is refused
;;! by name, since "which clause" is the whole of what an author needs to know.
(define (material-script-clause-ok? fields clause)
  (if (not (and (pair? clause) (symbol? (car clause))))
      (material-script-refuse "clause is not a (name value ...) list")
      (or (and (memq (car clause) '(shader texture)) #t)
          (let ((f    (material-script-field fields (car clause)))
                (name (symbol->string (car clause))))
            (cond ((not f)
                   (material-script-refuse
                    (string-append "no field named " name
                                   " in the shader's Material block")))
                  ((= (material-script-components f) 0)
                   (material-script-refuse
                    (string-append "field " name " is not authorable")))
                  ((not (= (length (cdr clause))
                           (material-script-components f)))
                   (material-script-refuse
                    (string-append "field " name " wants "
                                   (material-script-plural
                                    (material-script-components f)
                                    "component")
                                   ", got "
                                   (number->string
                                    (length (cdr clause))))))
                  ((let loop ((vs (cdr clause)))
                     (or (null? vs)
                         (and (number? (car vs)) (loop (cdr vs)))))
                   #t)
                  (else
                   (material-script-refuse
                    (string-append "field " name
                                   " takes numbers, not what it was given"))))))))

;;! (material-script-descriptor f vs) -> F with its authored-default element
;;! replaced by VS, the values this material resolved to. Spelled out element by
;;! element because the runtime image has only base s7 — no list-head — and
;;! because naming the nine positions once here is what lets every reader above
;;! reach them by list-ref without a second layout to keep in step.
(define (material-script-descriptor f vs)
  (list (list-ref f 0) (list-ref f 1) (list-ref f 2)
        (list-ref f 3) (list-ref f 4) (list-ref f 5)
        (list-ref f 6) (list-ref f 7) vs))

;;! (material-script-values form f) -> the values FORM gives field descriptor F,
;;! or F's own authored default zero-filled to its component count when FORM says
;;! nothing about it. The result always has exactly COMPONENTS entries, so the
;;! host writes a whole field or none of one and never a half-written vector.
(define (material-script-values form f)
  (let* ((n (material-script-components f))
         (c (assq (string->symbol (car f)) (cddr form)))
         (vs (if (pair? c) (cdr c) (list-ref f 8))))
    (let loop ((i 0) (rest vs) (acc '()))
      (if (>= i n)
          (reverse acc)
          (loop (+ i 1)
                (if (pair? rest) (cdr rest) '())
                (cons (if (pair? rest) (car rest) 0) acc))))))

;;! (material-script-fields SRC SHADER-SRC) -> (TOTAL-SIZE (NAME TYPE OFFSET SIZE
;;! COMPONENTS EDIT-KIND EDIT-MIN EDIT-MAX (V ...)) ...), or #f when SRC is
;;! refused for any of the reasons in this file's header.
;;!
;;! The shape is shader-material-params' own, with each field's authored default
;;! replaced by the value this material resolves to — so the host reuses one
;;! marshaller (core/script.c's query_params) and one struct shader_param for
;;! shaders, scripts, meshes, textures, sounds and now materials. TOTAL-SIZE is
;;! the std140 block size the shader reports, which is the size the packed bytes
;;! must come to.
(define (material-script-fields src shader-src)
  (catch #t
         (lambda ()
           (let ((form (material-script-form src)))
             (and (or form
                      (material-script-refuse "not a (material ...) form"))
                  (let* ((blk    (shader-material-params shader-src))
                         (fields (cdr blk)))
                    (and (let loop ((cs (cddr form)))
                           (or (null? cs)
                               (and (material-script-clause-ok? fields (car cs))
                                    (loop (cdr cs)))))
                         (cons (car blk)
                               (map (lambda (f)
                                      (material-script-descriptor
                                       f (material-script-values form f)))
                                    fields)))))))
         (lambda args (material-script-refuse "fault reading the source"))))
