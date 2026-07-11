; SPDX-License-Identifier: GPL-2.0-or-later

;;! mesh-script — the (mesh NAME (generate () ...)) form and the mesh
;;! generator, mirroring entity_script.scm's (script NAME ...) dispatcher.
;;!
;;! A mesh script is one (mesh NAME (generate () EXPR ...)) S-expression. Its
;;! generate clause takes no arguments and must evaluate to (cons VERTS
;;! INDICES): VERTS a list of 8-element (px py pz nx ny nz u v) lists,
;;! INDICES a flat list of vertex-index integers, a multiple of 3 (one
;;! triangle per triple) — the same shape asset/primitives.scm already
;;! produces for the four built-in primitives.
;;!
;;! The engine's mesh-script driver (asset/mesh_script.c) calls
;;! mesh-script-generate with the source text of a bound ASSET_TYPE_MESH_SCRIPT
;;! asset and marshals the (VERTS . INDICES) result into a mesh_blob. Unlike
;;! an entity script (re-run every tick, cheap pose math) a mesh script is a
;;! pure function of nothing — no clock, no entity — so its result is parsed
;;! AND generated once per exact source text, then cached: a render upload and
;;! an editor hit-test both asking for the same mesh cost nothing after the
;;! first.

(define *mesh-scripts* (make-hash-table))

;;! The (mesh NAME (generate () BODY ...)) form evaluates to a zero-argument
;;! thunk over BODY. NAME is a human-readable label only, exactly like the
;;! entity script DSL's NAME — the registry below keys on source text, not
;;! NAME, so a renamed clone never collides with the original it was copied
;;! from.
(define-macro (mesh name generate-clause)
  `(lambda () ,@(cddr generate-clause)))

;;! (mesh-script-generate src) -> (VERTS . INDICES), parsing and running SRC's
;;! generate thunk the first time this exact source is seen and caching the
;;! result. A fault (a malformed form, or a generate body that errors) is
;;! caught and logged, never taking the frame down, and caches to an empty
;;! mesh so a broken script degrades to "nothing renders" rather than a fault
;;! on every subsequent call.
(define (mesh-script-generate src)
  (or (hash-table-ref *mesh-scripts* src)
      (let ((result
              (catch #t
                (lambda ()
                  (let ((thunk (eval (with-input-from-string src (lambda () (read)))
                                     (rootlet))))
                    (thunk)))
                (lambda args
                  (krudd-log 2 (string-append "mesh-script: fault: " src))
                  (cons '() '())))))
        (hash-table-set! *mesh-scripts* src result)
        result)))
