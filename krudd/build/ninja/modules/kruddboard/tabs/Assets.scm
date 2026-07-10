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

;;! Last script Save result, the same three states as shader-ok: 'untried,
;;! #t (saved) or #f (rejected as not a well-formed (script ...) form).
(define kruddboard-assets-script-ok 'untried)

;;! Material color editor buffer and the id whose bytes it holds.
(define kruddboard-assets-color-id 0)
(define kruddboard-assets-color (list 1.0 1.0 1.0 1.0))

;;! New Asset form state: visible?, the name field, and the type combo index
;;! (0 Text, 1 Shader, 2 Material, 3 Script).
(define kruddboard-assets-naming #f)
(define kruddboard-assets-new-name "")
(define kruddboard-assets-new-type 0)

;;! Built-in shader "Clone" form state: which built-in it's seeded from, the
;;! proposed name, and whether the last attempt hit a duplicate path.
(define kruddboard-assets-clone-src 0)
(define kruddboard-assets-clone-name "")
(define kruddboard-assets-clone-conflict #f)

;;! Whether the browser table's BUILT-IN group is shown. Off by default —
;;! most sessions only care about project assets, and the read-only builtin://
;;! rows just add noise to the list.
(define kruddboard-assets-show-builtin #f)

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
    (set! kruddboard-assets-shader-ok 'untried)
    (set! kruddboard-assets-script-ok 'untried)))

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
	((= type 3) (krudd-asset-create-script name))
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
	  (let* ((new-type (imgui-combo "type"
					(list "Text" "Shader" "Material" "Script")
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
;;! Asset browser table — path tree helpers
;;! ------------------------------------------------------------------
;;!
;;! The browser groups rows into a tree by splitting each asset's path on
;;! "/", the same de facto virtual-filesystem convention builtin:// paths
;;! already use ("builtin://shader/scene" -> folder "shader", leaf "scene")
;;! and that authored names are free to opt into by including a "/". Rows
;;! that don't use it just come out as a single top-level leaf, so nothing
;;! about existing flat-named assets changes.
;;!
;;! This build's s7 doesn't carry a core `filter`, so the handful of list
;;! operations the tree builder needs are spelled out by hand below rather
;;! than assumed to exist.

;;! (kruddboard-string-split str ch) splits STR at each occurrence of the
;;! character CH, dropping empty pieces — so a leading "/" or a doubled "//"
;;! doesn't produce a blank path segment.
(define (kruddboard-string-split str ch)
  (let loop ((start 0) (acc '()))
    (let ((pos (char-position ch str start)))
      (if pos
	  (loop (+ pos 1)
		(let ((seg (substring str start pos)))
		  (if (string=? seg "") acc (cons seg acc))))
	  (let ((seg (substring str start (string-length str))))
	    (reverse (if (string=? seg "") acc (cons seg acc))))))))

;;! (kruddboard-assets-path-segments path) splits an asset path into the
;;! folder segments the tree groups by: a builtin:// scheme prefix is
;;! dropped first (kruddboard-strip-builtin-prefix) so "shader"/"material"
;;! read as folders instead of URI noise, then the rest splits on "/". A
;;! path with no "/" (or that strips down to nothing) comes back as its own
;;! single-element list, i.e. a top-level leaf.
(define (kruddboard-assets-path-segments path)
  (let ((segs (kruddboard-string-split
	       (kruddboard-strip-builtin-prefix path) #\/)))
    (if (null? segs) (list path) segs)))

;;! (kruddboard-assets-rows->entries rows) turns a flat ROWS list (one group
;;! as krudd-assets returns it) into (segments row) entries, the shape
;;! kruddboard-draw-asset-tree groups and recurses on.
(define (kruddboard-assets-rows->entries rows)
  (map (lambda (row)
	 (list (kruddboard-assets-path-segments (list-ref row 1)) row))
       rows))

;;! (kruddboard-assets-entries-at-depth entries want-folders?) splits
;;! ENTRIES by whether their remaining segment list still has more than one
;;! element (nested under a folder) or has bottomed out at exactly one (a
;;! leaf asset); WANT-FOLDERS? selects which half comes back. Written as
;;! explicit recursion since this s7 has no core `filter`.
(define (kruddboard-assets-entries-at-depth entries want-folders?)
  (cond ((null? entries) '())
	(else
	 (let* ((e (car entries))
		(segs (list-ref e 0))
		(is-folder (pair? (cdr segs)))
		(rest (kruddboard-assets-entries-at-depth
		       (cdr entries) want-folders?)))
	   (if (eq? is-folder want-folders?) (cons e rest) rest)))))

;;! (kruddboard-assets-entries-with-head entries head) is the ENTRIES whose
;;! next path segment is HEAD, the by-hand `filter` kruddboard-assets-
;;! group-by-head needs.
(define (kruddboard-assets-entries-with-head entries head)
  (cond ((null? entries) '())
	(else
	 (let* ((e (car entries))
		(segs (list-ref e 0))
		(rest (kruddboard-assets-entries-with-head (cdr entries) head)))
	   (if (string=? (car segs) head) (cons e rest) rest)))))

;;! (kruddboard-uniq lst) is LST's elements in first-appearance order with
;;! later duplicates dropped (compared with equal?, via member).
(define (kruddboard-uniq lst)
  (let loop ((lst lst) (seen '()))
    (cond ((null? lst) (reverse seen))
	  ((member (car lst) seen) (loop (cdr lst) seen))
	  (else (loop (cdr lst) (cons (car lst) seen))))))

;;! (kruddboard-assets-group-by-head entries) partitions ENTRIES — all of
;;! whose segment lists have length > 1 — into one bucket per distinct first
;;! segment, in first-appearance order. Each bucket is (head . child-
;;! entries), child-entries being the same (segments row) shape with the
;;! head segment stripped off, ready to recurse one level deeper.
(define (kruddboard-assets-group-by-head entries)
  (map (lambda (head)
	 (cons head
	       (map (lambda (e) (list (cdr (list-ref e 0)) (list-ref e 1)))
		    (kruddboard-assets-entries-with-head entries head))))
       (kruddboard-uniq (map (lambda (e) (car (list-ref e 0))) entries))))

;;! ------------------------------------------------------------------
;;! Asset browser table
;;! ------------------------------------------------------------------

;;! (kruddboard-draw-asset-row row name ro?) draws one leaf row: a tree-node
;;! bullet (opens the inspector on click, like the old span-all selectable
;;! did) showing NAME — the asset's path with every ancestor folder segment
;;! already peeled off, since those are drawn as the tree's parent nodes — a
;;! drag source for mesh rows (drag-to-spawn, #176), and the Type/Kind/
;;! State/Size/Flags cells. row is an (id path type kind state size refs)
;;! list; ro? is #t for the built-in group (draws "RO"), #f for the project
;;! group (draws "-"). selected? is always #f: this table only ever draws
;;! while kruddboard-assets-sel is 0 (the inspector takes over the instant
;;! it isn't), so there is never a live selection to highlight here.
(define (kruddboard-draw-asset-row row name ro?)
  (let ((id   (list-ref row 0))
	(path (list-ref row 1))
	(type (list-ref row 2))
	(kind (list-ref row 3))
	(state (list-ref row 4))
	(size (list-ref row 5)))
    (imgui-table-next-row)
    (imgui-table-next-column)
    (let ((r (imgui-tree-node path name #t #f)))
      (when (cdr r)
	(set! kruddboard-assets-sel id)))
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

;;! (kruddboard-draw-asset-tree entries prefix ro?) draws one tree level:
;;! a folder tree-node per distinct first path segment among ENTRIES,
;;! recursing into it when expanded, then a leaf row (kruddboard-draw-
;;! asset-row) for every entry that has already bottomed out to a single
;;! segment. Folders draw before leaves, VS-Code-Explorer-style. PREFIX is
;;! the accumulated id of already-drawn ancestor folders — folder names
;;! alone can repeat under different parents, so it's threaded through to
;;! keep every imgui-tree-node id unique; leaf rows use their own asset path
;;! as the id instead, which the catalog already guarantees is unique. RO?
;;! is threaded straight through to kruddboard-draw-asset-row.
(define (kruddboard-draw-asset-tree entries prefix ro?)
  (let ((folders (kruddboard-assets-group-by-head
		  (kruddboard-assets-entries-at-depth entries #t)))
	(leaves (kruddboard-assets-entries-at-depth entries #f)))
    (for-each
     (lambda (bucket)
       (let* ((name (car bucket))
	      (children (cdr bucket))
	      (id (string-append prefix "/" name)))
	 (imgui-table-next-row)
	 (imgui-table-next-column)
	 (let ((r (imgui-tree-node id name #f #f)))
	   (when (car r)
	     (kruddboard-draw-asset-tree children id ro?)
	     (imgui-tree-pop)))))
     folders)
    (for-each
     (lambda (e)
       (kruddboard-draw-asset-row (list-ref e 1) (car (list-ref e 0)) ro?))
     leaves)))

;;! (kruddboard-draw-asset-table groups) is the six-column Path / Type /
;;! Kind / State / Size / Flags tree table, in two labeled groups — the old
;;! draw_tab_assets table body, with the flat Path column turned into an
;;! expandable folder tree. groups is (builtin-rows project-rows), as
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
	(kruddboard-draw-asset-tree
	 (kruddboard-assets-rows->entries builtin) "builtin" #t))
      (unless (null? project)
	(imgui-table-next-row)
	(imgui-table-next-column)
	(imgui-text-disabled "-- PROJECT --")
	(kruddboard-draw-asset-tree
	 (kruddboard-assets-rows->entries project) "project" #f))
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

;;! Strip a leading "builtin://" scheme from PATH, if present — the Clone
;;! form seeds its default name from the source asset's path, and a bare
;;! "builtin://" prefix reads as noise on what's meant to become a normal
;;! project-local asset name.
(define (kruddboard-strip-builtin-prefix path)
  (let ((prefix "builtin://"))
    (if (and (>= (string-length path) (string-length prefix))
	     (string=? (substring path 0 (string-length prefix)) prefix))
	(substring path (string-length prefix) (string-length path))
	path)))

;;! The name field + Clone button for a read-only (built-in) shader — the old
;;! draw_asset_inspector built-in-shader branch. path seeds the default
;;! "<path>_copy" name the first time this built-in is viewed.
(define (kruddboard-draw-asset-shader-clone id path)
  (unless (= kruddboard-assets-clone-src id)
    (set! kruddboard-assets-clone-src id)
    (set! kruddboard-assets-clone-name
	  (string-append (kruddboard-strip-builtin-prefix path) "_copy"))
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

;;! The Save button + save-result text for an editable script — the script
;;! analogue of kruddboard-draw-asset-shader-save. A rejected save (not a
;;! well-formed (script ...) form) leaves the last-committed source live.
(define (kruddboard-draw-asset-script-save id)
  (when (imgui-button "Save")
    (set! kruddboard-assets-script-ok
	  (krudd-asset-save-script id kruddboard-assets-edit-text)))
  (imgui-same-line)
  (cond ((eq? kruddboard-assets-script-ok #t)
	 (imgui-text-colored 0.3 0.9 0.3 1.0 "Saved"))
	((eq? kruddboard-assets-script-ok #f)
	 (imgui-text-colored 1.0 0.3 0.3 1.0 "Not a valid script"))))

;;! The name field + Clone button for a read-only (built-in) script — the
;;! script analogue of kruddboard-draw-asset-shader-clone. It shares the same
;;! clone-src/name/conflict state (only one inspector is open at a time) and
;;! commits through krudd-asset-clone-script.
(define (kruddboard-draw-asset-script-clone id path)
  (unless (= kruddboard-assets-clone-src id)
    (set! kruddboard-assets-clone-src id)
    (set! kruddboard-assets-clone-name
	  (string-append (kruddboard-strip-builtin-prefix path) "_copy"))
    (set! kruddboard-assets-clone-conflict #f))
  (imgui-set-next-item-width 240.0)
  (let ((r (imgui-input-text-enter "##clonename" kruddboard-assets-clone-name)))
    (set! kruddboard-assets-clone-name (car r))
    (imgui-same-line)
    (let* ((clone-clicked (imgui-button "Clone"))
	   (confirm (or (cdr r) clone-clicked)))
      (when (and confirm (not (string=? kruddboard-assets-clone-name "")))
	(let ((nid (krudd-asset-clone-script kruddboard-assets-clone-name
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

;;! The name field + Clone button for a read-only (built-in) material — the
;;! old draw_asset_inspector built-in-material branch (new; mirrors the
;;! shader Clone flow above instead of just disabling Save). path seeds the
;;! default "<path>_copy" name the first time this built-in is viewed.
(define (kruddboard-draw-asset-material-clone id path)
  (unless (= kruddboard-assets-clone-src id)
    (set! kruddboard-assets-clone-src id)
    (set! kruddboard-assets-clone-name
	  (string-append (kruddboard-strip-builtin-prefix path) "_copy"))
    (set! kruddboard-assets-clone-conflict #f))
  (imgui-set-next-item-width 240.0)
  (let ((r (imgui-input-text-enter "##clonename" kruddboard-assets-clone-name)))
    (set! kruddboard-assets-clone-name (car r))
    (imgui-same-line)
    (let* ((clone-clicked (imgui-button "Clone"))
	   (confirm (or (cdr r) clone-clicked)))
      (when (and confirm (not (string=? kruddboard-assets-clone-name "")))
	(let ((nid (apply krudd-asset-clone-material
			  kruddboard-assets-clone-name
			  kruddboard-assets-color)))
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

;;! Script inspector: derived Declaration (format + live hook list), Source box
;;! (editable or not), then either the Save/Delete row or the built-in Clone
;;! row — the (script ...) counterpart of kruddboard-draw-asset-shader-editor,
;;! so authoring an entity script feels exactly like authoring a shader.
(define (kruddboard-draw-asset-script-editor id path editable)
  (kruddboard-assets-maybe-reload-edit id)
  (imgui-separator)
  (when (imgui-collapsing-header "Declaration")
    (let ((hooks (krudd-script-hooks kruddboard-assets-edit-text)))
      (imgui-text "format: krudd-script")
      (imgui-text (format #f "hooks: ~A"
			  (if (string=? hooks "") "(none)" hooks)))))
  (imgui-separator)
  (when (imgui-collapsing-header "Source")
    (let ((r (imgui-input-text-multiline "##script" kruddboard-assets-edit-text
					 260.0 (not editable))))
      (set! kruddboard-assets-edit-text (car r))))
  (imgui-separator)
  (if editable
      (begin
	(kruddboard-draw-asset-script-save id)
	(imgui-same-line)
	(when (imgui-button "Delete")
	  (kruddboard-assets-do-delete id)))
      (kruddboard-draw-asset-script-clone id path)))

;;! Material inspector: a color picker (disabled when read-only) plus Save/
;;! Delete, or the built-in Clone row — the old draw_asset_inspector material
;;! branch, extended to clone built-ins like the shader editor above instead
;;! of just disabling Save.
(define (kruddboard-draw-asset-material-editor id path editable)
  (kruddboard-assets-maybe-reload-color id)
  (imgui-separator)
  (when (imgui-collapsing-header "Color")
    (imgui-begin-disabled (not editable))
    (let ((r (imgui-color-edit4 "##basecolor" kruddboard-assets-color)))
      (set! kruddboard-assets-color (car r)))
    (imgui-end-disabled))
  (imgui-separator)
  (if editable
      (begin
	(when (imgui-button "Save")
	  (apply krudd-asset-save-material id kruddboard-assets-color))
	(imgui-same-line)
	(when (imgui-button "Delete")
	  (kruddboard-assets-do-delete id)))
      (kruddboard-draw-asset-material-clone id path)))

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
;;! MATERIAL=3/SCRIPT=8, mirroring asset_api.h, the same convention the label
;;! helpers above use.
(define (kruddboard-draw-asset-body id info)
  (let ((path (list-ref info 0))
	(type (list-ref info 1))
	(read-only (list-ref info 6))
	(origin (list-ref info 7)))
    (cond
     ((and (= origin 1) (= type 7)) (kruddboard-draw-asset-text-editor id))
     ((= type 4) (kruddboard-draw-asset-shader-editor id path (not read-only)))
     ((= type 3) (kruddboard-draw-asset-material-editor id path (not read-only)))
     ((= type 8) (kruddboard-draw-asset-script-editor id path (not read-only)))
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
	      (set! kruddboard-assets-show-builtin
		    (imgui-checkbox "Show built-in assets"
				    kruddboard-assets-show-builtin))
	      (let ((visible (list (if kruddboard-assets-show-builtin
					(car groups)
					'())
				    (cadr groups))))
		(if (and (null? (car visible)) (null? (cadr visible)))
		    (imgui-text-disabled "(no assets)")
		    (kruddboard-draw-asset-table visible))))))))
