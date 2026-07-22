/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * editor_layout — the s7 reader behind editor_layout.h.
 *
 * It evaluates the embedded layout spec (LAYOUT_SCM, core/editor_layout.scm)
 * into the shared interpreter, calls (editor-layout), and walks the returned
 * data tree into a plain C `struct editor_layout`. The walk is the same shape
 * as script.c's query_params(): call a Scheme procedure, then descend the
 * resulting list with s7_car / s7_cdr, reading each field defensively so a
 * malformed form degrades to a default rather than trapping. Keeping the s7 out
 * of krudd_qt.cpp lets the Qt shell and the Qt-free CI test (editor_layout_test)
 * read one spec through one reader.
 */
#include "editor_layout.h"

#include "script.h"
#include "s7.h"

#include "editor_layout_scm.h"  /* LAYOUT_SCM — the embedded spec */

#include <string.h>

/* The Nth element of LST, or nil when the list is shorter — never the s7
 * range error s7_list_ref would raise, so a short authored form is safe. */
static s7_pointer nth(s7_scheme *sc, s7_pointer lst, int n)
{
	while (n-- > 0 && s7_is_pair(lst))
		lst = s7_cdr(lst);
	return s7_is_pair(lst) ? s7_car(lst) : s7_nil(sc);
}

/* Copy an s7 string (or a symbol's name) into a fixed buffer, always
 * terminated. A value that is neither yields "". Same bounded copy as
 * script.c's copy_field. */
static void copy_str(char *dst, size_t cap, s7_pointer s)
{
	const char *src = "";
	size_t      n;

	if (s7_is_string(s))
		src = s7_string(s);
	else if (s7_is_symbol(s))
		src = s7_symbol_name(s);
	if (cap == 0)
		return;
	n = strlen(src);
	if (n >= cap)
		n = cap - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

/* The head symbol of a tagged form ("action", "dock", …), or "" if the form is
 * not a pair headed by a symbol. */
static const char *head_sym(s7_pointer form)
{
	if (s7_is_pair(form) && s7_is_symbol(s7_car(form)))
		return s7_symbol_name(s7_car(form));
	return "";
}

static enum editor_shortcut shortcut_of(s7_pointer s)
{
	const char *n = s7_is_symbol(s) ? s7_symbol_name(s) : "";

	if (!strcmp(n, "new"))     return EDITOR_KEY_NEW;
	if (!strcmp(n, "open"))    return EDITOR_KEY_OPEN;
	if (!strcmp(n, "save"))    return EDITOR_KEY_SAVE;
	if (!strcmp(n, "save-as")) return EDITOR_KEY_SAVE_AS;
	if (!strcmp(n, "quit"))    return EDITOR_KEY_QUIT;
	if (!strcmp(n, "undo"))    return EDITOR_KEY_UNDO;
	if (!strcmp(n, "redo"))    return EDITOR_KEY_REDO;
	if (!strcmp(n, "cut"))     return EDITOR_KEY_CUT;
	if (!strcmp(n, "copy"))    return EDITOR_KEY_COPY;
	if (!strcmp(n, "paste"))   return EDITOR_KEY_PASTE;
	return EDITOR_KEY_NONE;
}

static enum editor_dock_area area_of(s7_pointer s)
{
	const char *n = s7_is_symbol(s) ? s7_symbol_name(s) : "";

	if (!strcmp(n, "right"))  return EDITOR_AREA_RIGHT;
	if (!strcmp(n, "top"))    return EDITOR_AREA_TOP;
	if (!strcmp(n, "bottom")) return EDITOR_AREA_BOTTOM;
	return EDITOR_AREA_LEFT;
}

/* (menus (menu LABEL ITEM ...) ...) */
static void parse_menus(s7_scheme *sc, s7_pointer section,
			struct editor_layout *out)
{
	s7_pointer m;

	for (m = s7_cdr(section);
	     s7_is_pair(m) && out->menu_count < EDITOR_MAX_MENUS;
	     m = s7_cdr(m)) {
		s7_pointer          menu = s7_car(m), it;
		struct editor_menu *dst  = &out->menus[out->menu_count];

		if (strcmp(head_sym(menu), "menu"))
			continue;
		copy_str(dst->label, sizeof dst->label, nth(sc, menu, 1));

		for (it = s7_cdr(s7_cdr(menu));
		     s7_is_pair(it) && dst->item_count < EDITOR_MAX_MENU_ITEMS;
		     it = s7_cdr(it)) {
			s7_pointer               item = s7_car(it);
			struct editor_menu_item *di =
				&dst->items[dst->item_count];
			const char              *tag = head_sym(item);

			if (!strcmp(tag, "action")) {
				di->kind = EDITOR_ITEM_ACTION;
				copy_str(di->label, sizeof di->label,
					 nth(sc, item, 1));
				di->shortcut = shortcut_of(nth(sc, item, 2));
				copy_str(di->action, sizeof di->action,
					 nth(sc, item, 3));
			} else if (!strcmp(tag, "separator")) {
				di->kind = EDITOR_ITEM_SEPARATOR;
			} else if (!strcmp(tag, "dock-toggles")) {
				di->kind = EDITOR_ITEM_DOCK_TOGGLES;
			} else {
				continue; /* unknown item — skip, don't count */
			}
			dst->item_count++;
		}
		out->menu_count++;
	}
}

/* (toolbar (item LABEL ID) | (separator) | (badge ID TEXT) ...) */
static void parse_toolbar(s7_scheme *sc, s7_pointer section,
			  struct editor_layout *out)
{
	s7_pointer t;

	for (t = s7_cdr(section);
	     s7_is_pair(t) && out->tool_count < EDITOR_MAX_TOOLS;
	     t = s7_cdr(t)) {
		s7_pointer          item = s7_car(t);
		struct editor_tool *dst  = &out->tools[out->tool_count];
		const char         *tag  = head_sym(item);

		if (!strcmp(tag, "item")) {
			dst->kind = EDITOR_TOOL_ITEM;
			copy_str(dst->label, sizeof dst->label, nth(sc, item, 1));
			copy_str(dst->id, sizeof dst->id, nth(sc, item, 2));
		} else if (!strcmp(tag, "separator")) {
			dst->kind = EDITOR_TOOL_SEPARATOR;
		} else if (!strcmp(tag, "badge")) {
			dst->kind = EDITOR_TOOL_BADGE;
			copy_str(dst->id, sizeof dst->id, nth(sc, item, 1));
			copy_str(dst->label, sizeof dst->label, nth(sc, item, 2));
		} else {
			continue; /* unknown item — skip, don't count */
		}
		out->tool_count++;
	}
}

/* (docks (dock ID TITLE AREA PANEL BLURB EXTRA ...) ...) */
static void parse_docks(s7_scheme *sc, s7_pointer section,
			struct editor_layout *out)
{
	s7_pointer d;

	for (d = s7_cdr(section);
	     s7_is_pair(d) && out->dock_count < EDITOR_MAX_DOCKS;
	     d = s7_cdr(d)) {
		s7_pointer          dock = s7_car(d), ex;
		struct editor_dock *dst  = &out->docks[out->dock_count];

		if (strcmp(head_sym(dock), "dock"))
			continue;
		copy_str(dst->id,    sizeof dst->id,    nth(sc, dock, 1));
		copy_str(dst->title, sizeof dst->title, nth(sc, dock, 2));
		dst->area = area_of(nth(sc, dock, 3));
		copy_str(dst->panel, sizeof dst->panel, nth(sc, dock, 4));
		copy_str(dst->blurb, sizeof dst->blurb, nth(sc, dock, 5));

		/* Any trailing (tabbed-with ID) / (raise) forms. */
		for (ex = s7_cdr(dock);
		     s7_is_pair(ex);
		     ex = s7_cdr(ex)) {
			s7_pointer  e   = s7_car(ex);
			const char *tag = head_sym(e);

			if (!strcmp(tag, "tabbed-with"))
				copy_str(dst->tabbed_with,
					 sizeof dst->tabbed_with,
					 nth(sc, e, 1));
			else if (!strcmp(tag, "raise"))
				dst->raise = 1;
		}
		out->dock_count++;
	}
}

/* (statusbar (field ID TEXT) ...) */
static void parse_statusbar(s7_scheme *sc, s7_pointer section,
			    struct editor_layout *out)
{
	s7_pointer f;

	for (f = s7_cdr(section);
	     s7_is_pair(f) && out->status_count < EDITOR_MAX_STATUS;
	     f = s7_cdr(f)) {
		s7_pointer                  field = s7_car(f);
		struct editor_status_field *dst =
			&out->status[out->status_count];

		if (strcmp(head_sym(field), "field"))
			continue;
		copy_str(dst->id,   sizeof dst->id,   nth(sc, field, 1));
		copy_str(dst->text, sizeof dst->text, nth(sc, field, 2));
		out->status_count++;
	}
}

int editor_layout_load(struct editor_layout *out)
{
	s7_scheme *sc;
	s7_pointer fn, tree, s;

	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));

	sc = script_s7(); /* starts the interpreter on first use */
	if (!sc)
		return -1;
	if (script_eval(LAYOUT_SCM) != 0)
		return -1;

	fn = s7_name_to_value(sc, "editor-layout");
	if (!s7_is_procedure(fn))
		return -1;
	tree = s7_call(sc, fn, s7_nil(sc));
	if (!s7_is_pair(tree))
		return -1;

	for (s = tree; s7_is_pair(s); s = s7_cdr(s)) {
		s7_pointer  section = s7_car(s);
		const char *tag     = head_sym(section);

		if (!strcmp(tag, "menus"))
			parse_menus(sc, section, out);
		else if (!strcmp(tag, "toolbar"))
			parse_toolbar(sc, section, out);
		else if (!strcmp(tag, "docks"))
			parse_docks(sc, section, out);
		else if (!strcmp(tag, "statusbar"))
			parse_statusbar(sc, section, out);
	}
	return 0;
}
