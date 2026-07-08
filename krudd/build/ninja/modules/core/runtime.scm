; SPDX-License-Identifier: GPL-2.0-or-later

(krudd-log 1 "runtime: Scheme image live — krudd.scm owns the tick")

(define *frame* 0)

(define (tick)
  (set! *frame* (+ *frame* 1))
  (when (= 0 (modulo *frame* 60))
    (krudd-log 0 (string-append "runtime: tick " (number->string *frame*)))))
