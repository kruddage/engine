/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef EDITOR_LAYOUT_H
#define EDITOR_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * editor_layout — the native editor's chrome, read from the Scheme layout spec
 * (core/editor_layout.scm, embedded as LAYOUT_SCM) into a plain C tree.
 *
 * editor_layout_load() evaluates the spec through the shared s7 image and walks
 * the (editor-layout) data tree into `struct editor_layout`, reusing the
 * query_params() s7-tree-walk pattern in script.c. The Qt host (krudd_qt.cpp)
 * emits Qt widgets from the result; editor_layout_test.c asserts on the same
 * result GPU- and Qt-free in CI. Keeping the walk here — not in the C++ — is
 * what lets the shell and the test share one reader of one spec (#722).
 *
 * Every array is a fixed, generous bound. The spec is authored in-tree, so a
 * spec that outgrows a bound is a build-time edit here, never runtime input;
 * the reader silently stops at the bound rather than overrun.
 */

enum {
	EDITOR_MAX_MENUS      = 8,
	EDITOR_MAX_MENU_ITEMS = 16,
	EDITOR_MAX_TOOLS      = 16,
	EDITOR_MAX_DOCKS      = 16,
	EDITOR_MAX_STATUS     = 8,
	/* Body nodes are pooled across every dock, not capped per dock: one
	 * shared budget spends where the panels actually need it (the Inspector's
	 * field rows) instead of reserving the deepest panel's worth sixteen
	 * times over. */
	EDITOR_MAX_BODY_NODES = 64,
	/* Nesting cap, in the spirit of script.c's JSON_MAX_DEPTH: the authored
	 * bodies nest two or three deep, so this only stops a pathological or
	 * self-referential form from recursing without end. */
	EDITOR_BODY_MAX_DEPTH = 8
};

/*
 * A panel body node — what a dock's panel *is*, from the spec's (body ...)
 * form. See core/editor_layout.scm for the authored vocabulary.
 *
 * WHY A FLAT ARENA. The rest of this struct is fixed-shape arrays, on the
 * deliberate rule at the top of this file: the spec is a build input, so a spec
 * that outgrows a bound is an edit here, never runtime input. Panel bodies are
 * the first part of the spec that nests to an arbitrary depth, which no fixed
 * array of structs expresses without either capping depth or growing a struct
 * per kind. So the nodes live in ONE pool on `struct editor_layout` and refer to
 * each other by index: `struct editor_dock` holds the index of its root, and
 * each node holds its first child and next sibling. That keeps the bound single
 * and countable, keeps the reader one loop, and makes a new widget kind cost an
 * enum value and a string — not a struct, a bound and a parse function.
 *
 * Indices are int32_t and -1 means "none", so a walk terminates on a value the
 * zeroed struct cannot accidentally produce (index 0 is a real node).
 */
enum editor_body_kind {
	EDITOR_BODY_NONE = 0,      /* unset / unknown kind, skipped by hosts   */
	EDITOR_BODY_TREE,          /* a hierarchy from `source`                */
	EDITOR_BODY_LIST,          /* a flat collection from `source`          */
	EDITOR_BODY_PROPERTY_GRID, /* label/value rows for what `source` selects */
	EDITOR_BODY_FIELD,         /* one row of a property grid               */
	EDITOR_BODY_CONSOLE,       /* scrollback + input bound to `source`     */
	EDITOR_BODY_LABEL,         /* static text, carried in `source`         */
	EDITOR_BODY_STACK          /* its children, in order                   */
};

struct editor_body_node {
	enum editor_body_kind kind;
	/*
	 * The node's first string argument. For most kinds this names the data
	 * source the host resolves against the engine apis ("scene", "asset",
	 * "script") — never data, since the spec is evaluated once at boot. For
	 * EDITOR_BODY_LABEL it is the literal text; for EDITOR_BODY_FIELD it is
	 * the property name the host reads and writes.
	 */
	char                  source[64];
	/* The human label, where the kind carries one (a field's row label). */
	char                  label[64];
	int32_t               parent;       /* -1 for a dock's root node      */
	int32_t               first_child;  /* -1 when the node has no children */
	int32_t               next_sibling; /* -1 at the end of a sibling run  */
};

/*
 * A menu entry's accelerator, as a platform-independent standard key the host
 * maps to its QKeySequence::StandardKey. EDITOR_KEY_NONE means "no shortcut".
 */
enum editor_shortcut {
	EDITOR_KEY_NONE = 0,
	EDITOR_KEY_NEW,
	EDITOR_KEY_OPEN,
	EDITOR_KEY_SAVE,
	EDITOR_KEY_SAVE_AS,
	EDITOR_KEY_QUIT,
	EDITOR_KEY_UNDO,
	EDITOR_KEY_REDO,
	EDITOR_KEY_CUT,
	EDITOR_KEY_COPY,
	EDITOR_KEY_PASTE
};

enum editor_menu_item_kind {
	EDITOR_ITEM_ACTION = 0,   /* label + shortcut + action id           */
	EDITOR_ITEM_SEPARATOR,    /* a divider                              */
	EDITOR_ITEM_DOCK_TOGGLES  /* expands to one show/hide toggle / dock */
};

struct editor_menu_item {
	enum editor_menu_item_kind kind;
	char                       label[64];
	enum editor_shortcut       shortcut;
	char                       action[48];  /* opaque action id */
};

struct editor_menu {
	char                    label[48];
	struct editor_menu_item items[EDITOR_MAX_MENU_ITEMS];
	uint32_t                item_count;
};

enum editor_tool_kind {
	EDITOR_TOOL_ITEM = 0,   /* a clickable action (label + action id)     */
	EDITOR_TOOL_SEPARATOR,  /* a divider                                  */
	EDITOR_TOOL_BADGE,      /* a live label (id + initial text)           */
	EDITOR_TOOL_SPACER      /* elastic gap — pushes the rest to the end   */
};

struct editor_tool {
	enum editor_tool_kind kind;
	char                  label[48]; /* item text, or badge initial text  */
	char                  id[48];    /* action id (item) / badge id (badge) */
};

enum editor_dock_area {
	EDITOR_AREA_LEFT = 0,
	EDITOR_AREA_RIGHT,
	EDITOR_AREA_TOP,
	EDITOR_AREA_BOTTOM
};

struct editor_dock {
	char                  id[48];          /* objectName for saveState   */
	char                  title[48];
	enum editor_dock_area area;
	char                  panel[48];       /* placeholder heading        */
	char                  blurb[256];      /* placeholder body           */
	char                  tabbed_with[48]; /* "" or another dock id      */
	int                   raise;           /* show on top of its tab group */
	/*
	 * Index into editor_layout.bodies of this dock's root body node, or -1
	 * when the dock declares no (body ...). A host that finds -1 — or a root
	 * whose kind it cannot render — shows the panel/blurb placeholder, which
	 * is what every dock does until #798-#801 land.
	 */
	int32_t               body;
};

struct editor_status_field {
	char id[32];
	char text[48];
};

struct editor_layout {
	struct editor_menu         menus[EDITOR_MAX_MENUS];
	uint32_t                   menu_count;
	struct editor_tool         tools[EDITOR_MAX_TOOLS];
	uint32_t                   tool_count;
	struct editor_dock         docks[EDITOR_MAX_DOCKS];
	uint32_t                   dock_count;
	struct editor_status_field status[EDITOR_MAX_STATUS];
	uint32_t                   status_count;
	/* The panel-body pool every dock's `body` indexes into. Shared, so the
	 * bound is one number to reason about however the panels are shaped. */
	struct editor_body_node    bodies[EDITOR_MAX_BODY_NODES];
	uint32_t                   body_count;
};

/*
 * Evaluate the embedded layout spec and walk (editor-layout) into *OUT (zeroed
 * first). The s7 interpreter is started on demand, so no prior script_init() is
 * required. Returns 0 on success, or -1 if the interpreter could not start, the
 * spec did not define editor-layout, or the tree is not a list. Unknown
 * sections and item kinds are skipped rather than failed, so a forward-
 * compatible spec addition never breaks an older reader.
 */
int editor_layout_load(struct editor_layout *out);

#ifdef __cplusplus
}
#endif

#endif /* EDITOR_LAYOUT_H */
