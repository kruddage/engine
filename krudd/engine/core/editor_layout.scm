; SPDX-License-Identifier: GPL-2.0-or-later

;;! editor-layout — the editor's chrome as Scheme data (#722, part A of #706).
;;!
;;! (editor-layout) evaluates to one data tree describing the whole editor
;;! shell: its menu bar, toolbar, docks and status-bar fields. The browser
;;! editor reads it through core/script.c's script_layout_json, which serializes
;;! the tree to JSON for shell/web/shell.html.in's kruddBuildEditor to build DOM
;;! from — so the page hard-codes no menu, dock, toolbar or status literal.
;;! This spec is the single source of the shell's structure: a menu or dock
;;! added here reaches the chrome with no edit to the page. That is the whole
;;! point of #706, and the reason the spec lives in core/ rather than beside the
;;! shell — `script` is a library every module may link, so it may not reach
;;! into a shell for its own generated header.
;;!
;;! The tree is a list of tagged sections; script_layout_json is the reader, so
;;! its shape and this doc move together:
;;!
;;!   (menus (menu LABEL ITEM ...) ...)
;;!       ITEM is one of
;;!         (action LABEL SHORTCUT ACTION-ID)  a menu entry. SHORTCUT is a
;;!             standard-key symbol (new open save save-as quit undo redo cut
;;!             copy paste) or `none`. ACTION-ID is an opaque string, and which
;;!             ids the host has wired is the host's business, not the spec's —
;;!             an id the host does not recognize falls through to its "coming
;;!             soon" hint, with the label derived from LABEL. Most ids are
;;!             still unwired: the web shell handles reset-layout and the dock
;;!             toggles, and the rest hint. Wiring them is #802, not a change to
;;!             this spec.
;;!         (separator)                        a menu divider.
;;!         (dock-toggles)                     expands to one show/hide toggle
;;!             per dock below, in declaration order — the View menu's body.
;;!   (toolbar ITEM ...)
;;!       ITEM is (item LABEL ACTION-ID) | (separator) | (spacer) |
;;!       (badge ID TEXT). The badge is a live label the host updates by ID (the
;;!       renderer status, the version). The spacer is an elastic gap: it eats
;;!       the leftover width, so everything declared after it sits against the
;;!       toolbar's trailing edge.
;;!   (docks (dock ID TITLE AREA PANEL BLURB EXTRA ...) ...)
;;!       ID is the dock objectName — saveState/restoreState and View > Reset
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
     ;;! backend that actually went live (kruddSetRenderer, seeded with the
     ;;! chosen path by buildToolbar before that lands).
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
