; SPDX-License-Identifier: GPL-2.0-or-later

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

(define-macro (define-c-struct . _) #f)
(define-macro (define-c-export . _) #f)

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

(define (md-ch s i)
  (and (< i (string-length s)) (string-ref s i)))

(define (md-space? c) (or (eqv? c #\space) (eqv? c #\tab)))

(define (md-skip-spaces s i)
  (if (md-space? (md-ch s i)) (md-skip-spaces s (+ i 1)) i))

(define (md-blank? s)
  (= (md-skip-spaces s 0) (string-length s)))

(define (md-fence? s)
  (and (eqv? (md-ch s 0) #\`) (eqv? (md-ch s 1) #\`) (eqv? (md-ch s 2) #\`)))

(define (md-count-hashes s)
  (let loop ((n 0))
    (if (eqv? (md-ch s n) #\#)
        (loop (+ n 1))
        (if (or (= n 0) (> n 3) (not (md-space? (md-ch s n))))
            0
            n))))

(define (md-list-marker? s)
  (and (or (eqv? (md-ch s 0) #\-) (eqv? (md-ch s 0) #\*))
       (md-space? (md-ch s 1))))

(define (md-take s start)
  (let* ((len (string-length s))
         (n   (min (- len start) (- md-text-max 1))))
    (substring s start (+ start n))))

(define (md-inline-scan text i o out spans ns)
  (let ((len (string-length text)))
    (if (or (>= i len) (>= o (- md-text-max 1)))
        (cons (list->string (reverse out)) (reverse spans))
        (let ((c (string-ref text i)))
          (cond
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
           ((eqv? c #\`)
            (let find ((j (+ i 1)))
              (cond
               ((>= j len)
                (md-inline-scan text (+ i 1) (+ o 1) (cons c out) spans ns))
               ((eqv? (string-ref text j) #\`)
                (md-copy-run text (+ i 1) j o out spans ns md-span-code
                             (+ j 1)))
               (else (find (+ j 1))))))
           (else
            (md-inline-scan text (+ i 1) (+ o 1) (cons c out) spans ns)))))))

(define (md-copy-run text open close o out spans ns style resume)
  (let copy ((k open) (oo o) (ob out))
    (if (and (< k close) (< oo (- md-text-max 1)))
        (copy (+ k 1) (+ oo 1) (cons (string-ref text k) ob))
        (let ((add? (and (< ns md-spans-per-block) (< o oo))))
          (md-inline-scan text resume oo ob
                          (if add? (cons (list o oo style) spans) spans)
                          (if add? (+ ns 1) ns))))))

(define (md-parse-inline text)
  (md-inline-scan text 0 0 '() '() 0))

(define (md-block type level raw)
  (let* ((r     (md-parse-inline raw))
         (text  (car r))
         (spans (cdr r)))
    (list type level text spans)))

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
             (in-fence
              (if (md-fence? line)
                  (loop next #f acc)
                  (loop next #t
                        (cons (list md-block-code 0 (md-take line 0) '())
                              acc))))
             ((md-fence? line)
              (loop next #t acc))
             ((md-blank? line)
              (loop next #f acc))
             ((> (md-count-hashes line) 0)
              (let* ((hashes (md-count-hashes line))
                     (start  (md-skip-spaces line hashes)))
                (loop next #f
                      (cons (md-block md-block-heading hashes
                                      (md-take line start))
                            acc))))
             ((md-list-marker? line)
              (loop next #f
                    (cons (md-block md-block-list-item 0 (md-take line 2))
                          acc)))
             (else
              (loop next #f
                    (cons (md-block md-block-paragraph 0 (md-take line 0))
                          acc)))))))))
