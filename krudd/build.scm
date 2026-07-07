; SPDX-License-Identifier: GPL-2.0-or-later
;
; krudd build description — the build authority, in Scheme.
;
; krudd (see ../krudd.c) provides:
;   (run cmd)     -> run a shell command, return its integer exit status
;   *configure*   -> the configure command string (cmake / emcmake cmake ...)
;   *build*       -> the build command string
;
; The strangler fig, phase 2: before we hand off to CMake we *synthesize* the
; CMakeLists.txt for the directories krudd has taken ownership of, from the
; specs below. CMake is now a backend we emit for, not the source of truth.
; Each directory we move behind a spec is one more root strangling the vine;
; the rest still ships its hand-written CMakeLists.txt until its turn comes.

(define krudd-root (or (getenv "KRUDD_ROOT") "."))

(load (string-append krudd-root "/krudd/cmake.scm"))

(define (sh cmd)
  (let ((status (run cmd)))
    (if (not (= status 0))
	(error 'krudd-build-failed cmd))))

(define (write-file path text)
  (call-with-output-file path
    (lambda (port) (write-string text port))))

;; ---------------------------------------------------------------------------
;; Specs. Each entry is (cmake-relative-dir . directory-spec). The path is
;; relative to cmake/, matching the add_subdirectory() layout in the root
;; CMakeLists.txt. Grow this list to strangle another directory.
;; ---------------------------------------------------------------------------

(define owned-directories
  (list
    (cons "modules/log"
	  '((library "log"
	      (sources "log.c" (root "modules/core/ring_buf.c"))
	      (public "include" (root "plugins/include"))
	      (private (root "modules/core/include")))
	    (native-only
	      (executable "log_test"
		(sources "log_test.c")
		(link "log"))
	      (test "log" "log_test"))))))

;; Synthesize every owned directory's CMakeLists.txt, then let CMake build.
(define (synthesize-owned)
  (for-each
    (lambda (entry)
      (let ((path (string-append krudd-root "/cmake/" (car entry)
				 "/CMakeLists.txt")))
	(display (string-append "krudd: synthesize " path "\n"))
	(write-file path (cmake-synthesize (cdr entry)))))
    owned-directories))

(synthesize-owned)
(sh *configure*)
(sh *build*)
