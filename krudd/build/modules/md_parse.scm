; SPDX-License-Identifier: GPL-2.0-or-later
;; scm-lint:off
;
; md_parse.scm — the markdown block parser, in Scheme.
;
; This is the first module strangler-figged from C to Scheme: a faithful port
; of md_parse.c's single-pass line scanner. It lives outside the ninja build
; tree, under krudd/build/modules/, as a build-owned Scheme module. It is the
; single ABI artifact in git: the C ABI declaration below drives krudd's binding
; generator, which emits the whole C seam at synthesis time — the md_parse.h
; header (constants + struct md_span/md_block + the md_parse() prototype) and
; the md_parse.scm.c shim (this module baked into a byte array, the generated
; marshalers, and the md_parse() driver that calls (md-parse) and marshals the
; block list this returns into struct md_block[]). It runs inside the same s7
; runtime the engine boots (see modules/core/script.c). The existing
; md_parse_test.c is run against both this port and the C parser to prove them
; byte-for-byte equivalent.
;
; A block is (list type level text spans); a span is (list start end style),
; with [start, end) byte offsets into text. The constants and every edge of the
; behaviour — heading levels 1..3, "- "/"* " list markers, ``` fences that emit
; one code block per interior line, **bold**/`code` inline runs with their
; delimiters stripped, the 255-byte text cap and the 16-span-per-block cap —
; mirror md_parse.c exactly.
;; scm-lint:on

;; scm-lint:off
;; md-text-max counts the NUL, so text is capped at 255 chars; md-blocks-max and
;; md-span-normal are ABI-only — the caller's suggested per-parse cap and the
;; implicit style of unspanned text, neither read by this file.
;; scm-lint:on
(define md-text-max 256)
(define md-spans-per-block 16)
(define md-blocks-max 128)

(define md-block-paragraph 0)
(define md-block-heading   1)
(define md-block-list-item 2)
(define md-block-code      3)

(define md-span-normal 0)
(define md-span-bold   1)
(define md-span-code   2)

;; scm-lint:off
;; ---------------------------------------------------------------------------
;; C ABI declaration.
;;
;; krudd reads these forms at synthesis time — structurally, without evaluating
;; them — and generates the whole C ABI from them: the md_parse.h header
;; (constants + structs + prototype) and the md_parse.scm.c marshaling shim.
;; The .scm is the only ABI artifact in git; both C files are build outputs.
;;
;; The forms are no-ops when this module is loaded into s7: the two macros below
;; expand them to #f, so the exact same text is both the runtime image the shim
;; bakes in and the declaration the generator reads. The binding vocabulary the
;; generator understands lives in krudd/build/introspect.scm.
;; ---------------------------------------------------------------------------
;; scm-lint:on

(define-macro (define-c-struct . _) #f)
(define-macro (define-c-export . _) #f)

;; scm-lint:off
;; A block is (type level text spans); the shim marshals the returned list
;; positionally, so the field order here is the order md-parse returns its
;; values. span-count is a count field — derived from the spans list, not part
;; of the returned block — so it is skipped when marshaling positionally.
;; scm-lint:on
(define-c-struct md-span
  (start u32)
  (end   u32)
  (style u32))

(define-c-struct md-block
  (type       i32)
  (level      i32)
  (text       (char md-text-max))
  (spans      (vector md-span md-spans-per-block) span-count)
  (span-count u32))

(define-c-export (md-parse (src string) -> (vector md-block max))
  (calls md-parse))

;; scm-lint:off
;; Char at index I of S, or #f past the end. The C helpers read one past a
;; line's content (into the '\n' or NUL); returning #f for those reads keeps a
;; short line from matching a prefix it doesn't have.
;; scm-lint:on
(define (md-ch s i)
  (and (< i (string-length s)) (string-ref s i)))

(define (md-space? c) (or (eqv? c #\space) (eqv? c #\tab)))

;; scm-lint:off
;; First index at or after I in S that is not a space/tab.
;; scm-lint:on
(define (md-skip-spaces s i)
  (if (md-space? (md-ch s i)) (md-skip-spaces s (+ i 1)) i))

;; scm-lint:off
;; #t if S is empty or only spaces/tabs (a blank line).
;; scm-lint:on
(define (md-blank? s)
  (= (md-skip-spaces s 0) (string-length s)))

;; scm-lint:off
;; #t if line opens or closes a fenced code block (leading ```).
;; scm-lint:on
(define (md-fence? s)
  (and (eqv? (md-ch s 0) #\`) (eqv? (md-ch s 1) #\`) (eqv? (md-ch s 2) #\`)))

;; scm-lint:off
;; Leading '#' count if S is a heading (1..3 hashes then a space/tab), else 0.
;; scm-lint:on
(define (md-count-hashes s)
  (let loop ((n 0))
    (if (eqv? (md-ch s n) #\#)
        (loop (+ n 1))
        (if (or (= n 0) (> n 3) (not (md-space? (md-ch s n))))
            0
            n))))

;; scm-lint:off
;; #t if S starts with "- " or "* " (an unordered list marker).
;; scm-lint:on
(define (md-list-marker? s)
  (and (or (eqv? (md-ch s 0) #\-) (eqv? (md-ch s 0) #\*))
       (md-space? (md-ch s 1))))

;; scm-lint:off
;; Substring S[start, end) clamped to at most 255 chars — copy_n's cap.
;; scm-lint:on
(define (md-take s start)
  (let* ((len (string-length s))
         (n   (min (- len start) (- md-text-max 1))))
    (substring s start (+ start n))))

;; scm-lint:off
;; md-inline-scan — one step of the inline pass from an explicit cursor state,
;; returning (cons new-text spans) at end of input. out is a reversed char list
;; of length o; spans is a reversed list of length ns. **bold** and `code`
;; delimiters are stripped and each styled run becomes a span over its now
;; delimiter-free bytes; an unterminated delimiter stays as a literal char.
;; It recurses with md-copy-run (mutually), which handles the copied runs and
;; hands control back here at the byte after the closing delimiter. Faithful to
;; md_parse.c: text capped at 255 chars, spans capped at 16, and a run whose
;; stripped text is empty (start >= end) emits no span.
;; scm-lint:on
(define (md-inline-scan text i o out spans ns)
  (let ((len (string-length text)))
    (if (or (>= i len) (>= o (- md-text-max 1)))
        (cons (list->string (reverse out)) (reverse spans))
        (let ((c (string-ref text i)))
          (cond
            ;; scm-lint:off
            ;; Bold: **...**. No closing ** keeps the '*' as a literal char.
            ;; scm-lint:on
            ((and (eqv? c #\*)
                  (< (+ i 1) len)
                  (eqv? (string-ref text (+ i 1)) #\*))
             (let find ((j (+ i 2)))
               (cond
                 ((>= (+ j 1) len)
                  (md-inline-scan text (+ i 1) (+ o 1) (cons c out) spans ns))
                 ((and (eqv? (string-ref text j) #\*)
                       (eqv? (string-ref text (+ j 1)) #\*))
                  (md-copy-run text (+ i 2) j o out spans ns md-span-bold
                               (+ j 2)))
                 (else (find (+ j 1))))))
            ;; scm-lint:off
            ;; Inline code: `...`. No closing ` keeps it as a literal char.
            ;; scm-lint:on
            ((eqv? c #\`)
             (let find ((j (+ i 1)))
               (cond
                 ((>= j len)
                  (md-inline-scan text (+ i 1) (+ o 1) (cons c out) spans ns))
                 ((eqv? (string-ref text j) #\`)
                  (md-copy-run text (+ i 1) j o out spans ns md-span-code
                               (+ j 1)))
                 (else (find (+ j 1))))))
            ;; scm-lint:off
            ;; Plain byte.
            ;; scm-lint:on
            (else
             (md-inline-scan text (+ i 1) (+ o 1) (cons c out) spans ns)))))))

;; scm-lint:off
;; Copy TEXT[open, close) onto the output (respecting the 255 cap), append a
;; span of STYLE over the copied run if there is room and it is non-empty, then
;; resume the scan at RESUME. Mutually recursive with md-inline-scan.
;; scm-lint:on
(define (md-copy-run text open close o out spans ns style resume)
  (let copy ((k open) (oo o) (ob out))
    (if (and (< k close) (< oo (- md-text-max 1)))
        (copy (+ k 1) (+ oo 1) (cons (string-ref text k) ob))
        (let ((add? (and (< ns md-spans-per-block) (< o oo))))
          (md-inline-scan text resume oo ob
                          (if add? (cons (list o oo style) spans) spans)
                          (if add? (+ ns 1) ns))))))

;; scm-lint:off
;; parse-inline — the inline pass over TEXT, from a clean cursor.
;; scm-lint:on
(define (md-parse-inline text)
  (md-inline-scan text 0 0 '() '() 0))

;; scm-lint:off
;; Build a block whose body runs through the inline pass (heading/list/para).
;; scm-lint:on
(define (md-block type level raw)
  (let* ((r     (md-parse-inline raw))
         (text  (car r))
         (spans (cdr r)))
    (list type level text spans)))

;; scm-lint:off
;; md-parse — parse SRC into a list of blocks. The C shim caps the list at the
;; caller's `max`; parsing the whole string and letting the shim take the
;; prefix yields the same first `max` blocks the C parser would emit.
;; scm-lint:on
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
              ;; scm-lint:off
              ;; Inside a fence: a ``` closes it; anything else is a code line.
              ;; scm-lint:on
              (in-fence
               (if (md-fence? line)
                   (loop next #f acc)
                   (loop next #t
                         (cons (list md-block-code 0 (md-take line 0) '())
                               acc))))
              ;; scm-lint:off
              ;; A ``` opens a fence.
              ;; scm-lint:on
              ((md-fence? line)
               (loop next #t acc))
              ;; scm-lint:off
              ;; Blank lines separate blocks but emit nothing.
              ;; scm-lint:on
              ((md-blank? line)
               (loop next #f acc))
              ;; scm-lint:off
              ;; Heading.
              ;; scm-lint:on
              ((> (md-count-hashes line) 0)
               (let* ((hashes (md-count-hashes line))
                      (start  (md-skip-spaces line hashes)))
                 (loop next #f
                       (cons (md-block md-block-heading hashes
                                       (md-take line start))
                             acc))))
              ;; scm-lint:off
              ;; List item — exactly the "- "/"* " marker (2 chars) is dropped.
              ;; scm-lint:on
              ((md-list-marker? line)
               (loop next #f
                     (cons (md-block md-block-list-item 0 (md-take line 2))
                           acc)))
              ;; scm-lint:off
              ;; Paragraph.
              ;; scm-lint:on
              (else
               (loop next #f
                     (cons (md-block md-block-paragraph 0 (md-take line 0))
                           acc)))))))))
