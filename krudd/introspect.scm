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

(define (krudd-contains? hay needle)
	(let ((hl (string-length hay)) (nl (string-length needle)))
		(let loop ((i 0))
		  (cond ((> (+ i nl) hl) #f)
			((string=? (substring hay i (+ i nl)) needle) #t)
			(else (loop (+ i 1)))))))

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

;; Whether this synthesis is driving an Emscripten build. The configure command
;; krudd was handed (`*configure*`) carries `emcmake` for WASM and plain cmake
;; for native, so it is the seam that tells the two apart at generation time.
(define (krudd-emscripten-build?)
	(and (defined? '*configure*)
	     (string? *configure*)
	     (krudd-contains? *configure* "emcmake")))

;; ---------------------------------------------------------------------------
;; Dependency fetch: krudd's replacement for FetchContent. Clone REPO at the
;; pinned TAG into build/_deps/<name> if it is not already there, and return the
;; directory (the ${<name>_SOURCE_DIR} the specs reference). Idempotent: an
;; existing checkout is left untouched, so repeat builds do no network I/O.
;; ---------------------------------------------------------------------------

(define (krudd-fetch-dir name)
	(string-append (krudd-repo-root) "/build/_deps/" name))

(define (krudd-fetch name repo tag)
	(let ((dir (krudd-fetch-dir name)))
		(if (not (krudd-path-exists? dir))
		    (run (string-append "git clone --depth 1 --branch " tag
					" " repo " \"" dir "\"")))
		dir))
