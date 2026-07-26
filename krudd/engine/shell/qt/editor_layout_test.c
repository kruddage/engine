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

/* The Nth child of NODE in declaration order, or NULL. Walks first_child then
 * next_sibling, which is the only way to read the arena — so using it here means
 * the test exercises the link structure rather than trusting pool order. */
static const struct editor_body_node *body_child(const struct editor_layout *L,
						 int32_t node, uint32_t n)
{
	int32_t c;

	if (node < 0 || (uint32_t)node >= L->body_count)
		return NULL;
	c = L->bodies[node].first_child;
	while (c >= 0 && n--)
		c = L->bodies[c].next_sibling;
	if (c < 0 || (uint32_t)c >= L->body_count)
		return NULL;
	return &L->bodies[c];
}

/* How many children NODE has, by walking the sibling run. */
static uint32_t body_child_count(const struct editor_layout *L, int32_t node)
{
	uint32_t n = 0;
	int32_t  c;

	if (node < 0)
		return 0;
	for (c = L->bodies[node].first_child; c >= 0;
	     c = L->bodies[c].next_sibling)
		n++;
	return n;
}

/* The index of DOCK's body root, or -1. */
static int32_t dock_body(const struct editor_layout *L, const char *id)
{
	const struct editor_dock *d = dock_by_id(L, id);

	return d ? d->body : -1;
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

/*
 * The panel bodies each dock declares, and the arena's link structure (#795).
 *
 * The contents matter, but the structure matters more: every assertion below
 * reaches its node by walking first_child / next_sibling rather than indexing
 * the pool, so a reader that filled the arena but linked it wrongly — children
 * prepended instead of appended, a sibling run that loops, a parent that does
 * not point back — fails here rather than in a host that draws a scrambled
 * panel.
 */
static void test_bodies(const struct editor_layout *L)
{
	int32_t                        scene, inspector, assets, console, grid;
	const struct editor_body_node *n;
	uint32_t                       i;

	/* Each of the four docks names what its panel is. */
	scene     = dock_body(L, "dock.scene");
	inspector = dock_body(L, "dock.inspector");
	assets    = dock_body(L, "dock.assets");
	console   = dock_body(L, "dock.console");
	assert(scene >= 0 && inspector >= 0 && assets >= 0 && console >= 0 &&
	       "every dock declares a body");

	/* Leaf kinds carry their source and no children. */
	assert(L->bodies[scene].kind == EDITOR_BODY_TREE);
	assert(!strcmp(L->bodies[scene].source, "scene"));
	assert(body_child_count(L, scene) == 0);

	assert(L->bodies[assets].kind == EDITOR_BODY_LIST);
	assert(!strcmp(L->bodies[assets].source, "asset"));

	assert(L->bodies[console].kind == EDITOR_BODY_CONSOLE);
	assert(!strcmp(L->bodies[console].source, "script"));

	/* The Inspector nests: a stack of a property grid and an empty-state
	 * label. This is the case a fixed-shape struct could not have held. */
	assert(L->bodies[inspector].kind == EDITOR_BODY_STACK);
	assert(body_child_count(L, inspector) == 2 &&
	       "the Inspector stacks a grid and a label");

	n = body_child(L, inspector, 0);
	assert(n && n->kind == EDITOR_BODY_PROPERTY_GRID &&
	       "the grid is the stack's FIRST child — declaration order, so "
	       "children are appended, not prepended");
	assert(!strcmp(n->source, "scene.selection"));

	n = body_child(L, inspector, 1);
	assert(n && n->kind == EDITOR_BODY_LABEL &&
	       "the empty-state label follows the grid");
	assert(strstr(n->source, "Select an entity") &&
	       "a label carries its text in `source`");

	/* The grid's rows: four fields, in order, each with a name and a label
	 * — the two-string form. */
	grid = L->bodies[inspector].first_child;
	assert(body_child_count(L, grid) == 4 && "four property rows");
	n = body_child(L, grid, 0);
	assert(n && n->kind == EDITOR_BODY_FIELD);
	assert(!strcmp(n->source, "name") && !strcmp(n->label, "Name") &&
	       "a field's two strings land in source then label");
	n = body_child(L, grid, 3);
	assert(n && !strcmp(n->source, "scale") && !strcmp(n->label, "Scale") &&
	       "the last row is reached by walking the sibling run");

	/* Structural invariants across the whole pool, so a malformed link
	 * cannot hide in a node this test does not name individually. */
	assert(L->body_count > 0 && L->body_count <= EDITOR_MAX_BODY_NODES);
	for (i = 0; i < L->body_count; i++) {
		const struct editor_body_node *b = &L->bodies[i];

		assert(b->kind != EDITOR_BODY_NONE &&
		       "an unknown kind is skipped, never pooled");
		assert(b->parent >= -1 && b->parent < (int32_t)L->body_count);
		assert(b->first_child >= -1 &&
		       b->first_child < (int32_t)L->body_count);
		assert(b->next_sibling >= -1 &&
		       b->next_sibling < (int32_t)L->body_count);
		/* A child is created after its parent, so an index that points
		 * backwards would mean a cycle. */
		assert(b->first_child == -1 || b->first_child > (int32_t)i);
		assert(b->next_sibling == -1 || b->next_sibling > (int32_t)i);
		/* Every child agrees with its parent about the relationship. */
		if (b->first_child >= 0)
			assert(L->bodies[b->first_child].parent == (int32_t)i &&
			       "first_child points back at its parent");
		if (b->next_sibling >= 0)
			assert(L->bodies[b->next_sibling].parent == b->parent &&
			       "siblings share a parent");
	}

	/* Docks the spec never filled keep "no body" rather than index 0. */
	for (i = L->dock_count; i < EDITOR_MAX_DOCKS; i++)
		assert(L->docks[i].body == -1 &&
		       "an unfilled dock reads as no body, not node 0");
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

	/* ---- toolbar: Play, Stop, separator, badge, spacer, badge ------- */
	assert(L.tool_count == 6 &&
	       "two items, a separator, two badges, a spacer");
	for (i = 0; i < L.tool_count; i++)
		if (L.tools[i].kind == EDITOR_TOOL_BADGE) {
			badges++;
			assert((!strcmp(L.tools[i].id, "renderer") ||
				!strcmp(L.tools[i].id, "version")) &&
			       "the toolbar badges are renderer and version");
		}
	assert(badges == 2 && "renderer and version badges");
	/*
	 * The version badge is right-aligned, which the spec expresses as
	 * "declared after the spacer" — order is the whole meaning here, so a
	 * spacer that drifted past it would silently left-align the badge.
	 */
	for (i = 0; i < L.tool_count; i++)
		if (L.tools[i].kind == EDITOR_TOOL_SPACER)
			break;
	assert(i < L.tool_count && "the toolbar has an elastic spacer");
	assert(!strcmp(L.tools[L.tool_count - 1].id, "version") &&
	       "the version badge is last, so the spacer pushes it right");

	/* ---- status bar: fps / resolution / driver ---------------------- */
	assert(L.status_count == 3);
	assert(!strcmp(L.status[0].id, "fps"));
	assert(!strcmp(L.status[1].id, "resolution"));
	assert(!strcmp(L.status[2].id, "driver"));

	/* ---- panel bodies (#795) ---------------------------------------- */
	test_bodies(&L);

	printf("editor_layout_test: %u menus, %u docks, %u tools, %u status "
	       "fields, %u body nodes\n",
	       L.menu_count, L.dock_count, L.tool_count, L.status_count,
	       L.body_count);
	printf("editor_layout tests passed\n");
	return 0;
}
