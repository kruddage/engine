; SPDX-License-Identifier: GPL-2.0-or-later

(define krudd-root (or (getenv "KRUDD_ROOT") "."))

(load (string-append krudd-root "/krudd/kruddmake/ninja.scm"))

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

(define (load-datum path) (call-with-input-file path read))

(define manifest-dirs
  (load-datum (string-append krudd-root "/krudd/kruddmake/manifest.scm")))

(define (load-spec dir)
  (load-datum (string-append krudd-root "/krudd/engine/" dir
                             "/build.scm")))

(define manifest
  (map (lambda (d) (cons d (load-spec d))) manifest-dirs))

(define table (rz-target-table manifest))

(define (inc-check name expected)
  (check (string-append "includes " name)
         (set=? (resolve-includes table name) expected)))

(display "resolver: include sets vs CMake ground truth\n")
(inc-check "log" '("log/include" "abi"
                   "core/include"))
(inc-check "log_test" '("log/include" "abi"))
(inc-check "renderer_null" '("${generated}" "log/include"
                             "abi" "core/include"))
(inc-check "renderer_null_test"
           '("render/null" "${generated}" "log/include"
             "abi" "core/include"))
(inc-check "fg_test" '("render/frame_graph" "${generated}"
                       "render/null" "log/include"
                       "abi" "memory/include"
                       "core/include"))
(inc-check "asset_plugin" '("asset" "abi" "log/include"
                            "memory/include" "core/include"))

(display "resolver: transitive link closures\n")
(let ((libs (resolve-link-libs table "renderer_null_test")))
  (check "closure renderer_null_test membership"
         (set=? libs '("renderer_null" "log" "subsystem"
                       "subsystem_manager")))
  (check "closure renderer_null before its deps"
         (and (< (index-of "renderer_null" libs) (index-of "log" libs))
              (< (index-of "renderer_null" libs)
                 (index-of "subsystem" libs)))))

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

(check "resolve-check-all over the real manifest"
       (not (expect-error (lambda () (resolve-check-all table)))))

(display "emitter: rendered build.ninja\n")

(define (dirname path)
  (let loop ((i (- (string-length path) 1)))
    (cond ((< i 0) ".")
          ((char=? (string-ref path i) #\/) (substring path 0 i))
          (else (loop (- i 1))))))

(define ninja-out (getenv "KRUDD_NINJA_OUT"))

;;! When the harness gives us an s7 interpreter path, wire the generator edge to
;;! re-run this very script, so a `.scm` edit under raw `ninja` regenerates the
;;! codegen headers before recompiling their consumers. Without it the emitted
;;! build.ninja simply has no `regen` edge (fine for the string checks below).
(define s7bin (getenv "KRUDD_S7BIN"))
(define regen-cmd
  (if (and s7bin (> (string-length s7bin) 0)
           ninja-out (> (string-length ninja-out) 0))
      (string-append "env KRUDD_ROOT=" krudd-root
                     " KRUDD_NINJA_OUT=" ninja-out
                     " KRUDD_S7BIN=" s7bin " "
                     s7bin " " krudd-root
                     "/krudd/kruddmake/resolve_test.scm")
      #f))

(define ninja-text
  (if (and ninja-out (> (string-length ninja-out) 0))
      (ninja-synthesize manifest
                        (string-append krudd-root "/krudd/engine")
                        (dirname ninja-out)
                        regen-cmd)
      (ninja-synthesize manifest
                        (string-append krudd-root "/krudd/engine"))))

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
(check "C plugin compiles as a WASM library object (emcc_c), no side-module rule"
       (and (contains? ninja-text
                       (string-append "build wasm-obj/edit_plugin/edit/"
                                      "edit_plugin.c.o: emcc_c "))
            (contains? ninja-text "build wasm/libedit_plugin.a: emar ")
            (not (contains? ninja-text "sm_cc"))
            (not (contains? ninja-text "side_module"))))
(check "C++ module compiles with emcc_cxx and its wasm-flags"
       (and (contains? ninja-text
                       (string-append "build wasm-obj/kruddboard/ui/kruddboard/"
                                      "kruddboard.cpp.o: emcc_cxx "))
            (contains? ninja-text "emcxxflags = --std=c++17")))
(check "plugin archive folds into the main module link"
       (and (contains? ninja-text
                       (string-append "main_module wasm-obj/index/core/engine.c.o "
                                      "wasm-obj/index/core/plugin_abi.c.o "))
            (contains? ninja-text "wasm/libedit_plugin.a")
            (contains? ninja-text "wasm/libkruddboard.a")))
(check "default target is native"
       (contains? ninja-text "default native"))
(check "wasm main module stanza present"
       (contains? ninja-text "build index.html | index.js index.wasm: main_module"))
(check "wasm target present"
       (contains? ninja-text "build wasm: phony "))
(check "compile rules track headers via gcc depfiles"
       (and (contains? ninja-text "deps = gcc")
            (contains? ninja-text "depfile = $out.d")))
(if regen-cmd
    (begin
      (check "regen generator edge present"
             (and (contains? ninja-text "build build.ninja: regen ")
                  (contains? ninja-text "generator = 1")))
      (check "regen edge lists an embedded .scm source as an input"
             (contains? ninja-text "ui/kruddgui/kruddgui.scm"))))

(if (and ninja-out (> (string-length ninja-out) 0))
    (begin
      (call-with-output-file ninja-out
        (lambda (port) (write-string ninja-text port)))
      (display (string-append "wrote " ninja-out "\n"))))

(if (= fail-count 0)
    (begin (display "RESOLVE-TESTS: OK\n") (exit 0))
    (begin (display (string-append "RESOLVE-TESTS: FAIL ("
                                   (number->string fail-count) ")\n"))
           (exit 1)))
