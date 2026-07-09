; SPDX-License-Identifier: GPL-2.0-or-later

;;! kruddboard Assets tab — asset browser, markdown editor, and asset
;;! inspector, ported from kruddboard.cpp's draw_tab_assets / draw_asset_
;;! inspector (#402, the last and biggest tab in the "strangle kruddboard
;;! into Scheme" epic — see #397).
;;!
;;! The C++ host (kruddboard.cpp) registers the asset-mutation and text-edit
;;! primitives used here against the shared s7 interpreter, loads this image
;;! right after kruddboard.scm, then calls kruddboard-draw-assets from inside
;;! the Assets tab. This file is a self-contained unit: it keeps its own
;;! module state and small helpers rather than reaching into kruddboard.scm's,
;;! so it can move independently as the remaining strangler-fig steps land.
;;!
;;! A note on widget calls: every ImGui primitive here is a real draw call —
;;! it must run exactly once per frame regardless of its result, or the
;;! widget silently disappears. Where two such calls feed one combined
;;! boolean (e.g. "Enter in the field OR the button"), both are bound with a
;;! plain (parallel) let first and combined after, never with `or`/`and`
;;! directly on the calls — those short-circuit and would skip a draw.

;;! Selected asset id; 0 = browsing the table. Mirrors the old g_asset_sel.
(define kruddboard-assets-sel 0)

;;! Text/shader source edit buffer and the id whose bytes it holds (0 = none),
;;! reloaded only on selection change — the old g_edit / g_edit_id.
(define kruddboard-assets-edit-id 0)
(define kruddboard-assets-edit-text "")

;;! Last shader Save result: 'untried, #t (compiled), or #f (failed) — the
;;! old g_shader_compile_ok's three states.
(define kruddboard-assets-shader-ok 'untried)

;;! Material color editor buffer and the id whose bytes it holds.
(define kruddboard-assets-color-id 0)
(define kruddboard-assets-color (list 1.0 1.0 1.0 1.0))

;;! New Asset form state: visible?, the name field, and the type combo index
;;! (0 Text, 1 Shader, 2 Material).
(define kruddboard-assets-naming #f)
(define kruddboard-assets-new-name "")
(define kruddboard-assets-new-type 0)

;;! Built-in shader "Clone" form state: which built-in it's seeded from, the
;;! proposed name, and whether the last attempt hit a duplicate path.
(define kruddboard-assets-clone-src 0)
(define kruddboard-assets-clone-name "")
(define kruddboard-assets-clone-conflict #f)

;;! Label helpers. The integer codes mirror asset_api.h's ASSET_KIND_* /
;;! ASSET_TYPE_* defines and asset_api's state values (0 pending, 1 loaded,
;;! else error) — the same raw-int-from-C convention kruddboard-log-colors
;;! already uses for log_level.

(define (kruddboard-asset-state-label s)
  (cond ((= s 0) "Pending")
	((= s 1) "Loaded")
	(else "Error")))

(define (kruddboard-asset-kind-label k)
  (if (= k 1) "Primitive" "Normal"))

(define (kruddboard-asset-type-label t)
  (cond ((= t 1) "Mesh")
	((= t 2) "Texture")
	((= t 3) "Material")
	((= t 4) "Shader")
	((= t 5) "Font")
	((= t 6) "Scene")
	((= t 7) "Text")
	((= t 8) "Script")
	(else "Unknown")))

;;! (kruddboard-assets-maybe-reload-edit id) (re)loads the edit buffer from
;;! the asset catalog when the selection changes, resetting the shader-
;;! compile indicator — the old maybe_reload_edit, minus the markdown re-
;;! parse (krudd-md-preview parses fresh every draw, so there is no cached
;;! block list to invalidate here).
(define (kruddboard-assets-maybe-reload-edit id)
  (unless (= kruddboard-assets-edit-id id)
    (set! kruddboard-assets-edit-id id)
    (set! kruddboard-assets-edit-text (krudd-asset-data id))
    (set! kruddboard-assets-shader-ok 'untried)))

;;! (kruddboard-assets-maybe-reload-color id) mirrors the above for the
;;! material color editor — the old maybe_reload_material_color.
(define (kruddboard-assets-maybe-reload-color id)
  (unless (= kruddboard-assets-color-id id)
    (set! kruddboard-assets-color-id id)
    (set! kruddboard-assets-color (krudd-asset-color id))))

;;! (kruddboard-assets-do-delete id) deletes the selected asset and clears
;;! every buffer that might still be keyed to it, then returns to the browser.
(define (kruddboard-assets-do-delete id)
  (krudd-asset-delete id)
  (set! kruddboard-assets-edit-id 0)
  (set! kruddboard-assets-edit-text "")
  (set! kruddboard-assets-color-id 0)
  (set! kruddboard-assets-sel 0))

;;! One "Label: value" row in a bordered two-column table.
(define (kruddboard-assets-draw-row label value)
  (imgui-table-next-row)
  (imgui-table-next-column)
  (imgui-text label)
  (imgui-table-next-column)
  (imgui-text value))

;;! ------------------------------------------------------------------
;;! New Asset form
;;! ------------------------------------------------------------------

;;! (kruddboard-assets-create-of-type type name) dispatches to the typed
;;! creation primitive the New Asset form's type combo selected.
(define (kruddboard-assets-create-of-type type name)
  (cond ((= type 1) (krudd-asset-create-shader name))
	((= type 2) (krudd-asset-create-material name))
	(else (krudd-asset-create-text name))))

;;! (kruddboard-draw-new-asset-form) is the "New Asset" button, or (once
;;! clicked) the name field + type combo + Create/Cancel row — the old
;;! draw_tab_assets New Asset block, ported whole.
(define (kruddboard-draw-new-asset-form)
  (if (not kruddboard-assets-naming)
      (when (imgui-button "New Asset")
	(set! kruddboard-assets-naming #t)
	(set! kruddboard-assets-new-name "")
	(set! kruddboard-assets-new-type 0))
      (begin
	(imgui-set-next-item-width 240.0)
	(let ((r (imgui-input-text-enter "name" kruddboard-assets-new-name)))
	  (set! kruddboard-assets-new-name (car r))
	  (imgui-set-next-item-width 160.0)
	  (let* ((new-type (imgui-combo "type" (list "Text" "Shader" "Material")
					kruddboard-assets-new-type))
		 (create-clicked (imgui-button "Create")))
	    (set! kruddboard-assets-new-type new-type)
	    (imgui-same-line)
	    (when (imgui-button "Cancel")
	      (set! kruddboard-assets-naming #f))
	    (when (and (or (cdr r) create-clicked)
		       (not (string=? kruddboard-assets-new-name "")))
	      (let ((nid (kruddboard-assets-create-of-type
			  kruddboard-assets-new-type
			  kruddboard-assets-new-name)))
		(unless (= nid 0)
		  (set! kruddboard-assets-sel nid))
		(set! kruddboard-assets-naming #f))))))))

;;! ------------------------------------------------------------------
;;! Asset browser table
;;! ------------------------------------------------------------------

;;! (kruddboard-draw-asset-row row ro?) draws one browser row: a span-all
;;! selectable that opens the inspector, a drag source for mesh rows
;;! (drag-to-spawn, #176), and the Type/Kind/State/Size/Flags cells. row is
;;! an (id path type kind state size refs) list; ro? is #t for the built-in
;;! group (draws "RO"), #f for the project group (draws "-").
(define (kruddboard-draw-asset-row row ro?)
  (let ((id   (list-ref row 0))
	(path (list-ref row 1))
	(type (list-ref row 2))
	(kind (list-ref row 3))
	(state (list-ref row 4))
	(size (list-ref row 5)))
    (imgui-table-next-row)
    (imgui-table-next-column)
    (when (imgui-selectable path #f #t)
      (set! kruddboard-assets-sel id))
    ;;! type 1 = ASSET_TYPE_MESH.
    (when (= type 1)
      (imgui-mesh-drag-source id path))
    (imgui-table-next-column)
    (imgui-text (kruddboard-asset-type-label type))
    (imgui-table-next-column)
    (imgui-text (kruddboard-asset-kind-label kind))
    (imgui-table-next-column)
    (imgui-text (kruddboard-asset-state-label state))
    (imgui-table-next-column)
    (if (> size 0)
	(imgui-text (number->string size))
	(imgui-text-disabled "-"))
    (imgui-table-next-column)
    (if ro?
	(imgui-text-colored 1.0 0.6 0.2 1.0 "RO")
	(imgui-text-disabled "-"))))

;;! (kruddboard-draw-asset-table groups) is the six-column Path / Type /
;;! Kind / State / Size / Flags table, in two labeled groups — the old
;;! draw_tab_assets table body. groups is (builtin-rows project-rows), as
;;! krudd-assets returns.
(define (kruddboard-draw-asset-table groups)
  (let ((builtin (car groups))
	(project (cadr groups)))
    (when (imgui-begin-table "##assets" 6)
      (imgui-table-setup-column "Path")
      (imgui-table-setup-column "Type")
      (imgui-table-setup-column "Kind")
      (imgui-table-setup-column "State")
      (imgui-table-setup-column "Size")
      (imgui-table-setup-column "Flags")
      (imgui-table-headers-row)
      (unless (null? builtin)
	(imgui-table-next-row)
	(imgui-table-next-column)
	(imgui-text-disabled "-- BUILT-IN (read-only) --")
	(for-each (lambda (r) (kruddboard-draw-asset-row r #t)) builtin))
      (unless (null? project)
	(imgui-table-next-row)
	(imgui-table-next-column)
	(imgui-text-disabled "-- PROJECT --")
	(for-each (lambda (r) (kruddboard-draw-asset-row r #f)) project))
      (imgui-end-table))))

;;! ------------------------------------------------------------------
;;! Asset inspector — header + per-type editors
;;! ------------------------------------------------------------------

;;! (kruddboard-draw-asset-header info) draws the "<- Back", path, and
;;! bracketed [type | state | mutable] line every inspector screen shares.
(define (kruddboard-draw-asset-header info)
  (when (imgui-small-button "<- Back")
    (set! kruddboard-assets-sel 0))
  (imgui-same-line)
  (imgui-text (list-ref info 0))
  (imgui-same-line)
  (imgui-text-disabled
   (format #f "[~A | ~A | ~A]"
	   (kruddboard-asset-type-label (list-ref info 1))
	   (kruddboard-asset-state-label (list-ref info 3))
	   (if (list-ref info 6) "read-only" "mutable"))))

;;! Authored text (markdown) editor: source box + rendered preview + Save/
;;! Delete — the old draw_asset_inspector text branch.
(define (kruddboard-draw-asset-text-editor id)
  (kruddboard-assets-maybe-reload-edit id)
  (imgui-separator)
  (when (imgui-collapsing-header "Source")
    (let ((r (imgui-input-text-multiline "##md" kruddboard-assets-edit-text
					 200.0 #f)))
      (set! kruddboard-assets-edit-text (car r))))
  (imgui-separator)
  (when (imgui-collapsing-header "Preview")
    (krudd-md-preview kruddboard-assets-edit-text 200.0))
  (imgui-separator)
  (when (imgui-button "Save")
    (krudd-asset-save-text id kruddboard-assets-edit-text))
  (imgui-same-line)
  (when (imgui-button "Delete")
    (kruddboard-assets-do-delete id)))

;;! The Save button + compile-result text for an editable shader — the old
;;! draw_asset_inspector editable-shader branch.
(define (kruddboard-draw-asset-shader-save id)
  (when (imgui-button "Save")
    (set! kruddboard-assets-shader-ok
	  (krudd-asset-save-shader id kruddboard-assets-edit-text)))
  (imgui-same-line)
  (cond ((eq? kruddboard-assets-shader-ok #t)
	 (imgui-text-colored 0.3 0.9 0.3 1.0 "Compiled OK"))
	((eq? kruddboard-assets-shader-ok #f)
	 (imgui-text-colored 1.0 0.3 0.3 1.0 "Compile failed"))))

;;! The name field + Clone button for a read-only (built-in) shader — the old
;;! draw_asset_inspector built-in-shader branch. path seeds the default
;;! "<path>_copy" name the first time this built-in is viewed.
(define (kruddboard-draw-asset-shader-clone id path)
  (unless (= kruddboard-assets-clone-src id)
    (set! kruddboard-assets-clone-src id)
    (set! kruddboard-assets-clone-name (string-append path "_copy"))
    (set! kruddboard-assets-clone-conflict #f))
  (imgui-set-next-item-width 240.0)
  (let ((r (imgui-input-text-enter "##clonename" kruddboard-assets-clone-name)))
    (set! kruddboard-assets-clone-name (car r))
    (imgui-same-line)
    (let* ((clone-clicked (imgui-button "Clone"))
	   (confirm (or (cdr r) clone-clicked)))
      (when (and confirm (not (string=? kruddboard-assets-clone-name "")))
	(let ((nid (krudd-asset-clone-shader kruddboard-assets-clone-name
					     kruddboard-assets-edit-text)))
	  (if (= nid 0)
	      (set! kruddboard-assets-clone-conflict #t)
	      (begin
		(set! kruddboard-assets-clone-conflict #f)
		(set! kruddboard-assets-sel nid)))))
      (when kruddboard-assets-clone-conflict
	(imgui-same-line)
	(imgui-text-colored 1.0 0.3 0.3 1.0
			    (format #f "\"~A\" already exists"
				    kruddboard-assets-clone-name))))))

;;! Shader inspector: derived Declaration, Source box (editable or not), then
;;! either the Save/Delete row or the built-in Clone row — the old draw_
;;! asset_inspector shader branch.
(define (kruddboard-draw-asset-shader-editor id path editable)
  (kruddboard-assets-maybe-reload-edit id)
  (imgui-separator)
  (when (imgui-collapsing-header "Declaration")
    (let ((stages (krudd-shader-stages kruddboard-assets-edit-text)))
      (imgui-text "format: krudd-shader")
      (imgui-text (format #f "stages: ~A"
			  (if (string=? stages "") "(none)" stages)))))
  (imgui-separator)
  (when (imgui-collapsing-header "Source")
    (let ((r (imgui-input-text-multiline "##shader" kruddboard-assets-edit-text
					 260.0 (not editable))))
      (set! kruddboard-assets-edit-text (car r))))
  (imgui-separator)
  (if editable
      (begin
	(kruddboard-draw-asset-shader-save id)
	(imgui-same-line)
	(when (imgui-button "Delete")
	  (kruddboard-assets-do-delete id)))
      (kruddboard-draw-asset-shader-clone id path)))

;;! Material inspector: a color picker (disabled when read-only) plus Save/
;;! Delete or a disabled Save + "read-only" note — the old draw_asset_
;;! inspector material branch.
(define (kruddboard-draw-asset-material-editor id editable)
  (kruddboard-assets-maybe-reload-color id)
  (imgui-separator)
  (when (imgui-collapsing-header "Color")
    (imgui-begin-disabled (not editable))
    (let ((r (imgui-color-edit4 "##basecolor" kruddboard-assets-color)))
      (set! kruddboard-assets-color (car r)))
    (imgui-end-disabled))
  (imgui-separator)
  (if (not editable)
      (begin
	(imgui-begin-disabled #t)
	(imgui-button "Save")
	(imgui-end-disabled)
	(imgui-same-line)
	(imgui-text-disabled "read-only"))
      (when (imgui-button "Save")
	(apply krudd-asset-save-material id kruddboard-assets-color)))
  (when editable
    (imgui-same-line)
    (when (imgui-button "Delete")
      (kruddboard-assets-do-delete id))))

;;! Read-only fallback for every asset type without a dedicated editor: the
;;! declaration table (from describe()) and the full catalog snapshot — the
;;! old draw_asset_inspector generic tail, reached by every non-text/shader/
;;! material asset (meshes, textures, fonts, scenes).
(define (kruddboard-draw-asset-generic id info)
  (imgui-separator)
  (imgui-text "Declaration")
  (let ((fields (krudd-asset-describe id)))
    (if (null? fields)
	(imgui-text-disabled "(no declaration)")
	(when (imgui-begin-table "##decl" 2)
	  (imgui-table-setup-column "Property")
	  (imgui-table-setup-column "Value")
	  (imgui-table-headers-row)
	  (for-each (lambda (f)
		      (kruddboard-assets-draw-row (car f) (cdr f)))
		    fields)
	  (imgui-end-table))))
  (imgui-separator)
  (imgui-text "Catalog")
  (when (imgui-begin-table "##catalog" 2)
    (imgui-table-setup-column "Field")
    (imgui-table-setup-column "Value")
    (imgui-table-headers-row)
    (kruddboard-assets-draw-row "path" (list-ref info 0))
    (kruddboard-assets-draw-row "kind" (kruddboard-asset-kind-label
					(list-ref info 2)))
    (kruddboard-assets-draw-row "type" (kruddboard-asset-type-label
					(list-ref info 1)))
    (kruddboard-assets-draw-row "state" (kruddboard-asset-state-label
					 (list-ref info 3)))
    (kruddboard-assets-draw-row "size"
      (let ((sz (list-ref info 4)))
	(if (> sz 0) (number->string sz) "-")))
    (kruddboard-assets-draw-row "refs" (number->string (list-ref info 5)))
    (kruddboard-assets-draw-row "read_only"
      (if (list-ref info 6) "yes" "no"))
    (imgui-end-table)))

;;! (kruddboard-draw-asset-body id info) dispatches to the right editor by
;;! type/origin — ASSET_ORIGIN_AUTHORED=1 and ASSET_TYPE_TEXT=7/SHADER=4/
;;! MATERIAL=3, mirroring asset_api.h, the same convention the label helpers
;;! above use.
(define (kruddboard-draw-asset-body id info)
  (let ((path (list-ref info 0))
	(type (list-ref info 1))
	(read-only (list-ref info 6))
	(origin (list-ref info 7)))
    (cond
     ((and (= origin 1) (= type 7)) (kruddboard-draw-asset-text-editor id))
     ((= type 4) (kruddboard-draw-asset-shader-editor id path (not read-only)))
     ((= type 3) (kruddboard-draw-asset-material-editor id (not read-only)))
     (else (kruddboard-draw-asset-generic id info)))))

;;! (kruddboard-draw-asset-inspector id) is the whole inspector screen: the
;;! header plus the dispatched body, or a return to the browser if id no
;;! longer resolves (e.g. it was just deleted elsewhere).
(define (kruddboard-draw-asset-inspector id)
  (let ((info (krudd-asset-info id)))
    (if (not info)
	(set! kruddboard-assets-sel 0)
	(begin
	  (kruddboard-draw-asset-header info)
	  (kruddboard-draw-asset-body id info)))))

;;! ------------------------------------------------------------------
;;! Top level
;;! ------------------------------------------------------------------

;;! (kruddboard-draw-assets) is the whole Assets tab: either the inspector
;;! (once something is selected — it draws its own red collapsing headers,
;;! see below) or a single "Browser" section holding the New Asset form and
;;! the table/"(no assets)" placeholder — the Scheme composition that
;;! replaces the C draw_tab_assets. (krudd-assets) returning #f (no asset
;;! api) mirrors the old top-of-function null check.
;;!
;;! "Browser" has no C ancestor — draw_tab_assets never wrapped the table in
;;! a header. It exists purely so the tab reads as Scheme-driven the instant
;;! it opens, the same way World's top-level "Entities" header (which
;;! likewise nests its "+ Entity" creation control inside) does; every
;;! header here draws through imgui-collapsing-header, the same red-tinted
;;! primitive KRUDD and World's headers use (see sp_imgui_collapsing_header
;;! in kruddboard.cpp), so nothing extra was needed to make it match them.
(define (kruddboard-draw-assets)
  (let ((groups (krudd-assets)))
    (if (not groups)
	(imgui-text-disabled "(assets unavailable)")
	(if (not (= kruddboard-assets-sel 0))
	    (kruddboard-draw-asset-inspector kruddboard-assets-sel)
	    (when (imgui-collapsing-header "Browser")
	      (when (krudd-asset-mut?)
		(kruddboard-draw-new-asset-form)
		(imgui-separator))
	      (if (and (null? (car groups)) (null? (cadr groups)))
		  (imgui-text-disabled "(no assets)")
		  (kruddboard-draw-asset-table groups)))))))
