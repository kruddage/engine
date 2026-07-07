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

;; Changelog embed: bake the file at IN into a NUL-terminated char array under
;; SYMBOL, each byte written as a 0xNN element (so non-ASCII bytes and quotes
;; survive and md_parse()/script_eval() get a valid string), and write the
;; header to OUT.
;;
;; A byte array rather than a string literal on purpose: C99 only guarantees
;; 4095 chars per string literal and gcc -Wpedantic -Werror enforces it (via
;; -Woverlength-strings, which counts the concatenated length, so splitting
;; into adjacent literals does not help). An array initializer carries no such
;; limit. Each element is written as (char)0xNN: char is signed on native gcc,
;; so a byte over 127 (a UTF-8 em-dash, say) would trip -Werror=overflow
;; without the explicit narrowing cast — clang for WASM has unsigned char and
;; never hit it, but md_parse.scm is compiled natively for the Scheme port.
(define (krudd-embed-file in out symbol)
	(let* ((bytes (krudd-slurp in))
	       (n     (string-length bytes))
	       (body (let loop ((i 0) (acc ""))
		       (if (>= i n)
			   acc
			   (loop (+ i 1)
				 (string-append acc
				   "(char)0x" (krudd-hex-byte
						(char->integer
						  (string-ref bytes i)))
				   ","
				   (if (= 0 (modulo (+ i 1) 12)) "\n\t" "")))))))
		(call-with-output-file out
		  (lambda (port)
		    (write-string
		      (string-append
			"/* SPDX-License-Identifier: GPL-2.0-or-later */\n"
			"/* Generated by krudd. Do not edit. */\n"
			"#ifndef " symbol "_H\n"
			"#define " symbol "_H\n\n"
			"static const char " symbol "[] = {\n\t"
			body "(char)0x00\n};\n\n"
			"#endif /* " symbol "_H */\n")
		      port)))))

;; md_parse ABI header: krudd/build/modules/md_parse.scm owns the markdown
;; parser and, with it, the numeric ABI the C side binds to. Rather than repeat
;; those constants in md_parse.h by hand — where they could drift from the
;; Scheme port that now defines them — krudd reads them out of the module and
;; emits them as C macros. This is the first C-binds-generated-Scheme seam;
;; every future port that keeps a C ABI can reuse it.
;;
;; MAP pairs each C macro with the Scheme symbol that supplies its value. The
;; Scheme file stays the single source of truth for the numbers.
(define md-parse-abi-map
	'(("MD_TEXT_MAX"        . md-text-max)
	  ("MD_SPANS_PER_BLOCK" . md-spans-per-block)
	  ("MD_BLOCKS_MAX"      . md-blocks-max)
	  ("MD_BLOCK_PARAGRAPH" . md-block-paragraph)
	  ("MD_BLOCK_HEADING"   . md-block-heading)
	  ("MD_BLOCK_LIST_ITEM" . md-block-list-item)
	  ("MD_BLOCK_CODE"      . md-block-code)
	  ("MD_SPAN_NORMAL"     . md-span-normal)
	  ("MD_SPAN_BOLD"       . md-span-bold)
	  ("MD_SPAN_CODE"       . md-span-code)))

;; Every top-level (define <symbol> <integer>) in the file at PATH, as an alist
;; of (symbol . value). The forms are read, not evaluated: the constants are
;; plain integer literals, so no parser code runs at build time.
(define (krudd-scm-int-defs path)
	(call-with-input-file path
	  (lambda (port)
	    (let loop ((acc '()))
	      (let ((form (read port)))
		(if (eof-object? form)
		    (reverse acc)
		    (loop (if (and (pair? form)
				   (eq? (car form) 'define)
				   (pair? (cdr form))
				   (symbol? (cadr form))
				   (pair? (cddr form))
				   (integer? (caddr form)))
			      (cons (cons (cadr form) (caddr form)) acc)
			      acc))))))))

;; Generate the md_parse ABI header at OUT from the Scheme module at IN: one
;; #define per md-parse-abi-map entry, its value taken from IN. A macro whose
;; Scheme symbol is missing from IN is a build error — the C ABI and the Scheme
;; port are required to agree.
(define (krudd-embed-abi-header in out)
	(let ((defs (krudd-scm-int-defs in)))
	  (call-with-output-file out
	    (lambda (port)
	      (write-string
		(string-append
		  "/* SPDX-License-Identifier: GPL-2.0-or-later */\n"
		  "/* Generated by krudd from krudd/build/modules/md_parse.scm."
		  " Do not edit. */\n"
		  "#ifndef MD_PARSE_ABI_H\n"
		  "#define MD_PARSE_ABI_H\n\n")
		port)
	      (for-each
		(lambda (entry)
		  (let ((macro (car entry))
			(cell  (assq (cdr entry) defs)))
		    (if (not cell)
			(error 'krudd-md-abi-missing (cdr entry))
			(write-string
			  (string-append "#define " macro " "
					 (number->string (cdr cell)) "\n")
			  port))))
		md-parse-abi-map)
	      (write-string "\n#endif /* MD_PARSE_ABI_H */\n" port)))))
