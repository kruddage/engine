; SPDX-License-Identifier: GPL-2.0-or-later

;;! editor-layout — the native editor's chrome as Scheme data (#722, part A of
;;! #706).
;;!
;;! (editor-layout) evaluates to one data tree describing the whole editor
;;! shell: its menu bar, toolbar, docks and status-bar fields. Two hosts read
;;! it, and neither hard-codes a menu, dock, toolbar or status literal:
;;!   - the native Qt editor, through the s7 reader in shell/qt/editor_layout.c,
;;!     which walks the tree into a C struct shell/qt/krudd_qt.cpp emits Qt
;;!     widgets from;
;;!   - the Qt-free browser editor, through core/script.c's script_layout_json,
;;!     which serializes the same tree to JSON for shell/web/shell.html.in's
;;!     kruddBuildEditor to build DOM from.
;;! This spec is the single source of the shell's structure: a menu or dock
;;! added here reaches both hosts with no host-specific edit. That is the whole
;;! point of #706, and the reason the spec lives in core/ rather than beside
;;! either shell — `script` is a library every module may link, so it may not
;;! reach into a shell for its own generated header.
;;!
;;! The tree is a list of tagged sections. editor_layout.c is the reader, so its
;;! shape and this doc move together:
;;!
;;!   (menus (menu LABEL ITEM ...) ...)
;;!       ITEM is one of
;;!         (action LABEL SHORTCUT ACTION-ID)  a menu entry. SHORTCUT is a
;;!             standard-key symbol (new open save save-as quit undo redo cut
;;!             copy paste) or `none`. ACTION-ID is an opaque string, and which
;;!             ids a host has wired is that host's business, not the spec's —
;;!             an id a host does not recognize falls through to its "coming
;;!             soon" hint, with the label derived from LABEL. The two hosts do
;;!             not agree today: the Qt shell wires open-project, quit, about
;;!             and reset-layout, the web shell only reset-layout and the dock
;;!             toggles. Converging them is #802, not a change to this spec.
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
;;!       "coming soon" placeholder's heading and one-line description, shown
;;!       until the host renders the dock's body. EXTRA is any of
;;!       (tabbed-with ID) — tab this dock behind another — (raise) — show this
;;!       dock on top of its tab group — and (body NODE), below.
;;!   (statusbar (field ID TEXT) ...)
;;!       a permanent status-bar label; the host updates fps / resolution /
;;!       driver by ID each frame.
;;!
;;! PANEL BODIES (#795, the foundation for #798-#801)
;;!
;;! (body NODE) says what a dock's panel *is*, so the panels are data here like
;;! the menus and docks already are, rather than being hand-built twice in two
;;! hosts. Every node has the same shape:
;;!
;;!   (KIND SOURCE LABEL CHILD ...)
;;!
;;! where the two strings are optional and positional, and any nested forms are
;;! children. That one shape is what keeps the reader small and makes a new
;;! widget kind cost an enum value rather than a new struct.
;;!
;;! SOURCE names *what data feeds the node*; it never carries data. This spec is
;;! evaluated once at boot, so live contents cannot travel through it — the host
;;! resolves the name against the engine apis at render time. LABEL is a human
;;! label where the kind has one.
;;!
;;!   (tree SOURCE)           a hierarchy. SOURCE is the tree to walk.
;;!   (list SOURCE)           a flat collection.
;;!   (property-grid SOURCE)  label/value rows for whatever SOURCE selects,
;;!                           with (field ...) children naming the rows.
;;!   (field NAME LABEL)      one row of a property-grid: the property NAME the
;;!                           host reads/writes, and how to label it.
;;!   (console SOURCE)        scrollback plus an input line bound to SOURCE.
;;!   (label TEXT)            static text — an empty state, a hint.
;;!   (stack NODE ...)        several nodes in one panel, in order. Not a layout
;;!                           engine: it says "these, in this order", and the
;;!                           host decides every pixel.
;;!
;;! A host renders what it recognizes and skips what it does not, so a kind can
;;! land here before both hosts can draw it — which is exactly how this slice
;;! ships. A dock with no (body ...), or whose body a host cannot render, keeps
;;! showing the PANEL/BLURB placeholder.

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
     (item "▶ Play" "play")
     (item "■ Stop" "stop")
     (separator)
     ;;! The live backend badge. The seed names no backend on purpose: this
     ;;! spec is shared by both hosts, and each one runs a different GPU path —
     ;;! Vulkan under Qt, WebGPU or WebGL in the browser. A host-specific seed
     ;;! here reaches the other host verbatim and lies to the user for as long
     ;;! as the boot takes, which on the web's async device handshake is every
     ;;! frame until the adapter answers. Each host overwrites it with the
     ;;! backend that actually went live (krudd_qt.cpp; kruddSetRenderer on the
     ;;! web, seeded with the chosen path by buildToolbar before that lands).
     (badge "renderer" "renderer — booting…")
     (spacer)
     ;;! Right-aligned build identity. The host fills in the semver from
     ;;! ENGINE_VERSION_STRING, which only it knows — the spec seeds the name.
     (badge "version" "KRUDD Editor"))
    (docks
     ;;! The four panel bodies. Declared before either host can render them
     ;;! (#795): both readers carry them, a host skips what it cannot draw, so
     ;;! these docks still show their PANEL/BLURB placeholder until #798-#801
     ;;! teach the hosts each kind. Naming them here first is what lets those
     ;;! four land as one new case each rather than as a spec change plus two
     ;;! renderer changes apiece.
     ;;!
     ;;! Every SOURCE below is the name of an engine api the host resolves at
     ;;! render time, never data: "scene" is the entity api, "asset" the asset
     ;;! registry, "script" the shared s7 image.
     (dock "dock.scene" "Scene" left
           "Scene Tree"
           "The entity hierarchy of the open project — pick a node to edit it in the Inspector."
           (body (tree "scene")))
     (dock "dock.inspector" "Inspector" right
           "Inspector"
           "Components and properties of the selected entity, written back to the project files."
           ;;! The rows are named here rather than discovered, so both hosts show
           ;;! the same Inspector for the same entity. The component bindings a
           ;;! host reads through asset describe() are #800's business; these are
           ;;! the properties every entity has.
           (body (stack
                  (property-grid "scene.selection"
                                 (field "name"     "Name")
                                 (field "position" "Position")
                                 (field "rotation" "Rotation")
                                 (field "scale"    "Scale"))
                  (label "Select an entity in the Scene tree to edit it."))))
     (dock "dock.assets" "Assets" bottom
           "Asset Browser"
           "Meshes, textures, sounds and scenes in the project, ready to drag into the scene."
           (raise)
           (body (list "asset")))
     (dock "dock.console" "Console" bottom
           "Scheme REPL"
           "A live S7 Scheme console into the running engine image — evaluate against the game as it plays."
           (tabbed-with "dock.assets")
           (body (console "script"))))
    (statusbar
     (field "fps"        "fps —")
     (field "resolution" "—×—")
     (field "driver"     ""))))
