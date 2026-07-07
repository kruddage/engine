; SPDX-License-Identifier: GPL-2.0-or-later
;
; cmake.scm — the CMake synthesizer.
;
; This is the first real organ of the strangler fig: instead of hand-writing
; CMakeLists.txt, we describe a directory's targets as Scheme data and render
; the CMake text from that description. krudd owns the spec; CMake is demoted to
; a backend we emit for. Over time more directories move behind the spec, and
; the day CMake itself is replaced, only this emitter changes — the specs stay.
;
; A directory spec is a list of target forms:
;
;   (library NAME clause...)            -> add_library(NAME ...)
;   (interface-library NAME clause...)  -> add_library(NAME INTERFACE ...)
;   (executable NAME clause...)         -> add_executable(NAME ...)
;   (test NAME COMMAND)                 -> add_test(NAME NAME COMMAND COMMAND)
;   (side-module NAME clause...)        -> if(EMSCRIPTEN) add_custom_command ...
;   (native-only form...)               -> if(NOT EMSCRIPTEN) ... endif()
;
; Clauses shared by library/executable:
;
;   (sources SRC...)              source files
;   (public INC...)               PUBLIC include directories
;   (private INC...)              PRIVATE include directories
;   (link LIB...)                 PRIVATE link libraries
;
; interface-library takes (interface INC...); side-module clauses are documented
; at cmake-emit-side-module below.
;
; The root directory is not targets but project scaffolding, so it adds a few
; whole-file forms:
;
;   (set VAR VALUE)              -> set(VAR VALUE)
;   (compile-options FLAG...)    -> add_compile_options(FLAG ...)
;   (link-options FLAG...)       -> add_link_options(FLAG ...)
;   (enable-testing)             -> enable_testing()
;   (option NAME "DOC" DEF b...) -> option(...) + if(NAME) b... endif()
;   (subdirs PATH...)            -> one add_subdirectory(PATH) per line
;   (cmake-minimum VER)          -> cmake_minimum_required(VERSION VER)
;   (repo-root VAR)              -> get_filename_component(VAR ".../../.." ABSOLUTE)
;   (project NAME VFILE LANG...) -> project(NAME VERSION <VFILE contents> LANGUAGES ...)
;   (git-build-info)             -> set(ENGINE_BUILD_NUMBER n) + set(GIT_COMMIT_HASH h)
;   (fetch-content NAME REPO TAG GUARD)
;                                -> if(GUARD) set(NAME_SOURCE_DIR <clone>) endif()
;   (verbatim "text")            -> text emitted unchanged (the escape hatch, now
;                                   only for the leaf bootstrap krudd hasn't
;                                   strangled yet — modules/core's configure_file
;                                   and kruddboard's changelog embed)
;
; The project/git/fetch forms are the de-verbatimed root bootstrap (#341): they
; do their work at synthesis time through krudd/introspect.scm — reading VERSION,
; running git, cloning the pinned dependency — and bake the results in, rather
; than emitting CMake's file(READ)/execute_process/FetchContent for the backend
; to run later.
;
; A SRC/INC is a bare string (relative to this directory), (root "path") which
; expands to ${CMAKE_SOURCE_DIR}/path for references reaching across the tree,
; (current)/(current "path") which name this directory as
; ${CMAKE_CURRENT_SOURCE_DIR} explicitly, or (raw "text") which passes text
; through verbatim — for CMake variables the spec doesn't own, such as
; ${imgui_SOURCE_DIR} from FetchContent.

(load (string-append (or (getenv "KRUDD_ROOT") ".") "/krudd/introspect.scm"))

;; string list join — s7 has no string-join we can rely on.
(define (cmake-join sep lst)
  (cond ((null? lst) "")
	((null? (cdr lst)) (car lst))
	(else (string-append (car lst) sep (cmake-join sep (cdr lst))))))

;; Expand one source/include path spec to its CMake string. A bare string is
;; relative to this directory; (root "p") reaches from the tree root; (current)
;; / (current "p") name this directory explicitly, as the side-module custom
;; commands must (they spell out ${CMAKE_CURRENT_SOURCE_DIR} rather than letting
;; CMake resolve a relative source).
(define (cmake-path p)
  (cond ((string? p) p)
	((and (pair? p) (eq? (car p) 'root))
	 (string-append "${CMAKE_SOURCE_DIR}/" (cadr p)))
	((and (pair? p) (eq? (car p) 'current))
	 (if (null? (cdr p))
	     "${CMAKE_CURRENT_SOURCE_DIR}"
	     (string-append "${CMAKE_CURRENT_SOURCE_DIR}/" (cadr p))))
	((and (pair? p) (eq? (car p) 'raw)) (cadr p))
	(else (error 'cmake-bad-path p))))

;; Find the clause whose head is HEAD, or #f.
(define (cmake-clause head clauses)
  (cond ((null? clauses) #f)
	((eq? (caar clauses) head) (car clauses))
	(else (cmake-clause head (cdr clauses)))))

;; Render "MACRO(NAME src src ...)". One source stays on a single line; several
;; break one-per-line with the close paren on its own line, matching house style.
(define (cmake-target-decl macro name srcs)
  (let ((paths (map cmake-path srcs)))
    (cond
      ((null? paths)
       (list (string-append macro "(" name ")")))
      ((null? (cdr paths))
       (list (string-append macro "(" name " " (car paths) ")")))
      (else
       (append
	 (list (string-append macro "(" name))
	 (map (lambda (p) (string-append "\t" p)) paths)
	 (list ")"))))))

;; target_include_directories(NAME VIS a b c) — nothing if the clause is absent.
(define (cmake-includes name clause vis)
  (if clause
      (list (string-append "target_include_directories(" name " " vis " "
			   (cmake-join " " (map cmake-path (cdr clause))) ")"))
      '()))

(define (cmake-emit-library form)
  (let* ((name    (cadr form))
	 (clauses (cddr form))
	 (sources (cmake-clause 'sources clauses))
	 (link    (cmake-clause 'link clauses)))
    (append
      (cmake-target-decl "add_library" name
			 (if sources (cdr sources) '()))
      (cmake-includes name (cmake-clause 'public clauses) "PUBLIC")
      (cmake-includes name (cmake-clause 'private clauses) "PRIVATE")
      (if link
	  (list (string-append "target_link_libraries(" name " PRIVATE "
			       (cmake-join " " (cdr link)) ")"))
	  '()))))

;; An INTERFACE library carries no sources — only usage requirements. Renderer's
;; header-only seam is the one case.
(define (cmake-emit-interface-library form)
  (let* ((name (cadr form))
	 (inc  (cmake-clause 'interface (cddr form))))
    (list
      (string-append "add_library(" name " INTERFACE)")
      (string-append "target_include_directories(" name " INTERFACE "
		     (cmake-join " " (map cmake-path (if inc (cdr inc) '())))
		     ")"))))

(define (cmake-emit-executable form)
  (let* ((name    (cadr form))
	 (clauses (cddr form))
	 (sources (cmake-clause 'sources clauses))
	 (link    (cmake-clause 'link clauses)))
    (append
      (cmake-target-decl "add_executable" name
			 (if sources (cdr sources) '()))
      (cmake-includes name (cmake-clause 'public clauses) "PUBLIC")
      (cmake-includes name (cmake-clause 'private clauses) "PRIVATE")
      (if link
	  (list (string-append "target_link_libraries(" name " PRIVATE "
			       (cmake-join " " (cdr link)) ")"))
	  '()))))

(define (cmake-emit-test form)
  (list (string-append "add_test(NAME " (cadr form)
		       " COMMAND " (caddr form) ")")))

;; A plugin side module: one hand-rolled compiler invocation behind
;; if(EMSCRIPTEN), wrapped in a custom target the main module depends on. This
;; is the shape almost every plugin repeats — a -sSIDE_MODULE=1 -O2 build of a
;; handful of sources into ${CMAKE_BINARY_DIR}/NAME.wasm.
;;
;;   (side-module NAME clause...)
;;     (compiler c|cxx)   compiler var (default c)
;;     (target STR)       custom-target name (default NAME_wasm)
;;     (comment STR)      COMMENT text (default "Building NAME SIDE_MODULE")
;;     (flags STR...)     extra bare compiler flags, after -O2
;;     (includes P...)    -I paths, in order
;;     (sources P...)     compiled inputs, after -o
;;     (depends P...)     DEPENDS entries
(define (cmake-side-field clauses head deflt)
  (let ((c (cmake-clause head clauses)))
    (if c (cadr c) deflt)))

(define (cmake-emit-side-module form)
  (let* ((name     (cadr form))
	 (clauses  (cddr form))
	 (target   (cmake-side-field clauses 'target
				     (string-append name "_wasm")))
	 (comment  (cmake-side-field clauses 'comment
				     (string-append "Building " name
						    " SIDE_MODULE")))
	 (compiler (if (eq? (cmake-side-field clauses 'compiler 'c) 'cxx)
		       "${CMAKE_CXX_COMPILER}" "${CMAKE_C_COMPILER}"))
	 (flags    (let ((c (cmake-clause 'flags clauses)))
		     (if c (cdr c) '())))
	 (includes (let ((c (cmake-clause 'includes clauses)))
		     (if c (cdr c) '())))
	 (sources  (let ((c (cmake-clause 'sources clauses)))
		     (if c (cdr c) '())))
	 (depends  (let ((c (cmake-clause 'depends clauses)))
		     (if c (cdr c) '())))
	 (wasm     (string-append "${CMAKE_BINARY_DIR}/" name ".wasm")))
    (append
      (list
	"if(EMSCRIPTEN)"
	"\tadd_custom_command("
	(string-append "\t\tOUTPUT " wasm)
	(string-append "\t\tCOMMAND " compiler)
	"\t\t\t-sSIDE_MODULE=1"
	"\t\t\t-O2")
      (map (lambda (f) (string-append "\t\t\t" f)) flags)
      (map (lambda (i) (string-append "\t\t\t-I" (cmake-path i))) includes)
      (list (string-append "\t\t\t-o " wasm))
      (map (lambda (s) (string-append "\t\t\t" (cmake-path s))) sources)
      (if (and (pair? depends) (null? (cdr depends)))
	  (list (string-append "\t\tDEPENDS " (cmake-path (car depends))))
	  (cons "\t\tDEPENDS"
		(map (lambda (d) (string-append "\t\t\t" (cmake-path d)))
		     depends)))
      (list
	(string-append "\t\tCOMMENT \"" comment "\"")
	"\t)"
	(string-append "\tadd_custom_target(" target " ALL")
	(string-append "\t\tDEPENDS " wasm)
	"\t)"
	(string-append "\tadd_dependencies(index " target ")")
	"endif()"))))

;; ---------------------------------------------------------------------------
;; Whole-file forms. The root CMakeLists.txt describes the project, not a set of
;; targets: global flags, build options, the subdirectory layout, and a
;; bootstrap (project()/git introspection/FetchContent) we do not model — that
;; last part rides through (verbatim ...) verbatim.
;; ---------------------------------------------------------------------------

;; (verbatim "text") — the one escape hatch: emit the text unchanged, newlines
;; and all. Used for the CMake bootstrap krudd hasn't strangled yet.
(define (cmake-emit-verbatim form)
  (list (cadr form)))

;; (set VAR VALUE) -> set(VAR VALUE)
(define (cmake-emit-set form)
  (list (string-append "set(" (cadr form) " " (caddr form) ")")))

;; (compile-options FLAG...) -> add_compile_options(FLAG ...)
(define (cmake-emit-compile-options form)
  (list (string-append "add_compile_options("
		       (cmake-join " " (cdr form)) ")")))

;; (link-options FLAG...) -> add_link_options(FLAG ...)
(define (cmake-emit-link-options form)
  (list (string-append "add_link_options("
		       (cmake-join " " (cdr form)) ")")))

;; (enable-testing) -> enable_testing()
(define (cmake-emit-enable-testing form)
  (list "enable_testing()"))

;; (option NAME "DOC" DEFAULT body...) -> the option declaration followed by an
;; if(NAME) ... endif() carrying the body forms (compile-options/link-options in
;; practice), each indented one tab inside the guard.
(define (cmake-emit-option form)
  (let ((name  (list-ref form 1))
	(doc   (list-ref form 2))
	(deflt (list-ref form 3))
	(body  (list-tail form 4)))
    (append
      (list (string-append "option(" name " \"" doc "\" " deflt ")")
	    (string-append "if(" name ")"))
      (map (lambda (l) (string-append "\t" l))
	   (apply append (map cmake-emit-form body)))
      (list "endif()"))))

;; (subdirs PATH...) -> one add_subdirectory(PATH) per line. The order is the
;; traversal order CMake needs: a target must be defined before a later
;; subdirectory links against it.
(define (cmake-emit-subdirs form)
  (map (lambda (d) (string-append "add_subdirectory(" d ")")) (cdr form)))

;; ---------------------------------------------------------------------------
;; De-verbatimed root bootstrap (#341). These forms compute their values at
;; synthesis time via krudd/introspect.scm and emit the results, so the backend
;; never re-derives them — the Ninja backend, which has no execute_process or
;; FetchContent, can lean on the same introspection.
;; ---------------------------------------------------------------------------

;; (cmake-minimum VER) -> cmake_minimum_required(VERSION VER)
(define (cmake-emit-cmake-minimum form)
  (list (string-append "cmake_minimum_required(VERSION " (cadr form) ")")))

;; (repo-root VAR) -> the repo root, two levels up from krudd/cmake/. Kept as a
;; CMake variable because leaf bootstrap still verbatim-references it (kruddboard
;; embeds ${KRUDD_REPO_ROOT}/CHANGELOG.md).
(define (cmake-emit-repo-root form)
  (list (string-append "get_filename_component(" (cadr form)
		       " \"${CMAKE_CURRENT_SOURCE_DIR}/../..\" ABSOLUTE)")))

;; (project NAME VFILE LANG...) -> project(NAME VERSION <VFILE> LANGUAGES ...).
;; VFILE names the version file at the repo root; its contents are read and
;; stripped now, so the emitted project() carries a literal version.
(define (cmake-emit-project form)
  (let ((name  (list-ref form 1))
	(langs (list-tail form 3)))
    (list (string-append "project(" name " VERSION " (krudd-version)
			 " LANGUAGES " (cmake-join " " langs) ")"))))

;; (git-build-info) -> the ENGINE_BUILD_NUMBER / GIT_COMMIT_HASH the version
;; templates consume, derived from git now (defaults baked when git is absent).
(define (cmake-emit-git-build-info form)
  (let* ((version (krudd-version))
	 (number  (krudd-build-number version))
	 (hash    (krudd-commit-hash)))
    (list (string-append "set(ENGINE_BUILD_NUMBER " number ")")
	  (string-append "set(GIT_COMMIT_HASH \"" hash "\")"))))

;; (fetch-content NAME REPO TAG GUARD) -> krudd's FetchContent. Only when this
;; synthesis is driving a build that needs it (GUARD, e.g. EMSCRIPTEN) does it
;; clone REPO@TAG and emit the guarded set(NAME_SOURCE_DIR ...) the specs
;; reference; otherwise nothing is emitted and nothing is fetched.
(define (cmake-emit-fetch-content form)
  (let ((name  (list-ref form 1))
	(repo  (list-ref form 2))
	(tag   (list-ref form 3))
	(guard (list-ref form 4)))
    (if (krudd-emscripten-build?)
	(let ((dir (krudd-fetch name repo tag)))
	  (list (string-append "if(" guard ")")
		(string-append "\tset(" name "_SOURCE_DIR \"" dir "\")")
		"endif()"))
	'())))

;; Render one top-level form to a list of lines.
(define (cmake-emit-form form)
  (case (car form)
    ((library)           (cmake-emit-library form))
    ((interface-library) (cmake-emit-interface-library form))
    ((executable)        (cmake-emit-executable form))
    ((test)              (cmake-emit-test form))
    ((side-module)       (cmake-emit-side-module form))
    ((native-only)       (cmake-emit-native-only form))
    ((verbatim)          (cmake-emit-verbatim form))
    ((set)               (cmake-emit-set form))
    ((compile-options)   (cmake-emit-compile-options form))
    ((link-options)      (cmake-emit-link-options form))
    ((enable-testing)    (cmake-emit-enable-testing form))
    ((option)            (cmake-emit-option form))
    ((subdirs)           (cmake-emit-subdirs form))
    ((cmake-minimum)     (cmake-emit-cmake-minimum form))
    ((repo-root)         (cmake-emit-repo-root form))
    ((project)           (cmake-emit-project form))
    ((git-build-info)    (cmake-emit-git-build-info form))
    ((fetch-content)     (cmake-emit-fetch-content form))
    (else (error 'cmake-unknown-form form))))

(define (cmake-emit-native-only form)
  (let ((inner (apply append (map cmake-emit-form (cdr form)))))
    (append
      (list "if(NOT EMSCRIPTEN)")
      (map (lambda (l) (if (string=? l "") l (string-append "\t" l))) inner)
      (list "endif()"))))

;; SOURCE names the spec file this text was rendered from, so anyone who opens
;; a generated CMakeLists.txt can find the data behind it.
(define (cmake-header source)
  (string-append
    "# Generated by krudd — do not edit by hand.\n"
    "# Source of truth: " source ", rendered by krudd/cmake/cmake.scm.\n"
    "# Regenerate: ./krudd.sh build\n"
    "\n"))

;; Keep only non-empty strings — a form that renders to nothing (e.g. a
;; fetch-content that fetches nothing for a native build) must not leave a blank
;; block behind.
(define (cmake-nonempty lst)
  (cond ((null? lst) '())
	((string=? (car lst) "") (cmake-nonempty (cdr lst)))
	(else (cons (car lst) (cmake-nonempty (cdr lst))))))

;; Synthesize a directory spec to CMakeLists.txt text. Top-level forms are
;; separated by a blank line; the trailing newline keeps the file POSIX-clean.
(define (cmake-synthesize source spec)
  (string-append
    (cmake-header source)
    (cmake-join "\n\n"
		(cmake-nonempty
		  (map (lambda (form) (cmake-join "\n" (cmake-emit-form form)))
		       spec)))
    "\n"))
