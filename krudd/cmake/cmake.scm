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
; A SRC/INC is a bare string (relative to this directory), (root "path") which
; expands to ${CMAKE_SOURCE_DIR}/path for references reaching across the tree,
; (current)/(current "path") which name this directory as
; ${CMAKE_CURRENT_SOURCE_DIR} explicitly, or (raw "text") which passes text
; through verbatim — for CMake variables the spec doesn't own, such as
; ${imgui_SOURCE_DIR} from FetchContent.

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

;; Render one top-level form to a list of lines.
(define (cmake-emit-form form)
  (case (car form)
    ((library)           (cmake-emit-library form))
    ((interface-library) (cmake-emit-interface-library form))
    ((executable)        (cmake-emit-executable form))
    ((test)              (cmake-emit-test form))
    ((side-module)       (cmake-emit-side-module form))
    ((native-only)       (cmake-emit-native-only form))
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

;; Synthesize a directory spec to CMakeLists.txt text. Top-level forms are
;; separated by a blank line; the trailing newline keeps the file POSIX-clean.
(define (cmake-synthesize source spec)
  (string-append
    (cmake-header source)
    (cmake-join "\n\n"
		(map (lambda (form) (cmake-join "\n" (cmake-emit-form form)))
		     spec))
    "\n"))
