; SPDX-License-Identifier: GPL-2.0-or-later
;
; krudd build description — the build authority, in Scheme.
;
; This is the payoff of the strangler fig: `krudd build` no longer hard-codes
; the build, it loads THIS file and lets s7 drive. Phase 1 forwards exactly the
; cmake invocation krudd used to bake in, so the swap is a behavioural no-op —
; proof that Scheme now sits in the driver's seat. Later this file constructs
; the CMake (and CI, and the plugin manifest) itself instead of forwarding a
; prebaked command.
;
; krudd (see ../krudd.c) provides:
;   (run cmd)     -> run a shell command, return its integer exit status
;   *configure*   -> the configure command string (cmake / emcmake cmake ...)
;   *build*       -> the build command string

(define (sh cmd)
  (let ((status (run cmd)))
    (if (not (= status 0))
        (error 'krudd-build-failed cmd))))

(sh *configure*)
(sh *build*)
