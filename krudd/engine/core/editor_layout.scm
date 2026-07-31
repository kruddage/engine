; SPDX-License-Identifier: GPL-2.0-or-later

;;! editor-layout — the editor's chrome as Scheme data (#722, part A of #706).
;;!
;;! (editor-layout) evaluates to one data tree describing the whole editor
;;! shell: its menu bar, toolbar, docks and status-bar fields. The page
;;! hard-codes no menu, dock, toolbar or status literal — a menu or dock added
;;! here reaches the chrome with no edit to any renderer. That is the whole
;;! point of #706, and the reason the spec lives in core/ rather than beside a
;;! shell: `script` is a library every module may link, so it may not reach into
;;! a shell for its own generated header.
;;!
;;! What changed since #722, and why this file is back after #953 retired it:
;;! the spec's one consumer used to be script_layout_json, which serialized the
;;! tree to JSON for a DOM builder in the web shell. That transport is gone and
;;! is not restored here. The tree is now read *in Scheme*, by the chrome
;;! renderers in ui/kruddgui, so the reader is a set of accessors below rather
;;! than a C serializer — no seam to cross, and the spec and its reader move
;;! together in one language. script_init evaluates this image into the shared
;;! interpreter, so (editor-layout) is bound wherever s7 is up.
;;!
;;! The tree is a list of tagged sections; the accessors below are the reader,
;;! so their shape and this doc move together:
;;!
;;!   (menus (menu LABEL ITEM ...) ...)
;;!       ITEM is one of
;;!         (action LABEL SHORTCUT ACTION-ID)  a menu entry. SHORTCUT is a
;;!             standard-key symbol (new open save save-as quit undo redo cut
;;!             copy paste) or `none`. ACTION-ID is an opaque string, and which
;;!             ids the host has wired is the host's business, not the spec's —
;;!             an id the host does not recognize falls through to its "coming
;;!             soon" hint, with the label derived from LABEL.
;;!         (separator)                        a menu divider.
;;!         (dock-toggles)                     expands to one show/hide toggle
;;!             per dock below, in declaration order — the View menu's body.
;;!             editor-layout-menu-items does that expansion, so a renderer
;;!             never sees the unexpanded form.
;;!   (toolbar ITEM ...)
;;!       ITEM is (item LABEL ACTION-ID) | (separator) | (spacer) |
;;!       (badge ID TEXT). The badge is a live label the host updates by ID (the
;;!       renderer status, the version). The spacer is an elastic gap: it eats
;;!       the leftover width, so everything declared after it sits against the
;;!       toolbar's trailing edge.
;;!   (docks (dock ID TITLE AREA PANEL BLURB EXTRA ...) ...)
;;!       ID is the dock's identity — a renderer's saved layout and View > Reset
;;!       Layout key off it. AREA is left/right/top/bottom. PANEL/BLURB are the
;;!       "coming soon" placeholder's heading and one-line description. EXTRA is
;;!       any of (tabbed-with ID) — tab this dock behind another — and (raise) —
;;!       show this dock on top of its tab group.
;;!   (statusbar (field ID TEXT) ...)
;;!       a permanent status-bar label; the host updates fps / resolution /
;;!       driver by ID each frame.

(define (editor-layout)
  '((menus
     (menu "&File"
           (action "&New Project"   new     "new")
           (action "&Open Project…" open    "open-project")
           (separator)
           (action "&Save"          save    "save")
           (action "Save &As…"      save-as "save-as")
           (separator)
           (action "&Quit"          quit    "quit"))
     (menu "&Edit"
           (action "&Undo"  undo  "undo")
           (action "&Redo"  redo  "redo")
           (separator)
           (action "Cu&t"   cut   "cut")
           (action "&Copy"  copy  "copy")
           (action "&Paste" paste "paste"))
     (menu "&View"
           (dock-toggles)
           (separator)
           (action "Reset &Layout" none "reset-layout"))
     (menu "&Help"
           (action "&About krudd" none "about")))
    (toolbar
     ;;! The live backend badge. The seed names no backend on purpose: which
     ;;! one the page ends up on — WebGPU or WebGL — is not known when the
     ;;! chrome is built. A seed that guessed would lie to the user for as long
     ;;! as the boot takes, which on the web's async device handshake is every
     ;;! frame until the adapter answers. The host overwrites it with the
     ;;! backend that actually went live.
     (badge "renderer" "renderer — booting…")
     (spacer)
     ;;! Right-aligned build identity. The host fills in the semver from
     ;;! ENGINE_VERSION_STRING, which only it knows — the spec seeds the name.
     (badge "version" "KRUDD Editor"))
    (docks
     (dock "dock.scene" "Scene" left
           "Scene Tree"
           "The entity hierarchy of the open project — pick a node to edit it in the Inspector.")
     (dock "dock.inspector" "Inspector" right
           "Inspector"
           "Components and properties of the selected entity, written back to the project files.")
     (dock "dock.assets" "Assets" bottom
           "Asset Browser"
           "Meshes, textures, sounds and scenes in the project, ready to drag into the scene."
           (raise))
     (dock "dock.console" "Console" bottom
           "Scheme REPL"
           "A live S7 Scheme console into the running engine image — evaluate against the game as it plays."
           (tabbed-with "dock.assets")))
    (statusbar
     (field "fps"        "fps —")
     (field "resolution" "—×—")
     (field "driver"     ""))))

;;! ==================================================================
;;! The reader
;;! ==================================================================
;;!
;;! Every accessor below takes the tree (or a node out of it) and returns a
;;! plain value — no drawing, no state, no host primitive. That is deliberate:
;;! this half is what the two chrome renderers share, so a renderer can only
;;! disagree with another about *how* a section looks, never about what the
;;! section says. It is also why the reader is testable with nothing but s7
;;! (core/editor_layout_test.c).
;;!
;;! A missing section reads as the empty list rather than an error. A spec that
;;! declares no toolbar is a chrome with no toolbar, which is a coherent thing
;;! to ask for; making it a trap would mean every renderer guarding every
;;! section before it walked it.

;;! (editor-layout-section layout tag) -> the body of the TAG section of
;;! LAYOUT, or () when the spec declares no such section.
(define (editor-layout-section layout tag)
  (let ((s (assq tag layout)))
    (if (pair? s) (cdr s) '())))

;;! The four sections, by name.
(define (editor-layout-menus layout)
  (editor-layout-section layout 'menus))
(define (editor-layout-toolbar layout)
  (editor-layout-section layout 'toolbar))
(define (editor-layout-docks layout)
  (editor-layout-section layout 'docks))
(define (editor-layout-statusbar layout)
  (editor-layout-section layout 'statusbar))

;;! (editor-layout-kind node) -> the node's tag symbol ('action 'separator
;;! 'spacer 'badge 'item 'dock 'field ...). The one dispatch every renderer
;;! makes as it walks a section's items.
(define (editor-layout-kind node)
  (if (pair? node) (car node) #f))

;;! (editor-layout-mnemonic label) -> LABEL with its `&` mnemonic markers
;;! removed: "&New Project" -> "New Project", "Cu&t" -> "Cut". The markers are
;;! how the spec says which letter a keyboard shortcut underlines, which is a
;;! DOM menu's convention; a renderer that draws its own glyphs has nowhere to
;;! put an underline and wants the plain string. "&&" is a literal ampersand.
(define (editor-layout-mnemonic label)
  (let ((n (string-length label)))
    (let loop ((i 0) (out ""))
      (cond ((>= i n) out)
            ((and (char=? (string-ref label i) #\&)
                  (< (+ i 1) n)
                  (char=? (string-ref label (+ i 1)) #\&))
             (loop (+ i 2) (string-append out "&")))
            ((char=? (string-ref label i) #\&)
             (loop (+ i 1) out))
            (else
             (loop (+ i 1)
                   (string-append out (substring label i (+ i 1)))))))))

;;! Menu accessors. A menu is (menu LABEL ITEM ...).
(define (editor-layout-menu-label menu)
  (cadr menu))

;;! (editor-layout-menu-items menu docks) -> MENU's items with any
;;! (dock-toggles) form replaced, in place, by one toggle action per dock in
;;! DOCKS — the View menu's body. The expansion lives here rather than in each
;;! renderer because it is the one place the menus section and the docks
;;! section have to agree, and two renderers that expanded it themselves would
;;! be two chances to disagree. A toggle's action id is "toggle:" + the dock's
;;! id, so a host wires them by pattern rather than by enumerating them.
(define (editor-layout-menu-items menu docks)
  (let loop ((items (cddr menu)) (out '()))
    (if (null? items)
        (reverse out)
        (let ((item (car items)))
          (if (eq? (editor-layout-kind item) 'dock-toggles)
              (loop (cdr items)
                    (append (reverse (map editor-layout-dock-toggle docks))
                            out))
              (loop (cdr items) (cons item out)))))))

;;! (editor-layout-dock-toggle dock) -> the (action ...) form that shows or
;;! hides DOCK. Its label is the dock's title, so the View menu reads as the
;;! dock list it is.
(define (editor-layout-dock-toggle dock)
  (list 'action (editor-layout-dock-title dock) 'none
        (string-append "toggle:" (editor-layout-dock-id dock))))

;;! Action accessors. An action is (action LABEL SHORTCUT ACTION-ID).
(define (editor-layout-action-label action)
  (cadr action))
(define (editor-layout-action-shortcut action)
  (caddr action))
(define (editor-layout-action-id action)
  (cadddr action))

;;! Badge accessors. A badge is (badge ID TEXT) — a toolbar label the host
;;! rewrites by id while the chrome is up.
(define (editor-layout-badge-id badge)
  (cadr badge))
(define (editor-layout-badge-text badge)
  (caddr badge))

;;! Toolbar item accessors. An item is (item LABEL ACTION-ID).
(define (editor-layout-item-label item)
  (cadr item))
(define (editor-layout-item-id item)
  (caddr item))

;;! Dock accessors. A dock is (dock ID TITLE AREA PANEL BLURB EXTRA ...).
(define (editor-layout-dock-id dock)
  (cadr dock))
(define (editor-layout-dock-title dock)
  (caddr dock))
(define (editor-layout-dock-area dock)
  (cadddr dock))
(define (editor-layout-dock-panel dock)
  (list-ref dock 4))
(define (editor-layout-dock-blurb dock)
  (list-ref dock 5))
(define (editor-layout-dock-extras dock)
  (list-tail dock 6))

;;! (editor-layout-dock-tabbed-with dock) -> the id of the dock this one tabs
;;! behind, or #f when it stands on its own.
(define (editor-layout-dock-tabbed-with dock)
  (let ((e (assq 'tabbed-with (editor-layout-dock-extras dock))))
    (if (pair? e) (cadr e) #f)))

;;! (editor-layout-dock-raise? dock) -> #t when the spec asks for this dock to
;;! show on top of its tab group.
(define (editor-layout-dock-raise? dock)
  (pair? (assq 'raise (editor-layout-dock-extras dock))))

;;! (editor-layout-docks-in layout area) -> the docks LAYOUT declares for AREA
;;! ('left 'right 'top 'bottom), in declaration order. The renderers place by
;;! area, so this is the walk both of them start from.
(define (editor-layout-docks-in layout area)
  (let loop ((docks (editor-layout-docks layout)) (out '()))
    (cond ((null? docks) (reverse out))
          ((eq? (editor-layout-dock-area (car docks)) area)
           (loop (cdr docks) (cons (car docks) out)))
          (else (loop (cdr docks) out)))))

;;! (editor-layout-dock-find layout id) -> the dock with ID, or #f.
(define (editor-layout-dock-find layout id)
  (let loop ((docks (editor-layout-docks layout)))
    (cond ((null? docks) #f)
          ((string=? (editor-layout-dock-id (car docks)) id) (car docks))
          (else (loop (cdr docks))))))

;;! Status-bar field accessors. A field is (field ID TEXT).
(define (editor-layout-field-id field)
  (cadr field))
(define (editor-layout-field-text field)
  (caddr field))
