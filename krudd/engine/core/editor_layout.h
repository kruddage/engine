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
	EDITOR_MAX_STATUS     = 8
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
	EDITOR_TOOL_BADGE       /* a live label (id + initial text)           */
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
