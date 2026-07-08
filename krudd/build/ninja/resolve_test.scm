; SPDX-License-Identifier: GPL-2.0-or-later
;
; resolve_test.scm — native s7-only checks for the include/link resolver and the
; Ninja emitter. Runs without any WASM (or even C) toolchain: it exercises the
; Scheme passes over the real manifest and a few synthetic specs.
;
; Run via krudd/build/ninja/run-tests.sh. Prints "RESOLVE-TESTS: OK" and exits
; 0 when every check passes; prints failures and exits 1 otherwise.
;
; If KRUDD_NINJA_OUT is set, also renders the real manifest to a build.ninja at
; that path (with an absolute srcroot) so the harness can hand it to ninja(1).

(define krudd-root (or (getenv "KRUDD_ROOT") "."))

(load (string-append krudd-root "/krudd/build/ninja/ninja.scm"))

;; ---------------------------------------------------------------------------
;; Assertion plumbing.
;; ---------------------------------------------------------------------------

(define fail-count 0)

(define (check name ok)
	(if ok
	    (display (string-append "  ok    " name "\n"))
	    (begin
	      (set! fail-count (+ fail-count 1))
	      (display (string-append "  FAIL  " name "\n")))))

(define (subset? a b)
	(cond ((null? a) #t)
	      ((member (car a) b) (subset? (cdr a) b))
	      (else #f)))

(define (set=? a b) (and (subset? a b) (subset? b a)))

(define (expect-error thunk)
	(catch #t (lambda () (thunk) #f) (lambda args #t)))

(define (index-of x lst)
	(let loop ((l lst) (i 0))
		(cond ((null? l) -1)
		      ((equal? (car l) x) i)
		      (else (loop (cdr l) (+ i 1))))))

;; ---------------------------------------------------------------------------
;; The real manifest.
;; ---------------------------------------------------------------------------

(define (load-datum path) (call-with-input-file path read))

(define manifest-dirs
	(load-datum (string-append krudd-root "/krudd/build/ninja/manifest.scm")))

(define (load-spec dir)
	(load-datum (string-append krudd-root "/krudd/build/ninja/" dir
				   "/build.scm")))

(define manifest
	(map (lambda (d) (cons d (load-spec d))) manifest-dirs))

(define table (rz-target-table manifest))

;; ---------------------------------------------------------------------------
;; Include-set checks. Expected sets are the reference -I lists for these
;; targets (paths normalised relative to krudd/build/ninja/). The resolver must
;; produce the same *set*; order is not build-relevant.
;; ---------------------------------------------------------------------------

(define (inc-check name expected)
	(check (string-append "includes " name)
	       (set=? (resolve-includes table name) expected)))

(display "resolver: include sets vs CMake ground truth\n")
(inc-check "log" '("modules/log/include" "plugins/include"
		   "modules/core/include"))
(inc-check "log_test" '("modules/log/include" "plugins/include"))
(inc-check "renderer_null" '("plugins/renderer" "modules/log/include"
			     "plugins/include" "modules/core/include"))
(inc-check "renderer_null_test"
	   '("plugins/renderer_null" "plugins/renderer" "modules/log/include"
	     "plugins/include" "modules/core/include"))
(inc-check "fg_test" '("plugins/frame_graph" "plugins/renderer"
		       "plugins/renderer_null" "modules/log/include"
		       "plugins/include" "modules/memory/include"
		       "modules/core/include"))
(inc-check "asset_plugin" '("plugins/asset" "plugins/include"
			    "modules/log/include" "modules/memory/include"
			    "modules/core/include"))
(inc-check "scene_test" '("plugins/scene_plugin" "plugins/include"
			  "modules/core/include" "plugins/asset"
			  "modules/log/include" "modules/memory/include"))

;; ---------------------------------------------------------------------------
;; Link-closure checks: transitive membership and dependents-first ordering.
;; ---------------------------------------------------------------------------

(display "resolver: transitive link closures\n")
(let ((libs (resolve-link-libs table "renderer_null_test")))
	;; renderer_null_test links renderer_null (which links log, subsystem,
	;; subsystem_manager); subsystem is pulled in transitively.
	(check "closure renderer_null_test membership"
	       (set=? libs '("renderer_null" "log" "subsystem"
			     "subsystem_manager")))
	;; A library must precede the libraries it depends on.
	(check "closure renderer_null before its deps"
	       (and (< (index-of "renderer_null" libs) (index-of "log" libs))
		    (< (index-of "renderer_null" libs)
		       (index-of "subsystem" libs)))))

;; ---------------------------------------------------------------------------
;; Failure modes must fail loudly.
;; ---------------------------------------------------------------------------

(display "resolver: loud failures\n")
(let ((cyc (rz-target-table
	     (list (cons "d" '((library "A" (link "B"))
			       (library "B" (link "A"))))))))
	(check "cycle in link graph errors"
	       (expect-error (lambda () (resolve-includes cyc "A")))))

(let ((unk (rz-target-table
	     (list (cons "d" '((library "A" (link "nonesuch"))))))))
	(check "unknown link target errors"
	       (expect-error (lambda () (resolve-includes unk "A")))))

(let ((sys (rz-target-table
	     (list (cons "d" '((library "A" (sources "a.c") (public "inc")
				(link "m"))))))))
	(check "system lib m carries no include dir and no graph edge"
	       (and (null? (resolve-link-libs sys "A"))
		    (member "d/inc" (resolve-includes sys "A")))))

;; Whole-manifest resolution must not raise (no cycles, no unknown targets).
(check "resolve-check-all over the real manifest"
       (not (expect-error (lambda () (resolve-check-all table)))))

;; ---------------------------------------------------------------------------
;; Emitter smoke: the rendered build.ninja must carry the generated header and
;; a build stanza for a known leaf target.
;; ---------------------------------------------------------------------------

(display "emitter: rendered build.ninja\n")

;; The directory of KRUDD_NINJA_OUT is the build dir the WASM codegen (version.h,
;; shell.html, runtime_scm.h) is generated into; without it, render only.
(define (dirname path)
	(let loop ((i (- (string-length path) 1)))
		(cond ((< i 0) ".")
		      ((char=? (string-ref path i) #\/) (substring path 0 i))
		      (else (loop (- i 1))))))

(define ninja-out (getenv "KRUDD_NINJA_OUT"))
(define ninja-text
	(if (and ninja-out (> (string-length ninja-out) 0))
	    (ninja-synthesize manifest
			      (string-append krudd-root "/krudd/build/ninja")
			      (dirname ninja-out))
	    (ninja-synthesize manifest
			      (string-append krudd-root "/krudd/build/ninja"))))

(define (contains? hay needle)
	(let ((hl (string-length hay)) (nl (string-length needle)))
		(let loop ((i 0))
		  (cond ((> (+ i nl) hl) #f)
			((string=? (substring hay i (+ i nl)) needle) #t)
			(else (loop (+ i 1)))))))

(check "header present"
       (contains? ninja-text "Generated by krudd"))
(check "log library archive stanza present"
       (contains? ninja-text "build liblog.a: ar "))
(check "log_test link stanza present"
       (contains? ninja-text "build bin/log_test: link "))
(check "log test stamp present"
       (contains? ninja-text "build test/log.stamp: run_test bin/log_test"))
(check "interface-library emits no build output"
       (not (contains? ninja-text "renderer_interface")))
(check "plugin folds into an object, no standalone .wasm"
       (and (contains? ninja-text
	      (string-append "build wasm-obj/hello_plugin/plugins/hello_plugin/"
			     "hello_plugin.c.o: sm_cc "))
	    (not (contains? ninja-text "side_module"))))
(check "plugin object folds into the main module link"
       (contains? ninja-text
	 (string-append "main_module wasm-obj/index/modules/core/engine.c.o "
			"wasm-obj/index/modules/core/plugin_abi.c.o "
			"wasm-obj/hello_plugin/")))
(check "default target is native"
       (contains? ninja-text "default native"))
(check "wasm main module stanza present"
       (contains? ninja-text "build index.html | index.js index.wasm: main_module"))
(check "wasm target present"
       (contains? ninja-text "build wasm: phony "))

;; ---------------------------------------------------------------------------
;; Optional: write build.ninja for the harness to build with ninja(1).
;; ---------------------------------------------------------------------------

(if (and ninja-out (> (string-length ninja-out) 0))
    (begin
      (call-with-output-file ninja-out
	(lambda (port) (write-string ninja-text port)))
      (display (string-append "wrote " ninja-out "\n"))))

;; ---------------------------------------------------------------------------
;; Verdict.
;; ---------------------------------------------------------------------------

(if (= fail-count 0)
    (begin (display "RESOLVE-TESTS: OK\n") (exit 0))
    (begin (display (string-append "RESOLVE-TESTS: FAIL ("
				   (number->string fail-count) ")\n"))
	   (exit 1)))
