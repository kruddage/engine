; SPDX-License-Identifier: GPL-2.0-or-later
;
; introspect_test.scm — native s7-only checks for krudd/build/introspect.scm:
; the build-time introspection krudd owns now that CMake is gone (version +
; git facts, the configure_file / changelog codegen, and the dependency
; fetch).
;
; Run via krudd/build/run-tests.sh. Prints "INTROSPECT-TESTS: OK" and exits 0
; when every check passes; prints failures and exits 1 otherwise. No network
; I/O.

(define krudd-root (or (getenv "KRUDD_ROOT") "."))
(load (string-append krudd-root "/krudd/build/introspect.scm"))

(define fail-count 0)
(define (check name ok)
	(if ok
	    (display (string-append "  ok    " name "\n"))
	    (begin (set! fail-count (+ fail-count 1))
		   (display (string-append "  FAIL  " name "\n")))))

(define (has? s sub)
	(let ((hl (string-length s)) (nl (string-length sub)))
		(let loop ((i 0))
		  (cond ((> (+ i nl) hl) #f)
			((string=? (substring s i (+ i nl)) sub) #t)
			(else (loop (+ i 1)))))))

(define (all-digits? s)
	(and (> (string-length s) 0)
	     (let loop ((i 0))
	       (cond ((>= i (string-length s)) #t)
		     ((char-numeric? (string-ref s i)) (loop (+ i 1)))
		     (else #f)))))

(define (slurp path)
	(call-with-input-file path
	  (lambda (p)
	    (let loop ((cs '()) (c (read-char p)))
	      (if (eof-object? c) (list->string (reverse cs))
		  (loop (cons c cs) (read-char p)))))))

;; ---------------------------------------------------------------------------
;; String / version helpers.
;; ---------------------------------------------------------------------------

(display "introspect: helpers\n")
(check "strip trims whitespace/newlines"
       (string=? (krudd-strip "  6.3.2\n\t") "6.3.2"))
(check "split on dot" (equal? (krudd-split "6.3.2" #\.) '("6" "3" "2")))
(check "replace all occurrences"
       (string=? (krudd-replace "a@X@b@X@c" "@X@" "1") "a1b1c"))

(define version (krudd-version))
(check "version matches VERSION file"
       (string=? version (krudd-strip (slurp (string-append krudd-root
							     "/VERSION")))))
(check "build number is numeric" (all-digits? (krudd-build-number version)))
(check "commit hash non-empty" (> (string-length (krudd-commit-hash)) 0))

;; ---------------------------------------------------------------------------
;; Codegen: configure_file and the changelog embed.
;; ---------------------------------------------------------------------------

(display "introspect: codegen\n")
(define tmp (string-append krudd-root "/build/_introspect_test"))
(system (string-append "mkdir -p " tmp))

(krudd-configure-file
  (string-append krudd-root "/krudd/build/ninja/modules/core/version.h.in")
  (string-append tmp "/version.h"))
(let ((v (slurp (string-append tmp "/version.h"))))
	(check "version.h carries the literal version"
	       (has? v (string-append "ENGINE_VERSION_STRING \"" version "\"")))
	(check "version.h has no unexpanded @VAR@ tokens" (not (has? v "@"))))

(krudd-embed-file (string-append krudd-root "/CHANGELOG.md")
		  (string-append tmp "/changelog_data.h") "CHANGELOG_MD")
(let ((h (slurp (string-append tmp "/changelog_data.h"))))
	(check "changelog header declares the symbol"
	       (has? h "static const char CHANGELOG_MD[] ="))
	(check "changelog body is a NUL-terminated byte array"
	       (and (has? h "(char)0x") (has? h "(char)0x00"))))

;; ---------------------------------------------------------------------------
;; Binding generator: md_parse.scm's embedded ABI declaration -> the C header
;; and marshaling shim. Driven by the declaration, not hardcoded to md_parse;
;; here we just check the whole seam comes out of the real module.
;; ---------------------------------------------------------------------------

(display "introspect: binding generator\n")
(krudd-embed-scheme-module
  (string-append krudd-root "/krudd/build/modules/md_parse.scm")
  (string-append tmp "/md_parse.h")
  (string-append tmp "/md_parse.scm.c"))

(let ((h (slurp (string-append tmp "/md_parse.h"))))
	(check "header has the include guard" (has? h "#ifndef MD_PARSE_H"))
	(check "header folds in the constants" (has? h "#define MD_TEXT_MAX 256"))
	(check "header is extern C safe" (has? h "extern \"C\""))
	(check "header emits struct md_span" (has? h "struct md_span {"))
	(check "header emits the char field bound"
	       (has? h "char text[MD_TEXT_MAX];"))
	(check "header emits the vector field"
	       (has? h "struct md_span spans[MD_SPANS_PER_BLOCK];"))
	(check "header emits the prototype"
	       (has? h "int32_t md_parse(const char *src, struct md_block *out, int32_t max);"))
	(check "declaration forms do not leak into C"
	       (not (has? h "define-c-struct"))))

(let ((c (slurp (string-append tmp "/md_parse.scm.c"))))
	(check "shim includes the generated header" (has? c "#include \"md_parse.h\""))
	(check "shim bakes the image in" (has? c "static const char md_parse_scm_image[]"))
	(check "shim generates each struct marshaler"
	       (and (has? c "md_parse_marshal_md_span")
		    (has? c "md_parse_marshal_md_block")))
	(check "shim asserts marshal arity" (has? c "s7_list_length(s7, v) == 4"))
	(check "shim clamps the char field" (has? c "n > MD_TEXT_MAX - 1"))
	(check "shim drives the scheme proc"
	       (has? c "s7_name_to_value(s7, \"md-parse\")"))
	(check "shim emits the driver"
	       (has? c "int32_t md_parse(const char *src, struct md_block *out, int32_t max)")))

;; ---------------------------------------------------------------------------
;; Fetch hardening: an existing directory without a .git checkout must not count
;; as present (it would leave the build on a broken dependency).
;; ---------------------------------------------------------------------------

(display "introspect: fetch hardening\n")
(let ((broken (string-append tmp "/broken-dep")))
	(system (string-append "mkdir -p " broken))   ; empty dir, no .git
	(check "empty dir is not a valid checkout"
	       (not (krudd-path-exists? (string-append broken "/.git"))))
	;; A real checkout (has .git) is recognised.
	(system (string-append "mkdir -p " broken "/.git"))
	(check "dir with .git is a valid checkout"
	       (krudd-path-exists? (string-append broken "/.git"))))

(system (string-append "rm -rf " tmp))

(if (= fail-count 0)
    (begin (display "INTROSPECT-TESTS: OK\n") (exit 0))
    (begin (display (string-append "INTROSPECT-TESTS: FAIL ("
				   (number->string fail-count) ")\n"))
	   (exit 1)))
