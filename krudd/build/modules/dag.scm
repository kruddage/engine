; SPDX-License-Identifier: GPL-2.0-or-later

;;! dag — a shader form read as a node graph (the visual-DAG spike, #430).
;;!
;;! This is the read-only half of the round-trip in initiative #429: take the
;;! same (shader NAME ...) S-expression the transpiler lowers to GLSL and lower
;;! it instead to a node/edge model with a deterministic left-to-right layout —
;;! no positions stored in the source, no new asset format. The graph is a
;;! projection of the form, recomputed from the text every time.
;;!
;;! The traversal mirrors shader.scm's shader-emit: the same case dispatch over
;;! the expression algebra, but emitting nodes and dataflow edges rather than
;;! GLSL text. Where shader-emit returns a string, dag-expr returns the id of
;;! the node that produces the value, and the caller wires an edge to it.
;;!
;;! (dag-from-shader FORM) parses nothing new — it takes the already-read form
;;! and returns a laid-out graph. dag->text and dag->svg render it; both are
;;! pure functions of the graph, so the layout is deterministic.

;;! --- small list helpers (base s7 only, like shader.scm) ---

(define (dag-keep pred lst)
	(cond ((null? lst) '())
	      ((pred (car lst)) (cons (car lst) (dag-keep pred (cdr lst))))
	      (else (dag-keep pred (cdr lst)))))

(define (dag-max lst)
	(if (null? lst) 0
	    (let loop ((m (car lst)) (r (cdr lst)))
	      (cond ((null? r) m)
		    ((> (car r) m) (loop (car r) (cdr r)))
		    (else (loop m (cdr r)))))))

;;! Join STRS with SEP between them (no trailing separator).
(define (dag-join sep strs)
	(cond ((null? strs) "")
	      ((null? (cdr strs)) (car strs))
	      (else (string-append (car strs) sep (dag-join sep (cdr strs))))))

;;! --- graph accumulator ---
;;!
;;! A graph under construction is a mutable cell #(NEXT-ID NODES EDGES ENV):
;;!   NODES is a list of (ID KIND LABEL), newest first.
;;!   EDGES is a list of (FROM-ID TO-ID PORT), a directed dataflow edge.
;;!   ENV maps an identifier symbol to the id of the node that produces it, so
;;!   a reference to `model` or a varying resolves to its declaration node —
;;!   the graph analogue of shader-emit's ENV.

(define (dag-new) (vector 0 '() '() '()))
(define (dag-nodes g) (vector-ref g 1))
(define (dag-edges g) (vector-ref g 2))

(define (dag-add-node! g kind label)
	(let ((id (vector-ref g 0)))
	  (vector-set! g 0 (+ id 1))
	  (vector-set! g 1 (cons (list id kind label) (vector-ref g 1)))
	  id))

(define (dag-add-edge! g from to port)
	(vector-set! g 2 (cons (list from to port) (vector-ref g 2))))

(define (dag-bind! g sym id)
	(vector-set! g 3 (cons (cons sym id) (vector-ref g 3))))

(define (dag-lookup g sym)
	(let ((p (assq sym (vector-ref g 3))))
	  (and p (cdr p))))

(define (dag-nd-id n) (car n))
(define (dag-nd-kind n) (cadr n))
(define (dag-nd-label n) (caddr n))

;;! --- declaration nodes (the shared IO model) ---
;;!
;;! Inputs, uniform-block fields, varyings and targets each become a node and
;;! bind their name in ENV. A varying binds to a single node that the vertex
;;! stage writes (an edge in) and the fragment stage reads (edges out) — the
;;! one place the two stages join, drawn as one bridge node.

(define (dag-decl-nodes! g form)
	(for-each (lambda (i)
		    (dag-bind! g (car i)
			       (dag-add-node! g 'input
					      (string-append (symbol->string (car i))
							     " " (symbol->string (cadr i))))))
		  (dag-section form 'inputs))
	(for-each (lambda (b)
		    (for-each (lambda (f)
				(dag-bind! g (car f)
					   (dag-add-node! g 'uniform
							  (string-append (symbol->string (car f))
									 " " (symbol->string (cadr f))))))
			      (dag-block-fields b)))
		  (dag-section form 'uniforms))
	(for-each (lambda (v)
		    (dag-bind! g (car v)
			       (dag-add-node! g 'varying
					      (string-append (symbol->string (car v))
							     " " (symbol->string (cadr v))))))
		  (dag-section form 'varyings))
	(for-each (lambda (tg)
		    (dag-bind! g (car tg)
			       (dag-add-node! g 'target
					      (string-append (symbol->string (car tg))
							     " " (symbol->string (cadr tg))))))
		  (dag-section form 'targets)))

;;! The (KEY ...) subform body, skipping the leading 'shader and NAME.
(define (dag-section form key)
	(let ((p (assq key (cddr form))))
	  (if p (cdr p) '())))

;;! (block N)/(layout X) are options; the rest are (NAME TYPE) fields.
(define (dag-block-fields block)
	(dag-keep (lambda (f) (not (memq (car f) '(block layout))))
		  (cdr block)))

;;! --- expression walker ---
;;!
;;! Returns the id of the node that produces E, adding op/const/ref nodes and
;;! wiring an edge from each argument's producer into the op. This is the
;;! node-emitting twin of shader-emit's case dispatch.

(define (dag-expr! g e)
	(cond
	  ((number? e) (dag-add-node! g 'const (dag-num e)))
	  ((symbol? e)
	   (or (dag-lookup g e)
	       (dag-add-node! g 'ref (symbol->string e))))
	  ((pair? e)
	   (let ((op (car e)) (args (cdr e)))
	     (if (eq? op 'swizzle)
		 (let ((id (dag-add-node! g 'op
				(string-append "swizzle ." (symbol->string (cadr args))))))
		   (dag-add-edge! g (dag-expr! g (car args)) id "")
		   id)
		 (let ((id (dag-add-node! g 'op (symbol->string op))))
		   (for-each (lambda (a) (dag-add-edge! g (dag-expr! g a) id "")) args)
		   id))))
	  (else (dag-add-node! g 'op "?"))))

;;! A short label for a numeric literal: the fewest digits that still round-trip
;;! to the same value, so 0.35 reads as "0.35" and not the full double. This is
;;! shader-num's trick pared down to what a node label needs.
(define (dag-num n)
	(if (and (rational? n) (integer? n))
	    (number->string n)
	    (let ((x (exact->inexact n)))
	      (let loop ((p 1))
		(let ((s (format #f (string-append "~," (number->string p) "g") x)))
		  (if (or (>= p 17) (= (string->number s) x))
		      s
		      (loop (+ p 1))))))))

;;! --- statement walker ---
;;!
;;! (set TARGET EXPR) wires EXPR's producer into the target node (a declared
;;! varying/target, or a fresh node for `position`). (let* BINDINGS BODY)
;;! creates a local node per binding, edges its producer in, and binds the name
;;! so later statements reference it — the same lexical carry-through the
;;! transpiler does, but as graph edges.

(define (dag-stmt! g s)
	(case (car s)
	  ((set)
	   (let* ((target (cadr s))
		  (src (dag-expr! g (caddr s)))
		  (tid (or (dag-lookup g target)
			   (dag-add-node! g 'target (symbol->string target)))))
	     (dag-add-edge! g src tid "")))
	  ((let*)
	   (for-each (lambda (b)
		       (let* ((src (dag-expr! g (cadr b)))
			      (lid (dag-add-node! g 'local (symbol->string (car b)))))
			 (dag-add-edge! g src lid "")
			 (dag-bind! g (car b) lid)))
		     (cadr s))
	   (for-each (lambda (st) (dag-stmt! g st)) (cddr s)))
	  (else #f)))

;;! Walk a stage body (the statements after the stage keyword), if present.
(define (dag-stage! g form stage)
	(let ((body (assq stage (cddr form))))
	  (if body (for-each (lambda (s) (dag-stmt! g s)) (cdr body)))))

;;! --- build ---
;;!
;;! One graph for the whole shader: declare the IO once, then walk vertex then
;;! fragment sharing ENV, so a varying written in vertex and read in fragment
;;! is a single node bridging the two stages.

(define (dag-from-shader form)
	(let ((g (dag-new)))
	  (dag-decl-nodes! g form)
	  (dag-stage! g form 'vertex)
	  (dag-stage! g form 'fragment)
	  g))

;;! --- layered auto-layout ---
;;!
;;! Layer = longest path from a source (a node with no incoming edge), so
;;! dataflow runs strictly left to right. Rows order nodes within a layer by
;;! ascending id, which is creation order — declarations first, then the
;;! expression tree in the order the transpiler would read it. Both are pure
;;! functions of the graph, so the layout never depends on stored positions.

(define (dag-preds edges id)
	(map car (dag-keep (lambda (e) (= (cadr e) id)) edges)))

(define (dag-layers g)
	(let ((edges (dag-edges g)) (memo '()))
	  (define (layer id)
	    (let ((c (assv id memo)))
	      (if c (cdr c)
		  (let* ((ps (dag-preds edges id))
			 (v (if (null? ps) 0 (+ 1 (dag-max (map layer ps))))))
		    (set! memo (cons (cons id v) memo))
		    v))))
	  (map (lambda (n) (cons (dag-nd-id n) (layer (dag-nd-id n))))
	       (dag-nodes g))))

;;! Ascending-id node list (creation order); dag-nodes is newest-first.
(define (dag-nodes-asc g) (reverse (dag-nodes g)))

;;! Row index of each node within its layer, ids ascending: id -> row.
(define (dag-rows g layers)
	(let* ((asc (dag-nodes-asc g))
	       (maxl (dag-max (map cdr layers))))
	  (let loop ((l 0) (acc '()))
	    (if (> l maxl) acc
		(loop (+ l 1)
		      (let scan ((ns asc) (r 0) (a acc))
			(cond ((null? ns) a)
			      ((= (cdr (assv (dag-nd-id (car ns)) layers)) l)
			       (scan (cdr ns) (+ r 1)
				     (cons (cons (dag-nd-id (car ns)) r) a)))
			      (else (scan (cdr ns) r a)))))))))

;;! --- text render (the test oracle + console view) ---

(define (dag->text g)
	(let* ((layers (dag-layers g))
	       (rows (dag-rows g layers))
	       (asc (dag-nodes-asc g))
	       (maxl (dag-max (map cdr layers))))
	  (string-append
	    (let col ((l 0) (out ""))
	      (if (> l maxl) out
		  (col (+ l 1)
		       (string-append out
			 "layer " (number->string l) ":\n"
			 (apply string-append
			   (map (lambda (n)
				  (let ((id (dag-nd-id n)))
				    (if (= (cdr (assv id layers)) l)
					(string-append "  #" (number->string id) " "
						       (symbol->string (dag-nd-kind n)) " "
						       (dag-nd-label n) "\n")
					"")))
				asc))))))
	    "edges: " (number->string (length (dag-edges g))) "\n")))

;;! --- svg render (the visual artifact) ---
;;!
;;! A self-contained SVG of the laid-out graph: layer -> column, row -> stack,
;;! nodes as rounded boxes coloured by kind, edges as cubic beziers from a
;;! node's right edge to its consumer's left edge. Coordinates come straight
;;! from the layout above, so what the SVG shows is exactly what a live canvas
;;! (#432) would place.

(define dag-colw 190)
(define dag-rowh 62)
(define dag-boxw 150)
(define dag-boxh 38)
(define dag-margin 24)

(define (dag-kind-fill kind)
	(case kind
	  ((input) "#2f6d3b") ((uniform) "#274b7a") ((varying) "#5a3a7a")
	  ((target) "#8a3b2f") ((local) "#2f6d6d") ((const) "#3a3f47")
	  (else "#41474f")))

(define (dag-node-x layers id)
	(+ dag-margin (* (cdr (assv id layers)) dag-colw)))

(define (dag-node-y rows id)
	(+ dag-margin (* (cdr (assv id rows)) dag-rowh)))

(define (dag-svg-edges g layers rows)
	(apply string-append
	  (map (lambda (e)
		 (let* ((x1 (+ (dag-node-x layers (car e)) dag-boxw))
			(y1 (+ (dag-node-y rows (car e)) (quotient dag-boxh 2)))
			(x2 (dag-node-x layers (cadr e)))
			(y2 (+ (dag-node-y rows (cadr e)) (quotient dag-boxh 2)))
			(mx (quotient (+ x1 x2) 2)))
		   (string-append
		     "<path d=\"M " (number->string x1) " " (number->string y1)
		     " C " (number->string mx) " " (number->string y1)
		     " " (number->string mx) " " (number->string y2)
		     " " (number->string x2) " " (number->string y2)
		     "\" fill=\"none\" stroke=\"#8b929b\" stroke-width=\"1.5\"/>\n")))
	       (dag-edges g))))

(define (dag-svg-nodes g layers rows)
	(apply string-append
	  (map (lambda (n)
		 (let* ((id (dag-nd-id n))
			(x (dag-node-x layers id))
			(y (dag-node-y rows id)))
		   (string-append
		     "<g>\n<rect x=\"" (number->string x) "\" y=\"" (number->string y)
		     "\" width=\"" (number->string dag-boxw)
		     "\" height=\"" (number->string dag-boxh)
		     "\" rx=\"6\" fill=\"" (dag-kind-fill (dag-nd-kind n))
		     "\" stroke=\"#aeb4bd\" stroke-width=\"1\"/>\n"
		     "<text x=\"" (number->string (+ x 10))
		     "\" y=\"" (number->string (+ y 16))
		     "\" font-family=\"monospace\" font-size=\"11\" fill=\"#e8eaed\">"
		     (dag-nd-label n) "</text>\n"
		     "<text x=\"" (number->string (+ x 10))
		     "\" y=\"" (number->string (+ y 30))
		     "\" font-family=\"monospace\" font-size=\"9\" fill=\"#aeb4bd\">"
		     (symbol->string (dag-nd-kind n)) "</text>\n</g>\n")))
	       (dag-nodes-asc g))))

(define (dag->svg g)
	(let* ((layers (dag-layers g))
	       (rows (dag-rows g layers))
	       (maxl (dag-max (map cdr layers)))
	       (maxr (dag-max (map cdr rows)))
	       (w (+ (* 2 dag-margin) (* (+ maxl 1) dag-colw)))
	       (h (+ (* 2 dag-margin) (* (+ maxr 1) dag-rowh))))
	  (string-append
	    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" (number->string w)
	    "\" height=\"" (number->string h) "\" viewBox=\"0 0 "
	    (number->string w) " " (number->string h) "\">\n"
	    "<rect width=\"100%\" height=\"100%\" fill=\"#12151a\"/>\n"
	    (dag-svg-edges g layers rows)
	    (dag-svg-nodes g layers rows)
	    "</svg>\n")))
