/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * editor_layout_test — the spec -> layout walk, GPU- and Qt-free.
 *
 * The Qt shell (krudd_qt.cpp) turns `struct editor_layout` into a live menu
 * bar, toolbar, docks and status bar; opening that window needs a display and a
 * device a CI runner has no business assuming (which is why krudd_qt carries no
 * (test ...) edge). This test closes the loop the window can't close in CI: it
 * evaluates the same embedded spec (LAYOUT_SCM) through the same reader
 * (editor_layout_load) and asserts the C tree the shell would emit from — the
 * menus, their shortcuts and action ids, the docks and their areas / tab
 * grouping, the toolbar and the status fields. So a spec typo or a broken walk
 * fails here, in the spirit of editor_boot_test.c, long before it reaches a
 * window.
 */
#include "editor_layout.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The first menu whose label matches, or NULL. */
static const struct editor_menu *menu_by_label(const struct editor_layout *L,
					       const char *label)
{
	uint32_t i;

	for (i = 0; i < L->menu_count; i++)
		if (!strcmp(L->menus[i].label, label))
			return &L->menus[i];
	return NULL;
}

/* The first dock with the given objectName id, or NULL. */
static const struct editor_dock *dock_by_id(const struct editor_layout *L,
					    const char *id)
{
	uint32_t i;

	for (i = 0; i < L->dock_count; i++)
		if (!strcmp(L->docks[i].id, id))
			return &L->docks[i];
	return NULL;
}

/* Does MENU contain an action carrying ACTION-ID (with the expected shortcut)? */
static int has_action(const struct editor_menu *menu, const char *action,
		      enum editor_shortcut shortcut)
{
	uint32_t i;

	if (!menu)
		return 0;
	for (i = 0; i < menu->item_count; i++) {
		const struct editor_menu_item *it = &menu->items[i];

		if (it->kind == EDITOR_ITEM_ACTION &&
		    !strcmp(it->action, action) && it->shortcut == shortcut)
			return 1;
	}
	return 0;
}

static int menu_has_kind(const struct editor_menu *menu,
			 enum editor_menu_item_kind kind)
{
	uint32_t i;

	if (!menu)
		return 0;
	for (i = 0; i < menu->item_count; i++)
		if (menu->items[i].kind == kind)
			return 1;
	return 0;
}

int main(void)
{
	struct editor_layout      L;
	const struct editor_menu *file, *edit, *view, *help;
	const struct editor_dock *assets, *console;
	uint32_t                  i, badges = 0;

	/* The interpreter comes up on demand inside editor_layout_load, exactly
	 * as it does for the Qt shell before it walks the spec. */
	assert(editor_layout_load(&L) == 0 &&
	       "the embedded layout spec must evaluate and walk");

	/* ---- menus: File / Edit / View / Help, in order ------------------ */
	assert(L.menu_count == 4 && "four top-level menus");
	assert(!strcmp(L.menus[0].label, "&File"));
	assert(!strcmp(L.menus[1].label, "&Edit"));
	assert(!strcmp(L.menus[2].label, "&View"));
	assert(!strcmp(L.menus[3].label, "&Help"));

	file = menu_by_label(&L, "&File");
	edit = menu_by_label(&L, "&Edit");
	view = menu_by_label(&L, "&View");
	help = menu_by_label(&L, "&Help");

	/* The wired actions keep their ids and standard-key shortcuts. */
	assert(has_action(file, "open-project", EDITOR_KEY_OPEN) &&
	       "Open Project is the wired File entry");
	assert(has_action(file, "new", EDITOR_KEY_NEW));
	assert(has_action(file, "save", EDITOR_KEY_SAVE));
	assert(has_action(file, "save-as", EDITOR_KEY_SAVE_AS));
	assert(has_action(file, "quit", EDITOR_KEY_QUIT));
	assert(has_action(edit, "undo", EDITOR_KEY_UNDO));
	assert(has_action(edit, "paste", EDITOR_KEY_PASTE));
	assert(has_action(view, "reset-layout", EDITOR_KEY_NONE) &&
	       "Reset Layout is a View entry with no shortcut");
	assert(has_action(help, "about", EDITOR_KEY_NONE));

	/* The View menu's body is the dock-toggle placeholder, not literals. */
	assert(menu_has_kind(view, EDITOR_ITEM_DOCK_TOGGLES) &&
	       "View carries the dock-toggles expansion point");
	assert(menu_has_kind(file, EDITOR_ITEM_SEPARATOR) &&
	       "File keeps its separators");

	/* ---- docks: four, with the assets/console tab group -------------- */
	assert(L.dock_count == 4 && "four docks");
	assert(dock_by_id(&L, "dock.scene") &&
	       dock_by_id(&L, "dock.scene")->area == EDITOR_AREA_LEFT);
	assert(dock_by_id(&L, "dock.inspector") &&
	       dock_by_id(&L, "dock.inspector")->area == EDITOR_AREA_RIGHT);

	assets  = dock_by_id(&L, "dock.assets");
	console = dock_by_id(&L, "dock.console");
	assert(assets && console);
	assert(assets->area == EDITOR_AREA_BOTTOM &&
	       console->area == EDITOR_AREA_BOTTOM);
	assert(assets->raise && "Assets is raised above its tab group");
	assert(!strcmp(console->tabbed_with, "dock.assets") &&
	       "Console is tabbed behind Assets");
	/* The placeholder text survives the round-trip through the spec. */
	assert(!strcmp(console->panel, "Scheme REPL"));

	/* ---- toolbar: Play, Stop, a separator, and one live badge -------- */
	assert(L.tool_count == 4 && "two items, a separator, a badge");
	for (i = 0; i < L.tool_count; i++)
		if (L.tools[i].kind == EDITOR_TOOL_BADGE) {
			badges++;
			assert(!strcmp(L.tools[i].id, "renderer") &&
			       "the toolbar badge is the renderer status");
		}
	assert(badges == 1 && "exactly one toolbar badge");

	/* ---- status bar: fps / resolution / driver ---------------------- */
	assert(L.status_count == 3);
	assert(!strcmp(L.status[0].id, "fps"));
	assert(!strcmp(L.status[1].id, "resolution"));
	assert(!strcmp(L.status[2].id, "driver"));

	printf("editor_layout_test: %u menus, %u docks, %u tools, %u status "
	       "fields\n",
	       L.menu_count, L.dock_count, L.tool_count, L.status_count);
	printf("editor_layout tests passed\n");
	return 0;
}
