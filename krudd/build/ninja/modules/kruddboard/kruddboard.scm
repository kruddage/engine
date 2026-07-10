; SPDX-License-Identifier: GPL-2.0-or-later

;;! kruddboard — Scheme-authored panels for the engine debug board.
;;!
;;! The C++ host (kruddboard.cpp) registers the imgui-* drawing primitives and
;;! the krudd-stats accessor against the shared s7 interpreter, loads this image
;;! once, then calls the panel procedures below from inside the ImGui frame. A
;;! primitive only records a draw call — it must run while a frame is open — so
;;! the host invokes these procedures at draw time, never at load time.
;;!
;;! This is the first panel to move out of kruddboard.cpp; the remaining tabs
;;! follow the same shape (see the "strangle kruddboard into Scheme" epic). The
;;! C draw_tab_stats this replaces read three fields off the stats subsystem and
;;! laid them out with ImGui::Text; the labels and spacing are preserved here.

;;! (krudd-stats) hands back #f when the stats subsystem is absent, otherwise the
;;! list (fps frame-ms frame-count). The #f branch mirrors the old C null check.
(define (kruddboard-draw-stats)
  (let ((s (krudd-stats)))
    (if (not s)
	(imgui-text-disabled "(stats unavailable)")
	(begin
	  (imgui-text (format #f "FPS (avg): ~,1F" (car s)))
	  (imgui-text (format #f "Frame ms:  ~,2F" (cadr s)))
	  (imgui-text (format #f "Frame:     ~D"   (caddr s)))))))

;;! (kruddboard-draw-subsystem-row r) draws one subsystem: its name, then
;;! yes/- for whether it exposes an API and a tick. r is a (name api? tick?
;;! wasm-size) list; wasm-size is no longer shown (#kruddboard, WASM Size
;;! column removed) but stays in the row shape since krudd-subsystems is a
;;! shared data accessor.
(define (kruddboard-draw-subsystem-row r)
  (let ((name     (car r))
	(has-api  (cadr r))
	(has-tick (caddr r)))
    (imgui-table-next-row)
    (imgui-table-next-column)
    (imgui-text name)
    (imgui-table-next-column)
    (imgui-text (if has-api "yes" "-"))
    (imgui-table-next-column)
    (imgui-text (if has-tick "yes" "-"))))

;;! (kruddboard-draw-subsystems) renders the subsystem manager's entries as a
;;! Name / API / Tick table. (krudd-subsystems) returns one (name api? tick?
;;! wasm-size) row per subsystem in table order (static then dynamic), or #f
;;! when the manager is absent; the #f branch mirrors the old null check.
(define (kruddboard-draw-subsystems)
  (let ((rows (krudd-subsystems)))
    (if (not rows)
	(imgui-text-disabled "(subsystem manager unavailable)")
	(when (imgui-begin-table "##subsys" 3)
	  (imgui-table-setup-column "Name")
	  (imgui-table-setup-column "API")
	  (imgui-table-setup-column "Tick")
	  (imgui-table-headers-row)
	  (for-each kruddboard-draw-subsystem-row rows)
	  (imgui-end-table)))))

;;! Log view state persists across frames the way the old C statics did: the
;;! active level filter (a log_level integer, DEBUG=0) and whether the view
;;! auto-scrolls to the newest line.
(define kruddboard-log-filter 0)
(define kruddboard-log-autoscroll #t)

;;! Per-level text colours indexed by log_level: DEBUG grey, INFO white, WARN
;;! yellow, ERROR red — the same RGBA the old level_colors table carried.
(define kruddboard-log-colors
  (vector (list 0.6 0.6 0.6 1.0)
	  (list 1.0 1.0 1.0 1.0)
	  (list 1.0 0.8 0.2 1.0)
	  (list 1.0 0.3 0.3 1.0)))

;;! (kruddboard-draw-log-line m) draws one history entry: skip it when its
;;! level is below the active filter, otherwise draw the text in the level's
;;! colour. m is a (level . text) pair.
(define (kruddboard-draw-log-line m)
  (let ((level (car m))
	(text  (cdr m)))
    (when (>= level kruddboard-log-filter)
      (let ((c (vector-ref kruddboard-log-colors level)))
	(imgui-text-colored (car c) (cadr c) (caddr c) (cadddr c) text)))))

;;! (kruddboard-draw-log) draws the level-filter buttons, the auto-scroll
;;! toggle, and the scrolling history child — the C draw_tab_log ported whole.
;;! (krudd-log-history) hands back a list of (level . text) pairs oldest-first,
;;! or #f when the log subsystem is absent; the #f branch mirrors the old null
;;! check. The scroll region caps at ~88% of the viewport height minus the
;;! controls, floored at 80px, as the C computed it.
(define (kruddboard-draw-log)
  (let ((hist (krudd-log-history)))
    (if (not hist)
	(imgui-text-disabled "(log unavailable)")
	(begin
	  (when (imgui-small-button "DEBUG") (set! kruddboard-log-filter 0))
	  (imgui-same-line)
	  (when (imgui-small-button "INFO")  (set! kruddboard-log-filter 1))
	  (imgui-same-line)
	  (when (imgui-small-button "WARN")  (set! kruddboard-log-filter 2))
	  (imgui-same-line)
	  (when (imgui-small-button "ERROR") (set! kruddboard-log-filter 3))
	  (imgui-same-line)
	  (set! kruddboard-log-autoscroll
		(imgui-checkbox "Auto-scroll" kruddboard-log-autoscroll))
	  (imgui-separator)
	  (let ((scroll-h (max 80.0
			       (- (* (imgui-viewport-work-height) 0.88)
				  120.0))))
	    (imgui-begin-child "##logscroll" 0.0 scroll-h)
	    (for-each kruddboard-draw-log-line hist)
	    (when kruddboard-log-autoscroll (imgui-set-scroll-here-y 1.0))
	    (imgui-end-child))))))

;;! (kruddboard-draw-krudd) is the whole KRUDD tab: frame stats, subsystems,
;;! and log, each under its own collapsing header — the Scheme composition
;;! that replaces the C draw_tab_krudd. Frame Stats and Log default open;
;;! Subsystems starts rolled up since its table is rarely needed at a glance.
(define (kruddboard-draw-krudd)
  (when (imgui-collapsing-header "Frame Stats") (kruddboard-draw-stats))
  (when (imgui-collapsing-header "Subsystems" #f) (kruddboard-draw-subsystems))
  (when (imgui-collapsing-header "Log")         (kruddboard-draw-log)))

;;! World tab — the entity list and inspector, ported from the C draw_tab_world.
;;! This is the first tab that mutates engine state: it creates and destroys
;;! entities, edits the selected entity's name and transform, and rebinds its
;;! mesh and material assets. Every mutation goes through a krudd-entity-*
;;! primitive that drives the scene api, which records an undo step, exactly as
;;! the C did. The transform gizmo overlay stays in C++ (draw_board); only its
;;! Move/Rotate/Scale tool chips live here, over the shared krudd-gizmo-mode.

;;! (kruddboard-world-name id name) is an entity's display label: its name, or
;;! an "entity N" fallback when it has none — the C snprintf fallback.
(define (kruddboard-world-name id name)
  (if (string? name) name (format #f "entity ~D" id)))

;;! (kruddboard-draw-world-header) draws the scene title and the right-aligned,
;;! always-disabled "Save As..." placeholder the C header drew.
(define (kruddboard-draw-world-header)
  (imgui-text "Untitled Scene")
  (imgui-same-line-right (imgui-calc-text-width "Save As..."))
  (imgui-begin-disabled #t)
  (imgui-small-button "Save As...")
  (imgui-end-disabled))

;;! (kruddboard-draw-world-entity row has-api) draws one entity-list row: a
;;! full-width selectable that sets the selection, then a fixed delete button
;;! (disabled without the scene api). row is an (id . name) pair; the selection
;;! is re-read each row so a click earlier in the frame is reflected.
(define (kruddboard-draw-world-entity row has-api)
  (let* ((id   (car row))
	 (disp (kruddboard-world-name id (cdr row))))
    (imgui-table-next-row)
    (imgui-table-next-column)
    (when (imgui-selectable (format #f "~A##e~D" disp id)
			    (= id (krudd-selected)) #t)
      (krudd-entity-select id))
    (imgui-table-next-column)
    (imgui-begin-disabled (not has-api))
    (when (imgui-small-button (format #f "x##d~D" id))
      (krudd-entity-destroy id))
    (imgui-end-disabled)))

;;! (kruddboard-world-create) appends an entity, names it "Entity", and selects
;;! it — the three steps behind the "+ Entity" button.
(define (kruddboard-world-create)
  (let ((id (krudd-entity-create)))
    (when (>= id 0)
      (krudd-entity-set-name id "Entity")
      (krudd-entity-select id))))

;;! (kruddboard-draw-world-entities has-api) draws the "+ Entity" button and the
;;! two-column entity table, or a dimmed "(no entities)" when the world is empty
;;! or absent. (krudd-world-entities) hands back ((id . name) ...) or #f.
(define (kruddboard-draw-world-entities has-api)
  (imgui-begin-disabled (not has-api))
  (when (imgui-small-button "+ Entity")
    (kruddboard-world-create))
  (imgui-end-disabled)
  (let ((ents (krudd-world-entities)))
    (if (or (not ents) (null? ents))
	(imgui-text-disabled "(no entities)")
	(when (imgui-begin-table "##entlist" 2)
	  (imgui-table-setup-column "Name")
	  (imgui-table-setup-column-fixed "" 24.0)
	  (for-each (lambda (row) (kruddboard-draw-world-entity row has-api))
		    ents)
	  (imgui-end-table)))))

;;! Move/Rotate/Scale tool chips. The active chip is tinted the same blue the C
;;! draw_gizmo_mode_chips pushed (IM_COL32 70,110,170); clicking one sets the
;;! shared gizmo mode the viewport handles read.
(define kruddboard-gizmo-names (vector "Move" "Rotate" "Scale"))

(define (kruddboard-draw-gizmo-chips)
  (imgui-text "Tool")
  (imgui-same-line)
  (let ((mode (krudd-gizmo-mode)))
    (do ((m 0 (+ m 1))) ((= m 3))
      (let ((active (= mode m)))
	(when (> m 0) (imgui-same-line))
	(when active
	  (imgui-push-style-color-button (/ 70.0 255) (/ 110.0 255)
					 (/ 170.0 255) 1.0))
	(when (imgui-small-button (vector-ref kruddboard-gizmo-names m))
	  (krudd-set-gizmo-mode m))
	(when active (imgui-pop-style-color))))))

;;! One "Label: value" row in a borderless inspector table.
(define (kruddboard-draw-detail-row label value)
  (imgui-table-next-row)
  (imgui-table-next-column)
  (imgui-text label)
  (imgui-table-next-column)
  (imgui-text value))

;;! (kruddboard-parent-label parent) formats the Parent detail: "(root)" for a
;;! root, "name (#id)" for a named parent, else "entity id". parent is #f or an
;;! (id . name-or-#f) pair.
(define (kruddboard-parent-label parent)
  (if (not parent)
      "(root)"
      (let ((pid (car parent))
	    (pname (cdr parent)))
	(if (string? pname)
	    (format #f "~A (#~D)" pname pid)
	    (format #f "entity ~D" pid)))))

;;! (kruddboard-components-label ...) is the "Transform, Name, Render, Material,
;;! Script" summary, listing only the components the entity actually carries.
(define (kruddboard-components-label has-name has-render has-material
				     has-script)
  (string-append "Transform"
		 (if has-name     ", Name"     "")
		 (if has-render   ", Render"   "")
		 (if has-material ", Material" "")
		 (if has-script   ", Script"   "")))

;;! (kruddboard-draw-inspector-details ...) is the read-only id / parent /
;;! components table.
(define (kruddboard-draw-inspector-details e parent has-name has-render
					   has-material has-script)
  (when (imgui-begin-table-plain "##edetails" 2)
    (imgui-table-setup-column-fixed "" 80.0)
    (imgui-table-setup-column "")
    (kruddboard-draw-detail-row "Entity ID" (format #f "~D" e))
    (kruddboard-draw-detail-row "Parent" (kruddboard-parent-label parent))
    (kruddboard-draw-detail-row "Components"
	(kruddboard-components-label has-name has-render has-material
				     has-script))
    (imgui-end-table)))

;;! One transform row: a fixed label cell and a full-width float input. Returns
;;! the input's (vec . changed?) so the caller can OR the change flags.
(define (kruddboard-draw-xform-row label id vec four?)
  (imgui-table-next-row)
  (imgui-table-next-column)
  (imgui-text label)
  (imgui-table-next-column)
  (imgui-set-next-item-width -1.0)
  (if four?
      (imgui-input-float4 id vec)
      (imgui-input-float3 id vec)))

;;! (kruddboard-draw-xform-table pos rot scl) draws the Position / Rotation /
;;! Scale rows and returns (changed? new-pos new-rot new-scl). Rotation is a
;;! four-component quaternion; position and scale are three-vectors.
(define (kruddboard-draw-xform-table pos rot scl)
  (if (not (imgui-begin-table-plain "##xform" 2))
      (list #f pos rot scl)
      (begin
	(imgui-table-setup-column-fixed "" 64.0)
	(imgui-table-setup-column "")
	(let ((pr (kruddboard-draw-xform-row "Position" "##pos" pos #f))
	      (rr (kruddboard-draw-xform-row "Rotation" "##rot" rot #t))
	      (sr (kruddboard-draw-xform-row "Scale"    "##scl" scl #f)))
	  (imgui-end-table)
	  (list (or (cdr pr) (cdr rr) (cdr sr))
		(car pr) (car rr) (car sr))))))

;;! (kruddboard-binding-label bound? ref) is the combo preview: "(none)" when
;;! unbound, the asset's path when it resolves, else "(missing #ref)".
(define (kruddboard-binding-label bound? ref)
  (if (not bound?)
      "(none)"
      (let ((path (krudd-asset-find ref)))
	(if (string? path) path (format #f "(missing #~D)" ref)))))

;;! (kruddboard-draw-binding-combo ...) is the open dropdown body shared by the
;;! mesh and material rows: a "(none)" unbind entry, then one selectable per
;;! candidate asset, with the current one focused. setter is a krudd-entity-set-
;;! *-ref primitive; assets is an ((id . path) ...) list.
(define (kruddboard-draw-binding-combo combo-id bound? ref assets can-edit
				       setter e)
  (when (imgui-begin-combo combo-id (kruddboard-binding-label bound? ref))
    (when (and (imgui-selectable "(none)" (not bound?) #f) can-edit)
      (setter e 0))
    (for-each
     (lambda (a)
       (let ((aid (car a))
	     (cur (and bound? (= (car a) ref))))
	 (when (and (imgui-selectable (format #f "~A##m~D" (cdr a) aid) cur #f)
		    can-edit)
	   (setter e aid))
	 (when cur (imgui-set-item-default-focus))))
     assets)
    (imgui-end-combo)))

;;! (kruddboard-draw-inspector-binding ...) draws one "Label: <combo>" binding
;;! row (mesh or material) in its own borderless table, disabled when it cannot
;;! be edited.
(define (kruddboard-draw-inspector-binding label table-id combo-id e bound? ref
					   assets can-edit setter)
  (when (imgui-begin-table-plain table-id 2)
    (imgui-table-setup-column-fixed "" 80.0)
    (imgui-table-setup-column "")
    (imgui-table-next-row)
    (imgui-table-next-column)
    (imgui-text label)
    (imgui-table-next-column)
    (imgui-set-next-item-width -1.0)
    (imgui-begin-disabled (not can-edit))
    (kruddboard-draw-binding-combo combo-id bound? ref assets can-edit setter e)
    (imgui-end-disabled)
    (imgui-end-table)))

;;! One script-parameter widget, chosen by edit hint then component count — the
;;! entity-menu twin of the material editor's param widget. PARAM is the 8-tuple
;;! (name type off size comps kind min max); VALUE is its current component list.
;;! Returns (new-value . changed?): new-value stays a list so the save path packs
;;! it uniformly. A color drives a swatch, a range a slider, anything else a
;;! plain float input; a type with no editable components passes through.
(define (kruddboard-draw-param-widget param value)
  (let ((comps (list-ref param 4))
        (kind  (list-ref param 5))
        (mn    (list-ref param 6))
        (mx    (list-ref param 7))
        (wid   (string-append (list-ref param 0) "##sp")))
    (cond
     ((and (string=? kind "color") (= comps 4)) (imgui-color-edit4 wid value))
     ((and (string=? kind "color") (= comps 3)) (imgui-color-edit3 wid value))
     ((and (string=? kind "range") (= comps 1))
      (let ((r (imgui-slider-float wid (car value) mn mx)))
        (cons (list (car r)) (cdr r))))
     ((= comps 1)
      (let ((r (imgui-input-float wid (car value))))
        (cons (list (car r)) (cdr r))))
     ((= comps 2) (imgui-input-float2 wid value))
     ((= comps 3) (imgui-input-float3 wid value))
     ((= comps 4) (imgui-input-float4 wid value))
     (else (cons value #f)))))

;;! True when any (value . changed?) result in RESULTS is changed. Plain fold —
;;! every widget already drew once in the map, so this never touches the UI.
(define (kruddboard-any-param-changed results)
  (let loop ((r results))
    (cond ((null? r) #f)
          ((cdr (car r)) #t)
          (else (loop (cdr r))))))

;;! (kruddboard-draw-script-params e script-ref can-edit) draws the bound
;;! script's authored parameters as live entity-menu widgets — the per-entity
;;! override layer over the script's declared params, mirroring the material
;;! editor. Drawn only when the script declares params; any edit is packed and
;;! saved immediately (through the scene api's undo-recording setter), so the
;;! script picks up the new value on its next tick — no explicit Save.
(define (kruddboard-draw-script-params e script-ref can-edit)
  (let ((params (krudd-script-params script-ref)))
    (unless (null? params)
      (imgui-separator)
      (when (imgui-collapsing-header "Script Parameters")
        (let ((values (krudd-entity-script-values e script-ref)))
          (imgui-begin-disabled (not can-edit))
          (let* ((results  (map kruddboard-draw-param-widget params values))
                 (new-vals (map car results))
                 (changed  (kruddboard-any-param-changed results)))
            (imgui-end-disabled)
            (when (and can-edit changed)
              (krudd-entity-save-script-params e script-ref new-vals))))))))

;;! (kruddboard-draw-inspector-body e info caps) draws the editable name field
;;! and transform table (both disabled without the scene api), then the read-only
;;! details and the mesh/material/script binding rows. info is the
;;! krudd-entity-inspect bundle; caps is (entity-api? asset-api?). The name
;;! commits once on focus loss and the transform writes back after the disabled
;;! block, as the C did.
(define (kruddboard-draw-inspector-body e info caps)
  (let ((name         (list-ref info 0))
	(pos          (list-ref info 1))
	(rot          (list-ref info 2))
	(scl          (list-ref info 3))
	(parent       (list-ref info 4))
	(has-name     (list-ref info 5))
	(has-render   (list-ref info 6))
	(has-material (list-ref info 7))
	(render-ref   (list-ref info 8))
	(material-ref (list-ref info 9))
	(has-script   (list-ref info 10))
	(script-ref   (list-ref info 11))
	(has-entity   (car caps))
	(has-asset    (cadr caps)))
    (imgui-begin-disabled (not has-entity))
    (imgui-set-next-item-width -1.0)
    (let ((edit (imgui-input-text "##ename" name)))
      (when (cdr edit)
	(krudd-entity-set-name e (car edit))))
    (imgui-separator)
    (let ((xf (kruddboard-draw-xform-table pos rot scl)))
      (imgui-end-disabled)
      (when (car xf)
	(krudd-entity-set-transform e (cadr xf) (caddr xf) (cadddr xf))))
    (imgui-separator)
    (kruddboard-draw-inspector-details e parent has-name has-render
				       has-material has-script)
    (let ((can-bind (and has-entity has-asset)))
      (kruddboard-draw-inspector-binding "Mesh" "##emesh" "##meshsel" e
	  has-render render-ref (krudd-mesh-assets) can-bind
	  krudd-entity-set-render-ref)
      (kruddboard-draw-inspector-binding "Material" "##ematerial"
	  "##materialsel" e has-material material-ref (krudd-material-assets)
	  can-bind krudd-entity-set-material-ref)
      (kruddboard-draw-inspector-binding "Script" "##escript" "##scriptsel" e
	  has-script script-ref (krudd-script-assets) can-bind
	  krudd-entity-set-script-ref)
      (when has-script
	(kruddboard-draw-script-params e script-ref can-bind)))))

;;! (kruddboard-draw-world-inspector caps) shows the selected entity's inspector,
;;! or a dimmed "(nothing selected)" when nothing live is selected.
;;! (krudd-entity-inspect) returns #f for no / stale selection.
(define (kruddboard-draw-world-inspector caps)
  (let* ((sel  (krudd-selected))
	 (info (and (>= sel 0) (krudd-entity-inspect sel))))
    (if (not info)
	(imgui-text-disabled "(nothing selected)")
	(kruddboard-draw-inspector-body sel info caps))))

;;! (kruddboard-draw-world) is the whole World tab: scene header, entity list,
;;! gizmo tool chips, and inspector — the Scheme composition that replaces the C
;;! draw_tab_world.
(define (kruddboard-draw-world)
  (let* ((caps       (krudd-world-caps))
	 (has-entity (car caps)))
    (kruddboard-draw-world-header)
    (imgui-separator)
    (when (imgui-collapsing-header "Entities")
      (kruddboard-draw-world-entities has-entity))
    (imgui-separator)
    (kruddboard-draw-gizmo-chips)
    (imgui-separator)
    (when (imgui-collapsing-header "Inspector")
      (kruddboard-draw-world-inspector caps))))
