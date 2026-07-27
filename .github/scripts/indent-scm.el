;; SPDX-License-Identifier: GPL-2.0-or-later
;;
;; Batch-mode reindenter for .scm files, driven by indent-scm.py. Reindents
;; each file passed on the command line in place using Emacs's built-in
;; scheme-mode indenter (spaces, call arguments aligned under the first
;; argument) — the "standard indenter output" CODING_STANDARD.md calls for.
;;
;; Line 1 (the SPDX header) is skipped: it is a single-`;` comment, and Lisp
;; indentation rules push single-`;` comments out to comment-column instead
;; of column 0, which would mangle the header.
(require 'scheme)

(defun krudd-indent-file (path)
  (let ((buf (find-file-noselect path)))
    (with-current-buffer buf
      (scheme-mode)
      (setq indent-tabs-mode nil)
      (setq tab-width 8)
      (let ((inhibit-message t))
        (goto-char (point-min))
        (forward-line 1)
        (indent-region (point) (point-max))
        (untabify (point-min) (point-max)))
      (delete-trailing-whitespace)
      (save-buffer))))

(dolist (f command-line-args-left)
  (krudd-indent-file f))
