; SPDX-License-Identifier: GPL-2.0-or-later

(define (rz-filter pred lst)
  (cond ((null? lst) '())
        ((pred (car lst)) (cons (car lst) (rz-filter pred (cdr lst))))
        (else (rz-filter pred (cdr lst)))))

(define (rz-dedup lst)
  (let loop ((l lst) (seen '()) (out '()))
    (cond ((null? l) (reverse out))
          ((member (car l) seen) (loop (cdr l) seen out))
          (else (loop (cdr l) (cons (car l) seen)
                      (cons (car l) out))))))

(define (rz-clause head clauses)
  (cond ((null? clauses) #f)
        ((and (pair? (car clauses)) (eq? (caar clauses) head))
         (car clauses))
        (else (rz-clause head (cdr clauses)))))

(define rz-system-libs (list "m"))

(define (rz-system-lib? name) (member name rz-system-libs))

(define (rz-join dir sub)
  (cond ((or (string=? sub "") (string=? sub ".")) dir)
        ((string=? dir "") sub)
        (else (string-append dir "/" sub))))

(define (rz-path dir p)
  (cond ((string? p) (rz-join dir p))
        ((and (pair? p) (eq? (car p) 'root)) (cadr p))
        ((and (pair? p) (eq? (car p) 'current))
         (if (null? (cdr p)) dir (rz-join dir (cadr p))))
        ((and (pair? p) (eq? (car p) 'raw)) (cadr p))
        (else (error 'rz-bad-path p))))

(define (rz-paths dir specs) (map (lambda (p) (rz-path dir p)) specs))

(define (rz-clause-dirs dir clauses head)
  (let ((c (rz-clause head clauses)))
    (if c (rz-paths dir (cdr c)) '())))

(define (rz-make-target name dir kind public private links wasm-modules)
  (list name (cons 'dir dir) (cons 'kind kind)
        (cons 'public public) (cons 'private private)
        (cons 'links links) (cons 'wasm-modules wasm-modules)))

(define (rz-field target key) (cdr (assq key (cdr target))))

(define (rz-form->target dir form)
  (case (car form)
    ((library executable)
     (let ((clauses (cddr form)))
       (rz-make-target
        (cadr form) dir (car form)
        (rz-clause-dirs dir clauses 'public)
        (rz-clause-dirs dir clauses 'private)
        (let ((c (rz-clause 'link clauses))) (if c (cdr c) '()))
        (let ((c (rz-clause 'wasm-modules clauses)))
          (if c (cdr c) '())))))
    ((interface-library)
     (rz-make-target
      (cadr form) dir 'interface-library
      (rz-clause-dirs dir (cddr form) 'interface)
      '() '() '()))
    (else #f)))

(define (rz-spec-targets dir spec)
  (let loop ((forms spec) (out '()))
    (cond ((null? forms) (reverse out))
          ((memq (caar forms) '(native-only wasm-only))
           (loop (cdr forms)
                 (append (reverse (rz-spec-targets dir (cdar forms)))
                         out)))
          (else
           (let ((t (rz-form->target dir (car forms))))
             (loop (cdr forms) (if t (cons t out) out)))))))

(define (rz-target-table manifest)
  (apply append
         (map (lambda (pair) (rz-spec-targets (car pair) (cdr pair)))
              manifest)))

(define (rz-lookup table name) (assoc name table))

(define (rz-direct-deps table name)
  (let ((target (rz-lookup table name)))
    (if (not target)
        '()
        (rz-filter
         (lambda (dep) dep)
         (map (lambda (link)
                (cond ((rz-lookup table link) link)
                      ((rz-system-lib? link) #f)
                      (else (error 'rz-unknown-link-target
                                   (list 'in name 'links link)))))
              (rz-field target 'links))))))

(define (rz-closure table roots)
  (let ((state '())
        (out '()))
    (define (mark name tag) (set! state (cons (cons name tag) state)))
    (define (status name)
      (let ((s (assoc name state))) (and s (cdr s))))
    (define (visit name path)
      (case (status name)
        ((done) #t)
        ((active) (error 'rz-link-cycle (reverse (cons name path))))
        (else
         (mark name 'active)
         (for-each (lambda (dep) (visit dep (cons name path)))
                   (rz-direct-deps table name))
         (mark name 'done)
         (set! out (cons name out)))))
    (for-each (lambda (r) (visit r '())) roots)
    out))

(define (resolve-link-libs table name)
  (rz-closure table (rz-direct-deps table name)))

(define (resolve-wasm-module-libs table name)
  (let ((target (rz-lookup table name)))
    (rz-closure table
                (append (if target (rz-field target 'wasm-modules) '())
                        (rz-direct-deps table name)))))

(define (rz-target-syslibs table name)
  (let ((target (rz-lookup table name)))
    (if target
        (rz-filter rz-system-lib? (rz-field target 'links))
        '())))

(define (resolve-syslibs table name)
  (rz-dedup
   (apply append
          (map (lambda (n) (rz-target-syslibs table n))
               (cons name (resolve-link-libs table name))))))

(define (resolve-includes table name)
  (let ((target (rz-lookup table name)))
    (if (not target)
        (error 'rz-unknown-target name)
        (rz-dedup
         (append
          (rz-field target 'public)
          (rz-field target 'private)
          (apply append
                 (map (lambda (lib)
                        (rz-field (rz-lookup table lib) 'public))
                      (resolve-link-libs table name))))))))

(define (resolve-check-all table)
  (for-each (lambda (target) (resolve-includes table (car target)))
            table)
  #t)
