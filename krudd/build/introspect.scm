; SPDX-License-Identifier: GPL-2.0-or-later
;
; introspect.scm — the build-time introspection krudd owns.
;
; The root build spec used to hand CMake a block of imperative bootstrap
; verbatim: read the VERSION file, run git to derive the build number and commit
; hash, and FetchContent-clone imgui. Those are things CMake did for us at
; configure time; the Ninja backend has no execute_process or FetchContent, so
; krudd has to own them. This module is that ownership — plain Scheme that shells
; out (via s7's system for captured output, and the krudd `run` primitive for
; side-effecting commands) so both backends can bake the same values into their
; generated build files.
;
; It depends only on s7 built-ins plus, for the fetch path, the `run` primitive
; krudd.c installs — and `run` is touched only when a dependency actually needs
; cloning, so the version/git helpers work under a bare s7 too.

;; ---------------------------------------------------------------------------
;; String + file helpers (kept local, as the other krudd Scheme modules do).
;; ---------------------------------------------------------------------------

(define (krudd-whitespace? c)
	(or (char=? c #\space) (char=? c #\tab)
	    (char=? c #\newline) (char=? c #\return)))

;; Trim leading and trailing whitespace — the moral equivalent of CMake's
;; string(STRIP), for the newline git and file reads leave behind.
(define (krudd-strip s)
	(let* ((n (string-length s))
	       (start (let loop ((i 0))
			(if (and (< i n) (krudd-whitespace? (string-ref s i)))
			    (loop (+ i 1)) i)))
	       (end (let loop ((i n))
		      (if (and (> i start)
			       (krudd-whitespace? (string-ref s (- i 1))))
			  (loop (- i 1)) i))))
		(if (>= start end) "" (substring s start end))))

;; Slurp a whole file to a string (VERSION is a single short line, but keep it
;; general).
(define (krudd-slurp path)
	(call-with-input-file path
	  (lambda (port)
	    (let loop ((cs '()) (c (read-char port)))
	      (if (eof-object? c)
		  (list->string (reverse cs))
		  (loop (cons c cs) (read-char port)))))))

(define (krudd-path-exists? p)
	(if (defined? 'file-exists?)
	    (file-exists? p)
	    (= 0 (system (string-append "test -e \"" p "\"")))))

;; ---------------------------------------------------------------------------
;; Repository facts.
;; ---------------------------------------------------------------------------

(define (krudd-repo-root) (or (getenv "KRUDD_ROOT") "."))

;; The version string, read and stripped from the repo-root VERSION file.
(define (krudd-version)
	(krudd-strip (krudd-slurp (string-append (krudd-repo-root) "/VERSION"))))

;; Run git in the repo root and return its captured, stripped stdout ("" on
;; failure — git absent, not a repo, empty result), mirroring how CMake fell
;; back to defaults when GIT_FOUND was false or a command produced nothing.
(define (krudd-git args)
	(krudd-strip
	  (system (string-append "cd \"" (krudd-repo-root) "\" && git " args
				 " 2>/dev/null")
		  #t)))

;; The build number: commits since the current version was introduced, found by
;; the same VERSION-anchor walk the CMake bootstrap did. Defaults to "0".
(define (krudd-build-number version)
	(let ((anchor (krudd-git
			(string-append "log -1 --format=%H -S \"" version
				       "\" -- VERSION"))))
		(if (> (string-length anchor) 0)
		    (let ((count (krudd-git
				   (string-append "rev-list --count HEAD ^"
						  anchor))))
		      (if (> (string-length count) 0) count "0"))
		    "0")))

;; The short commit hash, or "unknown" when git can't answer.
(define (krudd-commit-hash)
	(let ((h (krudd-git "rev-parse --short HEAD")))
		(if (> (string-length h) 0) h "unknown")))

;; ---------------------------------------------------------------------------
;; Dependency fetch: krudd's replacement for FetchContent. Clone REPO at the
;; pinned TAG into build/_deps/<name> if it is not already there, and return the
;; directory (the ${<name>_SOURCE_DIR} the specs reference). Idempotent: a
;; complete checkout is left untouched, so repeat builds do no network I/O.
;; ---------------------------------------------------------------------------

(define (krudd-fetch-dir name)
	(string-append (krudd-repo-root) "/build/_deps/" name))

;; A checkout counts as present only if its .git is there — an empty or
;; half-cloned directory (an interrupted clone, or a stray mkdir) must not pass
;; for a real one, or the build inherits a broken dependency.
(define (krudd-fetch name repo tag)
	(let ((dir (krudd-fetch-dir name)))
		(if (not (krudd-path-exists? (string-append dir "/.git")))
		    (run (string-append "rm -rf \"" dir "\" && git clone --depth 1 "
					"--branch " tag " " repo " \"" dir "\"")))
		dir))

;; ---------------------------------------------------------------------------
;; Codegen: krudd's replacements for CMake's configure_file and its changelog
;; embed script — the file transforms the build needs generated before
;; compiling. The emitter calls these at synthesis time (as CMake ran them at
;; configure time) and references the outputs.
;; ---------------------------------------------------------------------------

;; Split "6.3.2" into ("6" "3" "2").
(define (krudd-split s ch)
	(let loop ((i 0) (start 0) (out '()))
		(cond ((>= i (string-length s))
		       (reverse (cons (substring s start i) out)))
		      ((char=? (string-ref s i) ch)
		       (loop (+ i 1) (+ i 1) (cons (substring s start i) out)))
		      (else (loop (+ i 1) start out)))))

;; The @VAR@ substitutions the version templates use, mirroring the CMake
;; variables project() and git-build-info supply.
(define (krudd-template-values)
	(let* ((version (krudd-version))
	       (parts   (krudd-split version #\.)))
		(list (cons "PROJECT_NAME" "krudd")
		      (cons "PROJECT_VERSION" version)
		      (cons "PROJECT_VERSION_MAJOR" (list-ref parts 0))
		      (cons "PROJECT_VERSION_MINOR" (list-ref parts 1))
		      (cons "PROJECT_VERSION_PATCH" (list-ref parts 2))
		      (cons "ENGINE_BUILD_NUMBER" (krudd-build-number version))
		      (cons "GIT_COMMIT_HASH" (krudd-commit-hash)))))

;; Replace every occurrence of OLD in S with NEW.
(define (krudd-replace s old new)
	(let ((ol (string-length old)))
		(let loop ((i 0) (out ""))
		  (cond ((> (+ i ol) (string-length s))
			 (string-append out (substring s i)))
			((string=? (substring s i (+ i ol)) old)
			 (loop (+ i ol) (string-append out new)))
			(else (loop (+ i 1)
				    (string-append out
						   (string (string-ref s i)))))))))

;; configure_file: expand @VAR@ occurrences in the template at IN and write the
;; result to OUT. (@ONLY semantics — only @VAR@, never ${VAR} — which is all the
;; version.h.in / shell.html.in templates use.)
(define (krudd-configure-file in out)
	(let loop ((text (krudd-slurp in)) (vals (krudd-template-values)))
		(if (null? vals)
		    (call-with-output-file out
		      (lambda (port) (write-string text port)))
		    (loop (krudd-replace text (string-append "@" (caar vals) "@")
					 (cdar vals))
			  (cdr vals)))))

;; Two hex nibbles for a byte value 0..255.
(define (krudd-hex-byte b)
	(let ((digits "0123456789abcdef"))
		(string (string-ref digits (quotient b 16))
			(string-ref digits (remainder b 16)))))

;; Changelog embed: bake the file at IN into a NUL-terminated C string
;; literal under SYMBOL, each byte written as a \xNN escape (the same hex-escape
;; scheme, so non-ASCII bytes and quotes survive and md_parse() gets a valid
;; string), and write the header to OUT.
(define (krudd-embed-file in out symbol)
	(let* ((bytes (krudd-slurp in))
	       (body (let loop ((i 0) (acc ""))
		       (if (>= i (string-length bytes))
			   acc
			   (loop (+ i 1)
				 (string-append acc "\\x"
				   (krudd-hex-byte (char->integer
						    (string-ref bytes i)))))))))
		(call-with-output-file out
		  (lambda (port)
		    (write-string
		      (string-append
			"/* SPDX-License-Identifier: GPL-2.0-or-later */\n"
			"/* Generated by krudd. Do not edit. */\n"
			"#ifndef " symbol "_H\n"
			"#define " symbol "_H\n\n"
			"static const char " symbol "[] =\n\t\"" body "\";\n\n"
			"#endif /* " symbol "_H */\n")
		      port)))))
