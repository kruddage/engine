; SPDX-License-Identifier: GPL-2.0-or-later

(define krudd-root (or (getenv "KRUDD_ROOT") "."))

(load (string-append krudd-root "/krudd/engine/shader/dag.scm"))

(define fail-count 0)

(define (check name ok)
	(if ok
	    (display (string-append "  ok    " name "\n"))
	    (begin
	      (set! fail-count (+ fail-count 1))
	      (display (string-append "  FAIL  " name "\n")))))

(define (has? s sub)
	(let ((hl (string-length s)) (nl (string-length sub)))
	  (let loop ((i 0))
	    (cond ((> (+ i nl) hl) #f)
		  ((string=? (substring s i (+ i nl)) sub) #t)
		  (else (loop (+ i 1)))))))

;;! The built-in scene shader — the exact form asset_plugin seeds and shader.scm
;;! transpiles, read here as a graph instead of lowered to GLSL.
(define scene "(shader scene
  (inputs (a_pos vec3 (location 0)) (a_normal vec3 (location 1)) (a_uv0 vec2 (location 2)))
  (uniforms (Camera (block 0) (layout std140) (view_proj mat4) (model mat4))
            (Material (block 1) (layout std140) (base_color vec4)))
  (varyings (v_normal vec3))
  (targets (frag_color vec4 (location 0)))
  (vertex
    (set v_normal (* (mat3 model) a_normal))
    (set position (* view_proj model (vec4 a_pos 1.0))))
  (fragment
    (let* ((n (normalize v_normal))
           (base (+ 0.5 (* 0.5 n)))
           (diff (max (dot n (normalize (vec3 0.5 0.8 0.4))) 0.0))
           (col (* base (+ 0.35 (* 0.65 diff)))))
      (set frag_color (vec4 (* col (swizzle base_color rgb)) 1.0)))))")

(define form (with-input-from-string scene (lambda () (read))))
(define g (dag-from-shader form))
(define layers (dag-layers g))

;;! --- query helpers over the built graph ---

(define (node-by-label label)
	(let loop ((ns (dag-nodes g)))
	  (cond ((null? ns) #f)
		((string=? (dag-nd-label (car ns)) label) (car ns))
		(else (loop (cdr ns))))))

(define (kind-of label)
	(let ((n (node-by-label label))) (and n (dag-nd-kind n))))

(define (layer-of label)
	(let ((n (node-by-label label)))
	  (and n (cdr (assv (dag-nd-id n) layers)))))

(define (max-layer) (dag-max (map cdr layers)))

(display "dag: graph builds\n")
(check "dag-from-shader returns a graph cell" (vector? g))
(check "every declared input/uniform/varying/target became a node"
       (and (node-by-label "a_pos vec3")
	    (node-by-label "view_proj mat4")
	    (node-by-label "base_color vec4")
	    (node-by-label "v_normal vec3")
	    (node-by-label "frag_color vec4")))

(display "dag: node kinds\n")
(check "a vertex input is kind input" (eq? (kind-of "a_pos vec3") 'input))
(check "a uniform-block field is kind uniform" (eq? (kind-of "model mat4") 'uniform))
(check "a varying is kind varying" (eq? (kind-of "v_normal vec3") 'varying))
(check "a declared target is kind target" (eq? (kind-of "frag_color vec4") 'target))
(check "a let* binding is kind local" (eq? (kind-of "col") 'local))
(check "an expression op became a node" (eq? (kind-of "dot") 'op))
(check "a synthesized position target exists" (eq? (kind-of "position") 'target))

(display "dag: expression algebra lowers node-by-node\n")
(check "the mat3 constructor is a node" (node-by-label "mat3"))
(check "normalize is a node" (node-by-label "normalize"))
(check "the swizzle carries its components in the label"
       (node-by-label "swizzle .rgb"))
(check "each let* local is present" (and (node-by-label "n") (node-by-label "base")
					 (node-by-label "diff") (node-by-label "col")))

(display "dag: layered layout runs dataflow left to right\n")
(check "declaration inputs sit at layer 0" (= (layer-of "a_pos vec3") 0))
(check "uniforms sit at layer 0" (= (layer-of "model mat4") 0))
(check "the varying is not a source — vertex writes it, so its layer > 0"
       (> (layer-of "v_normal vec3") 0))
(check "the fragment output sinks at the deepest layer"
       (= (layer-of "frag_color vec4") (max-layer)))
(check "the local n is downstream of the varying it reads"
       (> (layer-of "n") (layer-of "v_normal vec3")))
(check "col is downstream of the n it (transitively) depends on"
       (> (layer-of "col") (layer-of "n")))

(display "dag: layout is deterministic\n")
(check "two independent builds render byte-identical text"
       (string=? (dag->text (dag-from-shader form))
		 (dag->text (dag-from-shader form))))
(check "two independent builds render byte-identical svg"
       (string=? (dag->svg (dag-from-shader form))
		 (dag->svg (dag-from-shader form))))

(display "dag: renders\n")
(let ((txt (dag->text g)) (svg (dag->svg g)))
  (check "text render lists layers and an edge count"
	 (and (has? txt "layer 0:") (has? txt "edges: ")))
  (check "svg render is a self-contained <svg> with node boxes"
	 (and (has? svg "<svg") (has? svg "</svg>") (has? svg "<rect"))))

(display (string-append "\ndag: nodes=" (number->string (length (dag-nodes g)))
			" edges=" (number->string (length (dag-edges g)))
			" layers=0.." (number->string (max-layer)) "\n"))

(if (> fail-count 0)
    (begin (display (string-append "\ndag: " (number->string fail-count) " FAILED\n"))
	   (exit 1))
    (display "\ndag: all checks passed\n"))
