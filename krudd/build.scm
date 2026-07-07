; SPDX-License-Identifier: GPL-2.0-or-later
;
; krudd build description — the build authority, in Scheme.
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
