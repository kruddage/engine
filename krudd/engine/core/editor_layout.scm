; SPDX-License-Identifier: GPL-2.0-or-later

;;! editor-layout — the native editor's chrome as Scheme data (#722, part A of
;;! #706).
;;!
;;! (editor-layout) evaluates to one data tree describing the whole editor
;;! shell: its menu bar, toolbar, docks and status-bar fields. The native Qt
;;! host (core/krudd_qt.cpp) walks this tree — through the s7 reader in
;;! core/editor_layout.c — and emits the equivalent Qt widgets, so there are no
;;! hard-coded menu/dock/toolbar literals left in the C++. This spec is the
;;! single source of the shell's structure; a later slice (#706 parts B/C) feeds
;;! the same tree to a Qt-free HTML/JS web editor.
;;!
;;! The tree is a list of tagged sections. editor_layout.c is the reader, so its
;;! shape and this doc move together:
;;!
;;!   (menus (menu LABEL ITEM ...) ...)
;;!       ITEM is one of
;;!         (action LABEL SHORTCUT ACTION-ID)  a menu entry. SHORTCUT is a
;;!             standard-key symbol (new open save save-as quit undo redo cut
;;!             copy paste) or `none`. ACTION-ID is an opaque string the host
;;!             maps to behavior — open-project / quit / reset-layout / about are
;;!             wired; every other id falls through to the "coming soon" status
;;!             hint (its label is derived from LABEL).
;;!         (separator)                        a menu divider.
;;!         (dock-toggles)                     expands to one show/hide toggle
;;!             per dock below, in declaration order — the View menu's body.
;;!   (toolbar ITEM ...)
;;!       ITEM is (item LABEL ACTION-ID) | (separator) | (badge ID TEXT). The
;;!       badge is a live label the host updates by ID (the renderer status).
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
     (item "▶ Play" "play")
     (item "■ Stop" "stop")
     (separator)
     (badge "renderer" "Vulkan — booting…"))
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
