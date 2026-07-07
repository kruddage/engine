; SPDX-License-Identifier: GPL-2.0-or-later
;
; introspect_test.scm — native s7-only checks for the de-verbatimed root
; bootstrap (#341): the introspect helpers and the new cmake.scm root forms.
;
; Run via krudd/run-tests.sh. Prints "INTROSPECT-TESTS: OK" and exits 0 when
; every check passes; prints failures and exits 1 otherwise.
;
; It renders the root spec for both a native and an Emscripten configure without
; any network I/O: the imgui fetch dir is pre-created so krudd-fetch treats it as
; already present and skips the clone.

(define krudd-root (or (getenv "KRUDD_ROOT") "."))

;; cmake.scm loads introspect.scm; loading it here gives us both.
(define *configure* "cmake -S krudd/cmake -B build")
(load (string-append krudd-root "/krudd/cmake/cmake.scm"))

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

;; ---------------------------------------------------------------------------
;; Introspect helpers.
;; ---------------------------------------------------------------------------

(display "introspect: helpers\n")
(check "strip trims whitespace/newlines"
       (string=? (krudd-strip "  6.3.2\n\t") "6.3.2"))
(check "contains? finds emcmake" (krudd-contains? "emcmake cmake" "emcmake"))
(check "contains? rejects absent" (not (krudd-contains? "cmake -S" "emcmake")))

(define version (krudd-version))
(check "version matches VERSION file"
       (string=? version
		 (krudd-strip (krudd-slurp (string-append krudd-root
							  "/VERSION")))))
(check "build number is numeric" (all-digits? (krudd-build-number version)))
(check "commit hash non-empty" (> (string-length (krudd-commit-hash)) 0))

;; *configure* seam distinguishes the two backends.
(check "native configure is not an emscripten build"
       (not (krudd-emscripten-build?)))
(set! *configure* "emcmake cmake -S krudd/cmake -B build")
(check "emcmake configure is an emscripten build"
       (krudd-emscripten-build?))
(set! *configure* "cmake -S krudd/cmake -B build")

;; ---------------------------------------------------------------------------
;; Root spec renders to no verbatim, correct bootstrap, guarded fetch.
;; ---------------------------------------------------------------------------

(display "root spec: de-verbatimed bootstrap\n")
(define root-spec
	(call-with-input-file
	  (string-append krudd-root "/krudd/cmake/CMakeLists.scm") read))

;; No (verbatim ...) form survives in the root spec data.
(check "root spec has no verbatim form"
       (not (memq 'verbatim (map (lambda (f) (car f)) root-spec))))

;; Pre-create the fetch dir so rendering the WASM variant does no network I/O.
(system (string-append "mkdir -p " (krudd-fetch-dir "imgui")))

(define (render cfg)
	(set! *configure* cfg)
	(cmake-synthesize "root" root-spec))

(define native (render "cmake -S krudd/cmake -B build"))
(define wasm   (render "emcmake cmake -S krudd/cmake -B build"))

(check "renders cmake_minimum_required"
       (has? native "cmake_minimum_required(VERSION 3.20)"))
(check "renders project with literal version"
       (has? native (string-append "project(krudd VERSION " version
				   " LANGUAGES C CXX)")))
(check "renders ENGINE_BUILD_NUMBER set"
       (has? native "set(ENGINE_BUILD_NUMBER "))
(check "renders GIT_COMMIT_HASH set"
       (has? native "set(GIT_COMMIT_HASH \""))
(check "keeps KRUDD_REPO_ROOT for leaf bootstrap"
       (has? native "get_filename_component(KRUDD_REPO_ROOT"))
(check "no literal (verbatim escape hatch in output"
       (not (has? native "execute_process")))

(check "native: fetches no imgui" (not (has? native "imgui_SOURCE_DIR")))
(check "wasm: guards imgui on EMSCRIPTEN" (has? wasm "if(EMSCRIPTEN)"))
(check "wasm: sets imgui_SOURCE_DIR" (has? wasm "set(imgui_SOURCE_DIR"))

(if (= fail-count 0)
    (begin (display "INTROSPECT-TESTS: OK\n") (exit 0))
    (begin (display (string-append "INTROSPECT-TESTS: FAIL ("
				   (number->string fail-count) ")\n"))
	   (exit 1)))
