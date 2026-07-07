; SPDX-License-Identifier: GPL-2.0-or-later
;
; md_parse.scm — the markdown block parser, in Scheme.
;
; This is the first module strangler-figged from C to Scheme: a faithful port
; of md_parse.c's single-pass line scanner. It runs inside the same s7 runtime
; the engine boots (see modules/core/script.c); a thin C shim (md_parse_scm.c)
; marshals the block list this returns back into the caller's struct md_block[]
; so the C ABI (md_parse.h) is unchanged. The existing md_parse_test.c is run
; against both implementations to prove them byte-for-byte equivalent.
;
; A block is (list type level text spans); a span is (list start end style),
; with [start, end) byte offsets into text. The constants and every edge of the
; behaviour — heading levels 1..3, "- "/"* " list markers, ``` fences that emit
; one code block per interior line, **bold**/`code` inline runs with their
; delimiters stripped, the 255-byte text cap and the 16-span-per-block cap —
; mirror md_parse.c exactly.

(define md-text-max 256)          ; bytes incl. NUL; text is capped at 255 chars
(define md-spans-per-block 16)

(define md-block-paragraph 0)
(define md-block-heading   1)
(define md-block-list-item 2)
(define md-block-code      3)

(define md-span-bold 1)
(define md-span-code 2)

;; Char at index I of S, or #f past the end. The C helpers read one past a
;; line's content (into the '\n' or NUL); returning #f for those reads keeps a
;; short line from matching a prefix it doesn't have.
(define (md-ch s i)
  (and (< i (string-length s)) (string-ref s i)))

(define (md-space? c) (or (eqv? c #\space) (eqv? c #\tab)))

;; First index at or after I in S that is not a space/tab.
(define (md-skip-spaces s i)
  (if (md-space? (md-ch s i)) (md-skip-spaces s (+ i 1)) i))

;; #t if S is empty or only spaces/tabs (a blank line).
(define (md-blank? s)
  (= (md-skip-spaces s 0) (string-length s)))

;; #t if line opens or closes a fenced code block (leading ```).
(define (md-fence? s)
  (and (eqv? (md-ch s 0) #\`) (eqv? (md-ch s 1) #\`) (eqv? (md-ch s 2) #\`)))

;; Leading '#' count if S is a heading (1..3 hashes then a space/tab), else 0.
(define (md-count-hashes s)
  (let loop ((n 0))
    (if (eqv? (md-ch s n) #\#)
        (loop (+ n 1))
        (if (or (= n 0) (> n 3) (not (md-space? (md-ch s n))))
            0
            n))))

;; #t if S starts with "- " or "* " (an unordered list marker).
(define (md-list-marker? s)
  (and (or (eqv? (md-ch s 0) #\-) (eqv? (md-ch s 0) #\*))
       (md-space? (md-ch s 1))))

;; Substring S[start, end) clamped to at most 255 chars — copy_n's cap.
(define (md-take s start)
  (let* ((len (string-length s))
         (n   (min (- len start) (- md-text-max 1))))
    (substring s start (+ start n))))

;; md-inline-scan — one step of the inline pass from an explicit cursor state,
;; returning (cons new-text spans) at end of input. out is a reversed char list
;; of length o; spans is a reversed list of length ns. **bold** and `code`
;; delimiters are stripped and each styled run becomes a span over its now
;; delimiter-free bytes; an unterminated delimiter stays as a literal char.
;; It recurses with md-copy-run (mutually), which handles the copied runs and
;; hands control back here at the byte after the closing delimiter. Faithful to
;; md_parse.c: text capped at 255 chars, spans capped at 16, and a run whose
;; stripped text is empty (start >= end) emits no span.
(define (md-inline-scan text i o out spans ns)
  (let ((len (string-length text)))
    (if (or (>= i len) (>= o (- md-text-max 1)))
        (cons (list->string (reverse out)) (reverse spans))
        (let ((c (string-ref text i)))
          (cond
            ;; Bold: **...**
            ((and (eqv? c #\*)
                  (< (+ i 1) len)
                  (eqv? (string-ref text (+ i 1)) #\*))
             (let find ((j (+ i 2)))
               (cond
                 ((>= (+ j 1) len)   ; no closing **: keep '*' as literal
                  (md-inline-scan text (+ i 1) (+ o 1) (cons c out) spans ns))
                 ((and (eqv? (string-ref text j) #\*)
                       (eqv? (string-ref text (+ j 1)) #\*))
                  (md-copy-run text (+ i 2) j o out spans ns md-span-bold
                               (+ j 2)))
                 (else (find (+ j 1))))))
            ;; Inline code: `...`
            ((eqv? c #\`)
             (let find ((j (+ i 1)))
               (cond
                 ((>= j len)         ; no closing `: keep it as literal
                  (md-inline-scan text (+ i 1) (+ o 1) (cons c out) spans ns))
                 ((eqv? (string-ref text j) #\`)
                  (md-copy-run text (+ i 1) j o out spans ns md-span-code
                               (+ j 1)))
                 (else (find (+ j 1))))))
            ;; Plain byte.
            (else
             (md-inline-scan text (+ i 1) (+ o 1) (cons c out) spans ns)))))))

;; Copy TEXT[open, close) onto the output (respecting the 255 cap), append a
;; span of STYLE over the copied run if there is room and it is non-empty, then
;; resume the scan at RESUME. Mutually recursive with md-inline-scan.
(define (md-copy-run text open close o out spans ns style resume)
  (let copy ((k open) (oo o) (ob out))
    (if (and (< k close) (< oo (- md-text-max 1)))
        (copy (+ k 1) (+ oo 1) (cons (string-ref text k) ob))
        (let ((add? (and (< ns md-spans-per-block) (< o oo))))
          (md-inline-scan text resume oo ob
                          (if add? (cons (list o oo style) spans) spans)
                          (if add? (+ ns 1) ns))))))

;; parse-inline — the inline pass over TEXT, from a clean cursor.
(define (md-parse-inline text)
  (md-inline-scan text 0 0 '() '() 0))

;; Build a block whose body runs through the inline pass (heading/list/para).
(define (md-block type level raw)
  (let* ((r     (md-parse-inline raw))
         (text  (car r))
         (spans (cdr r)))
    (list type level text spans)))

;; md-parse — parse SRC into a list of blocks. The C shim caps the list at the
;; caller's `max`; parsing the whole string and letting the shim take the
;; prefix yields the same first `max` blocks the C parser would emit.
(define (md-parse src)
  (let ((len (string-length src)))
    (let loop ((i 0) (in-fence #f) (acc '()))
      (if (>= i len)
          (reverse acc)
          (let* ((eol  (let scan ((k i))
                         (if (or (>= k len) (eqv? (string-ref src k) #\newline))
                             k
                             (scan (+ k 1)))))
                 (line (substring src i eol))
                 (next (if (and (< eol len)
                                (eqv? (string-ref src eol) #\newline))
                           (+ eol 1)
                           eol)))
            (cond
              ;; Inside a fence: a ``` closes it; anything else is a code line.
              (in-fence
               (if (md-fence? line)
                   (loop next #f acc)
                   (loop next #t
                         (cons (list md-block-code 0 (md-take line 0) '())
                               acc))))
              ;; A ``` opens a fence.
              ((md-fence? line)
               (loop next #t acc))
              ;; Blank lines separate blocks but emit nothing.
              ((md-blank? line)
               (loop next #f acc))
              ;; Heading.
              ((> (md-count-hashes line) 0)
               (let* ((hashes (md-count-hashes line))
                      (start  (md-skip-spaces line hashes)))
                 (loop next #f
                       (cons (md-block md-block-heading hashes
                                       (md-take line start))
                             acc))))
              ;; List item — exactly the "- "/"* " marker (2 chars) is dropped.
              ((md-list-marker? line)
               (loop next #f
                     (cons (md-block md-block-list-item 0 (md-take line 2))
                           acc)))
              ;; Paragraph.
              (else
               (loop next #f
                     (cons (md-block md-block-paragraph 0 (md-take line 0))
                           acc)))))))))
