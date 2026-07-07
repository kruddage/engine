; SPDX-License-Identifier: GPL-2.0-or-later
;
; resolve.scm — the static include/link resolver.
;
; The directory specs (krudd/cmake/**/CMakeLists.scm) name each target's own
; include directories and the libraries it links, but not the include
; directories those libraries drag in. CMake computes that for us: a
; target_link_libraries(A PRIVATE B) hands A every PUBLIC include B declares,
; and it flattens transitively — if A links B and B links C, A can see C's
; public headers. The Ninja emitter has no such machinery, so this pass owns
; that flattening explicitly.
;
; The input is the whole manifest as a list of (DIR . SPEC) pairs, where DIR is
; a directory path relative to krudd/cmake/ (e.g. "modules/log") and SPEC is the
; datum read from that directory's CMakeLists.scm. The output, per target, is
; the ordered de-duplicated list of include directories a compile line needs —
; the target's own PUBLIC then PRIVATE dirs, then every linked library's PUBLIC
; dirs, transitively.
;
; Paths come out relative to the tree root (krudd/cmake/): a bare "include" in
; modules/log becomes "modules/log/include", (root "p") becomes "p". The one
; escape hatch, (raw "text"), passes through unchanged (for CMake variables the
; WASM side-module builds reference, like ${imgui_SOURCE_DIR}); native leaf
; targets never carry those.
;
; Two failure modes fail loudly at resolution time rather than mis-linking
; later: a cycle in the link graph, and a link entry that names neither a
; defined target nor a known system library.

;; ---------------------------------------------------------------------------
;; Small list helpers (s7 gives us map/for-each/assoc/member; the rest we spell
;; out, as cmake.scm does for string-join).
;; ---------------------------------------------------------------------------

(define (rz-filter pred lst)
	(cond ((null? lst) '())
	      ((pred (car lst)) (cons (car lst) (rz-filter pred (cdr lst))))
	      (else (rz-filter pred (cdr lst)))))

;; De-duplicate a list of strings, keeping the first occurrence's position.
(define (rz-dedup lst)
	(let loop ((l lst) (seen '()) (out '()))
		(cond ((null? l) (reverse out))
		      ((member (car l) seen) (loop (cdr l) seen out))
		      (else (loop (cdr l) (cons (car l) seen)
				  (cons (car l) out))))))

;; Find the clause whose head is HEAD in a target's clause list, or #f.
(define (rz-clause head clauses)
	(cond ((null? clauses) #f)
	      ((and (pair? (car clauses)) (eq? (caar clauses) head))
	       (car clauses))
	      (else (rz-clause head (cdr clauses)))))

;; The libraries linked against by a native build that are not targets we
;; define — they resolve to a plain -l flag and carry no include directories.
;; Keep this list tight so a mistyped internal target name is caught, not
;; silently treated as a system library.
(define rz-system-libs (list "m"))

(define (rz-system-lib? name) (member name rz-system-libs))

;; ---------------------------------------------------------------------------
;; Path expansion. A source/include path spec resolves relative to the tree
;; root (krudd/cmake/), matching what ${CMAKE_SOURCE_DIR} meant to the CMake
;; backend. DIR is the owning directory, relative to that root.
;; ---------------------------------------------------------------------------

;; Join a directory and a relative component, collapsing "." to just the dir so
;; "." in plugins/asset yields "plugins/asset", not "plugins/asset/.".
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

;; The include dirs named by a clause (public/private/interface), expanded.
(define (rz-clause-dirs dir clauses head)
	(let ((c (rz-clause head clauses)))
		(if c (rz-paths dir (cdr c)) '())))

;; ---------------------------------------------------------------------------
;; Target table. Walk every spec, flattening (native-only ...) wrappers, and
;; record each link/include target (library, interface-library, executable) as
;; an alist entry name -> (dir kind public private links). side-module/test and
;; the root scaffolding forms are not link targets and are skipped here.
;; ---------------------------------------------------------------------------

(define (rz-make-target name dir kind public private links)
	(list name (cons 'dir dir) (cons 'kind kind)
	      (cons 'public public) (cons 'private private)
	      (cons 'links links)))

(define (rz-field target key) (cdr (assq key (cdr target))))

;; Pull a target record out of a single form, or #f if the form is not a target.
(define (rz-form->target dir form)
	(case (car form)
	  ((library executable)
	   (let ((clauses (cddr form)))
		(rz-make-target
		  (cadr form) dir (car form)
		  (rz-clause-dirs dir clauses 'public)
		  (rz-clause-dirs dir clauses 'private)
		  (let ((c (rz-clause 'link clauses))) (if c (cdr c) '())))))
	  ((interface-library)
	   (rz-make-target
	     (cadr form) dir 'interface-library
	     (rz-clause-dirs dir (cddr form) 'interface)
	     '() '()))
	  (else #f)))

;; Flatten one spec into its target records, descending into native-only.
(define (rz-spec-targets dir spec)
	(let loop ((forms spec) (out '()))
		(cond ((null? forms) (reverse out))
		      ((eq? (caar forms) 'native-only)
		       (loop (cdr forms)
			     (append (reverse (rz-spec-targets dir (cdar forms)))
				     out)))
		      (else
			(let ((t (rz-form->target dir (car forms))))
			  (loop (cdr forms) (if t (cons t out) out)))))))

;; Build the whole target table from the (dir . spec) manifest.
(define (rz-target-table manifest)
	(apply append
	       (map (lambda (pair) (rz-spec-targets (car pair) (cdr pair)))
		    manifest)))

(define (rz-lookup table name) (assoc name table))

;; The internal library dependencies named by NAME's link clause. Each link
;; entry must be a defined target or a known system library; anything else is a
;; typo we refuse to paper over. System libraries are dropped (they add no
;; include dirs and no graph edge); defined targets are kept as edges.
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

;; ---------------------------------------------------------------------------
;; Link closure. Depth-first walk of the link graph from a set of roots,
;; returning the transitive internal libraries dependents-first (a library
;; appears before the libraries it depends on — the order a single-pass static
;; linker needs). A back-edge onto the active DFS path is a cycle and errors.
;; ---------------------------------------------------------------------------

(define (rz-closure table roots)
	(let ((state '())   ; name -> 'active | 'done  (latest cons wins)
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

;; ---------------------------------------------------------------------------
;; Public API.
;; ---------------------------------------------------------------------------

;; The link libraries (internal only) a target must link, dependents-first,
;; transitively. Roots are the target's own direct internal deps, so the target
;; itself is not in the result.
(define (resolve-link-libs table name)
	(rz-closure table (rz-direct-deps table name)))

;; The system libraries (-l flags) a target must link. A static library's own
;; system-lib dependency (asset_plugin links m) has to reach the final
;; executable, exactly as CMake propagates a PRIVATE link requirement through
;; the static archive, so gather them across the whole link closure.
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

;; The include directories a target's own sources compile against: its own
;; PUBLIC then PRIVATE dirs, then every transitively linked library's PUBLIC
;; dirs, de-duplicated. Errors (cycle / unknown target) surface here.
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

;; Force resolution of every target — the cheap way to make cycles and unknown
;; link targets fail loudly for the whole manifest, not just the targets an
;; emitter happens to touch.
(define (resolve-check-all table)
	(for-each (lambda (target) (resolve-includes table (car target)))
		  table)
	#t)
