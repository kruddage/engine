; SPDX-License-Identifier: GPL-2.0-or-later
;; scm-lint:off
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
;; scm-lint:on

(define krudd-root (or (getenv "KRUDD_ROOT") "."))

(load (string-append krudd-root "/krudd/build/ninja/ninja.scm"))

;; scm-lint:off
;; ---------------------------------------------------------------------------
;; Assertion plumbing.
;; ---------------------------------------------------------------------------
;; scm-lint:on

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

;; scm-lint:off
;; ---------------------------------------------------------------------------
;; The real manifest.
;; ---------------------------------------------------------------------------
;; scm-lint:on

(define (load-datum path) (call-with-input-file path read))

(define manifest-dirs
	(load-datum (string-append krudd-root "/krudd/build/ninja/manifest.scm")))

(define (load-spec dir)
	(load-datum (string-append krudd-root "/krudd/build/ninja/" dir
				   "/build.scm")))

(define manifest
	(map (lambda (d) (cons d (load-spec d))) manifest-dirs))

(define table (rz-target-table manifest))

;; scm-lint:off
;; ---------------------------------------------------------------------------
;; Include-set checks. Expected sets are the reference -I lists for these
;; targets (paths normalised relative to krudd/build/ninja/). The resolver must
;; produce the same *set*; order is not build-relevant.
;; ---------------------------------------------------------------------------
;; scm-lint:on

(define (inc-check name expected)
	(check (string-append "includes " name)
	       (set=? (resolve-includes table name) expected)))

(display "resolver: include sets vs CMake ground truth\n")
(inc-check "log" '("modules/log/include" "modules/include"
		   "modules/core/include"))
(inc-check "log_test" '("modules/log/include" "modules/include"))
(inc-check "renderer_null" '("modules/renderer" "modules/log/include"
			     "modules/include" "modules/core/include"))
(inc-check "renderer_null_test"
	   '("modules/renderer_null" "modules/renderer" "modules/log/include"
	     "modules/include" "modules/core/include"))
(inc-check "fg_test" '("modules/frame_graph" "modules/renderer"
		       "modules/renderer_null" "modules/log/include"
		       "modules/include" "modules/memory/include"
		       "modules/core/include"))
(inc-check "asset_plugin" '("modules/asset" "modules/include"
			    "modules/log/include" "modules/memory/include"
			    "modules/core/include"))
(inc-check "scene_test" '("modules/scene_plugin" "modules/include"
			  "modules/core/include" "modules/asset"
			  "modules/log/include" "modules/memory/include"))

;; scm-lint:off
;; ---------------------------------------------------------------------------
;; Link-closure checks: transitive membership and dependents-first ordering.
;; ---------------------------------------------------------------------------
;; scm-lint:on

(display "resolver: transitive link closures\n")
(let ((libs (resolve-link-libs table "renderer_null_test")))
	;; scm-lint:off
	;; renderer_null_test links renderer_null (which links log, subsystem,
	;; subsystem_manager); subsystem is pulled in transitively.
	;; scm-lint:on
	(check "closure renderer_null_test membership"
	       (set=? libs '("renderer_null" "log" "subsystem"
			     "subsystem_manager")))
	;; scm-lint:off
	;; A library must precede the libraries it depends on.
	;; scm-lint:on
	(check "closure renderer_null before its deps"
	       (and (< (index-of "renderer_null" libs) (index-of "log" libs))
		    (< (index-of "renderer_null" libs)
		       (index-of "subsystem" libs)))))

;; scm-lint:off
;; ---------------------------------------------------------------------------
;; Failure modes must fail loudly.
;; ---------------------------------------------------------------------------
;; scm-lint:on

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

;; scm-lint:off
;; Whole-manifest resolution must not raise (no cycles, no unknown targets).
;; scm-lint:on
(check "resolve-check-all over the real manifest"
       (not (expect-error (lambda () (resolve-check-all table)))))

;; scm-lint:off
;; ---------------------------------------------------------------------------
;; Emitter smoke: the rendered build.ninja must carry the generated header and
;; a build stanza for a known leaf target.
;; ---------------------------------------------------------------------------
;; scm-lint:on

(display "emitter: rendered build.ninja\n")

;; scm-lint:off
;; The directory of KRUDD_NINJA_OUT is the build dir the WASM codegen (version.h,
;; shell.html, runtime_scm.h) is generated into; without it, render only.
;; scm-lint:on
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
;; scm-lint:off
;; A plugin is now an ordinary WASM library: its C sources compile with emcc_c
;; and archive to wasm/lib<name>.a — no side-module rule survives.
;; scm-lint:on
(check "C plugin compiles as a WASM library object (emcc_c), no side-module rule"
       (and (contains? ninja-text
	      (string-append "build wasm-obj/scene_plugin/modules/scene_plugin/"
			     "scene_plugin.c.o: emcc_c "))
	    (contains? ninja-text "build wasm/libscene_plugin.a: emar ")
	    (not (contains? ninja-text "sm_cc"))
	    (not (contains? ninja-text "side_module"))))
;; scm-lint:off
;; The C++ modules (imgui, the board) take emcc_cxx with their own $emcxxflags.
;; scm-lint:on
(check "C++ module compiles with emcc_cxx and its wasm-flags"
       (and (contains? ninja-text
	      (string-append "build wasm-obj/imgui_plugin/modules/imgui_plugin/"
			     "imgui_plugin.cpp.o: emcc_cxx "))
	    (contains? ninja-text "emcxxflags = --std=c++17")))
;; scm-lint:off
;; The main module links each module as an archive (engine + plugin_abi objects
;; first), not a pile of loose plugin objects.
;; scm-lint:on
(check "plugin archive folds into the main module link"
       (and (contains? ninja-text
	      (string-append "main_module wasm-obj/index/modules/core/engine.c.o "
			     "wasm-obj/index/modules/core/plugin_abi.c.o "))
	    (contains? ninja-text "wasm/libscene_plugin.a")
	    (contains? ninja-text "wasm/libimgui_plugin.a")))
(check "default target is native"
       (contains? ninja-text "default native"))
(check "wasm main module stanza present"
       (contains? ninja-text "build index.html | index.js index.wasm: main_module"))
(check "wasm target present"
       (contains? ninja-text "build wasm: phony "))

;; scm-lint:off
;; ---------------------------------------------------------------------------
;; Optional: write build.ninja for the harness to build with ninja(1).
;; ---------------------------------------------------------------------------
;; scm-lint:on

(if (and ninja-out (> (string-length ninja-out) 0))
    (begin
      (call-with-output-file ninja-out
	(lambda (port) (write-string ninja-text port)))
      (display (string-append "wrote " ninja-out "\n"))))

;; scm-lint:off
;; ---------------------------------------------------------------------------
;; Verdict.
;; ---------------------------------------------------------------------------
;; scm-lint:on

(if (= fail-count 0)
    (begin (display "RESOLVE-TESTS: OK\n") (exit 0))
    (begin (display (string-append "RESOLVE-TESTS: FAIL ("
				   (number->string fail-count) ")\n"))
	   (exit 1)))
