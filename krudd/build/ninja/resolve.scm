; SPDX-License-Identifier: GPL-2.0-or-later
;; scm-lint:off
;
; resolve.scm — the static include/link resolver.
;
; The directory specs (krudd/build/ninja/**/build.scm) name each target's own
; include directories and the libraries it links, but not the include
; directories those libraries drag in. A (link ... "B") means A wants every
; PUBLIC include B declares, and that flattens transitively — if A links B and
; B links C, A can see C's public headers. This pass owns that flattening
; explicitly (the job CMake's target_link_libraries once did for us).
;
; The input is the whole manifest as a list of (DIR . SPEC) pairs, where DIR is
; a directory path relative to krudd/build/ninja/ (e.g. "modules/log") and SPEC
; is the datum read from that directory's build.scm. The output, per target, is
; the ordered de-duplicated list of include directories a compile line needs —
; the target's own PUBLIC then PRIVATE dirs, then every linked library's PUBLIC
; dirs, transitively.
;
; Paths come out relative to the tree root (krudd/build/ninja/): a bare "include" in
; modules/log becomes "modules/log/include", (root "p") becomes "p". The one
; escape hatch, (raw "text"), passes through unchanged (for the generated and
; fetched paths the WASM-only modules reference, like ${imgui}); native leaf
; targets never carry those.
;
; Two failure modes fail loudly at resolution time rather than mis-linking
; later: a cycle in the link graph, and a link entry that names neither a
; defined target nor a known system library.
;; scm-lint:on

;; scm-lint:off
;; ---------------------------------------------------------------------------
;; Small list helpers (s7 gives us map/for-each/assoc/member; the rest we spell
;; out).
;; ---------------------------------------------------------------------------
;; scm-lint:on

(define (rz-filter pred lst)
	(cond ((null? lst) '())
	      ((pred (car lst)) (cons (car lst) (rz-filter pred (cdr lst))))
	      (else (rz-filter pred (cdr lst)))))

;; scm-lint:off
;; De-duplicate a list of strings, keeping the first occurrence's position.
;; scm-lint:on
(define (rz-dedup lst)
	(let loop ((l lst) (seen '()) (out '()))
		(cond ((null? l) (reverse out))
		      ((member (car l) seen) (loop (cdr l) seen out))
		      (else (loop (cdr l) (cons (car l) seen)
				  (cons (car l) out))))))

;; scm-lint:off
;; Find the clause whose head is HEAD in a target's clause list, or #f.
;; scm-lint:on
(define (rz-clause head clauses)
	(cond ((null? clauses) #f)
	      ((and (pair? (car clauses)) (eq? (caar clauses) head))
	       (car clauses))
	      (else (rz-clause head (cdr clauses)))))

;; scm-lint:off
;; The libraries linked against by a native build that are not targets we
;; define — they resolve to a plain -l flag and carry no include directories.
;; Keep this list tight so a mistyped internal target name is caught, not
;; silently treated as a system library.
;; scm-lint:on
(define rz-system-libs (list "m"))

(define (rz-system-lib? name) (member name rz-system-libs))

;; scm-lint:off
;; ---------------------------------------------------------------------------
;; Path expansion. A source/include path spec resolves relative to the tree
;; root (krudd/build/ninja/) — the source tree krudd owns. DIR is the owning
;; directory, relative to that root.
;; ---------------------------------------------------------------------------
;; scm-lint:on

;; scm-lint:off
;; Join a directory and a relative component, collapsing "." to just the dir so
;; "." in modules/asset yields "modules/asset", not "modules/asset/.".
;; scm-lint:on
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

;; scm-lint:off
;; The include dirs named by a clause (public/private/interface), expanded.
;; scm-lint:on
(define (rz-clause-dirs dir clauses head)
	(let ((c (rz-clause head clauses)))
		(if c (rz-paths dir (cdr c)) '())))

;; scm-lint:off
;; ---------------------------------------------------------------------------
;; Target table. Walk every spec, flattening (native-only ...) / (wasm-only ...)
;; wrappers, and record each link/include target (library, interface-library,
;; executable) as an alist entry name -> (dir kind public private links
;; wasm-modules). test and the root scaffolding forms are not link targets and
;; are skipped here. An executable's (wasm-modules ...) clause names the plugin
;; libraries the WASM main module folds in beyond its own (link ...); it is
;; empty for every other target.
;; ---------------------------------------------------------------------------
;; scm-lint:on

(define (rz-make-target name dir kind public private links wasm-modules)
	(list name (cons 'dir dir) (cons 'kind kind)
	      (cons 'public public) (cons 'private private)
	      (cons 'links links) (cons 'wasm-modules wasm-modules)))

(define (rz-field target key) (cdr (assq key (cdr target))))

;; scm-lint:off
;; Pull a target record out of a single form, or #f if the form is not a target.
;; scm-lint:on
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

;; scm-lint:off
;; Flatten one spec into its target records, descending into native-only and
;; wasm-only (both are toolchain gates around otherwise ordinary target forms;
;; the resolver treats their contents as plain targets — only the emitter cares
;; which toolchain builds them).
;; scm-lint:on
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

;; scm-lint:off
;; Build the whole target table from the (dir . spec) manifest.
;; scm-lint:on
(define (rz-target-table manifest)
	(apply append
	       (map (lambda (pair) (rz-spec-targets (car pair) (cdr pair)))
		    manifest)))

(define (rz-lookup table name) (assoc name table))

;; scm-lint:off
;; The internal library dependencies named by NAME's link clause. Each link
;; entry must be a defined target or a known system library; anything else is a
;; typo we refuse to paper over. System libraries are dropped (they add no
;; include dirs and no graph edge); defined targets are kept as edges.
;; scm-lint:on
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

;; scm-lint:off
;; Link closure. Depth-first walk of the link graph from a set of roots,
;; returning the transitive internal libraries dependents-first (a library
;; appears before the libraries it depends on — the order a single-pass static
;; linker needs). A back-edge onto the active DFS path is a cycle and errors.
;; state maps name -> 'active | 'done (latest cons wins).
;; scm-lint:on

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

;; scm-lint:off
;; ---------------------------------------------------------------------------
;; Public API.
;; ---------------------------------------------------------------------------
;; scm-lint:on

;; scm-lint:off
;; The link libraries (internal only) a target must link, dependents-first,
;; transitively. Roots are the target's own direct internal deps, so the target
;; itself is not in the result.
;; scm-lint:on
(define (resolve-link-libs table name)
	(rz-closure table (rz-direct-deps table name)))

;; scm-lint:off
;; The full internal-library closure the WASM main module folds in: NAME's own
;; direct deps (the core libraries the executable links) together with the
;; plugin libraries its (wasm-modules ...) clause names, flattened
;; dependents-first. Unlike resolve-link-libs, the wasm-module libraries ARE in
;; the result — they are the plugins themselves, not just NAME's dependencies —
;; each pulled in as its own archive.
;; scm-lint:on
(define (resolve-wasm-module-libs table name)
	(let ((target (rz-lookup table name)))
		(rz-closure table
			(append (if target (rz-field target 'wasm-modules) '())
				(rz-direct-deps table name)))))

;; scm-lint:off
;; The system libraries (-l flags) a target must link. A static library's own
;; system-lib dependency (asset_plugin links m) has to reach the final
;; executable, exactly as CMake propagates a PRIVATE link requirement through
;; the static archive, so gather them across the whole link closure.
;; scm-lint:on
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

;; scm-lint:off
;; The include directories a target's own sources compile against: its own
;; PUBLIC then PRIVATE dirs, then every transitively linked library's PUBLIC
;; dirs, de-duplicated. Errors (cycle / unknown target) surface here.
;; scm-lint:on
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

;; scm-lint:off
;; Force resolution of every target — the cheap way to make cycles and unknown
;; link targets fail loudly for the whole manifest, not just the targets an
;; emitter happens to touch.
;; scm-lint:on
(define (resolve-check-all table)
	(for-each (lambda (target) (resolve-includes table (car target)))
		  table)
	#t)
