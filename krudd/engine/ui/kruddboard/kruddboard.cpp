/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * kruddboard — engine debug overlay plugin.
 *
 * Single full-width ImGui window anchored to the top of the viewport.
 * Height auto-sizes to the active tab's content; the Log section caps at
 * ~88 % of the viewport height and scrolls internally.  The tab bar
 * uses FittingPolicyScroll to handle overflow on narrow / phone screens.
 * Toggle visibility with backtick (`).
 *
 * Tabs:
 *   Scene      — entity list, create/delete, inspector
 *   Assets     — asset browser and markdown editor
 *   KRUDD      — frame stats, subsystems, log (collapsible sections)
 */

extern "C" {
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"
#include "log_level.h"
#include "stats_api.h"
#include "imgui_api.h"
#include "asset_api.h"
#include "entity_api.h"
#include "edit_api.h"
#include "camera_api.h"
#include "preview_api.h"
#include "mesh.h"
#include "memory_api.h"
#ifdef __EMSCRIPTEN__
#include "backend_api.h"
#include "script.h"
#include "mesh_script.h"
#include "texture_script.h"
#endif
}

#include "imgui.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#include <GLES3/gl3.h>
#include <string.h>
#include <math.h>
#include "md_parse.h"
#include "md_draw.h"
#include "s7.h"			/* self-guards for C++ linkage */
#include "kruddboard_scm.h"	/* KRUDDBOARD_SCM — the panel image */
#include "assets_scm.h"		/* ASSETS_SCM — the Assets-tab panel image */

/*
 * Soft-keyboard toggle (plugin_abi.c, main module — see imgui_plugin.cpp
 * for the fuller comment on the bridge).  On a touch device kruddboard owns
 * krudd_text_input_show/hide directly via the top-right button below;
 * imgui_plugin's own WantTextInput-driven auto-focus is disabled for touch
 * devices precisely so the two don't fight over who's driving the keyboard.
 */
extern "C" void krudd_text_input_show(void);
extern "C" void krudd_text_input_hide(void);
extern "C" int  krudd_is_touch_device(void);
#endif

#include <cstdio>
#include <cfloat>

static const struct log_api           *g_log;
static const struct stats_api         *g_stats;
static const struct asset_api         *g_asset_api;
static const struct memory_api        *g_mem; /* for click-to-pick mesh gen */
static const struct subsystem_manager *g_mgr;
static int                             g_visible = 1;
static int                             g_collapsed = 1;
#ifdef __EMSCRIPTEN__
static bool                            g_touch_device;
static bool                            g_kbd_shown;
#endif
static int                             g_panels_registered;
static uint32_t                        asset_id_by_path(const char *path);
static const struct entity_api        *g_entity_api;
static int32_t                         g_entity_sel = -1; /* -1 = none */
static const struct edit_api          *g_edit_api;  /* NULL = no history */
static const struct camera_api        *g_camera_api; /* NULL = no viewport gizmo */
static const struct preview_api       *g_preview_api; /* NULL = no mesh preview */

/* Transform gizmo mode, shared between the viewport handles and the World tab. */
enum gizmo_mode {
	GIZMO_MOVE,
	GIZMO_ROTATE,
	GIZMO_SCALE,
};
static enum gizmo_mode g_gizmo_mode = GIZMO_MOVE;

#ifdef __EMSCRIPTEN__
static const struct asset_mut_api     *g_asset_mut;
static const struct backend_api       *g_backend;
#endif

/* ------------------------------------------------------------------ */
/* Visibility toggle                                                   */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
static EM_BOOL on_keydown(int /*type*/, const EmscriptenKeyboardEvent *e,
			  void * /*ud*/)
{
	bool ctrl;

	if (strcmp(e->code, "Backquote") == 0) {
		g_visible = !g_visible;
		return EM_TRUE;
	}

	/*
	 * Global undo/redo: Ctrl+Z undoes, Ctrl+Y / Ctrl+Shift+Z redo (Cmd on
	 * mac). Skip entirely when a text widget is capturing keys so a focused
	 * markdown/shader field keeps its own stb_textedit undo — the shortcut
	 * must never both edit text and pop the global stack in one press. No
	 * "edit" service (older engine build) means these are safe no-ops.
	 */
	if (!g_edit_api || ImGui::GetIO().WantTextInput)
		return EM_FALSE;

	ctrl = e->ctrlKey || e->metaKey;
	if (!ctrl)
		return EM_FALSE;

	if (strcmp(e->code, "KeyZ") == 0) {
		if (e->shiftKey)
			g_edit_api->redo();
		else
			g_edit_api->undo();
		return EM_TRUE;
	}
	if (strcmp(e->code, "KeyY") == 0) {
		g_edit_api->redo();
		return EM_TRUE;
	}
	return EM_FALSE;
}
#endif

/* ------------------------------------------------------------------ */
/* Scheme panel host                                                   */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
/*
 * The seam that lets a kruddboard panel be authored in Scheme instead of C++.
 * The image (kruddboard.scm, embedded as KRUDDBOARD_SCM) draws through the
 * primitives registered here against the shared s7 interpreter — the same one
 * the shader DSL and the runtime tick already run in. A primitive only issues
 * an ImGui call, so a panel procedure must be invoked while a frame is open;
 * call_scm_panel() does exactly that at draw time. Nothing here touches the
 * engine ABI — only kruddboard's own tabs cross this seam, one at a time.
 */

/*
 * The three primitives below are s7 callbacks, so they carry C language
 * linkage to match the s7_function pointer type they are registered as.
 *
 * (imgui-text str) -> unspecified. Draw one line. A non-string argument is
 * ignored rather than trapped, so a malformed call in the image cannot take
 * the frame down.
 */
extern "C" {

static s7_pointer sp_imgui_text(s7_scheme *sc, s7_pointer args)
{
	s7_pointer str = s7_car(args);

	if (s7_is_string(str))
		ImGui::TextUnformatted(s7_string(str));
	return s7_unspecified(sc);
}

/* (imgui-text-disabled str) -> unspecified. As imgui-text, dimmed. */
static s7_pointer sp_imgui_text_disabled(s7_scheme *sc, s7_pointer args)
{
	s7_pointer str = s7_car(args);

	if (s7_is_string(str))
		ImGui::TextDisabled("%s", s7_string(str));
	return s7_unspecified(sc);
}

/* (krudd-stats) -> (fps frame-ms frame-count), or #f when stats are absent. */
static s7_pointer sp_krudd_stats(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	if (!g_stats)
		return s7_f(sc);
	return s7_list(sc, 3,
		       s7_make_real(sc, (s7_double)g_stats->fps_avg),
		       s7_make_real(sc, (s7_double)g_stats->last_frame_ms),
		       s7_make_integer(sc, (s7_int)g_stats->frame_count));
}

/*
 * (imgui-text-colored r g b a str) -> unspecified. Draw one line in an RGBA
 * colour (each channel 0..1). A non-string str is ignored, like imgui-text.
 */
static s7_pointer sp_imgui_text_colored(s7_scheme *sc, s7_pointer args)
{
	s7_pointer p   = args;
	double     r   = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double     g   = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double     b   = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double     a   = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	s7_pointer str = s7_car(p);

	if (s7_is_string(str))
		ImGui::TextColored(ImVec4((float)r, (float)g, (float)b,
					  (float)a), "%s", s7_string(str));
	return s7_unspecified(sc);
}

/* (imgui-small-button label) -> #t when clicked this frame, else #f. */
static s7_pointer sp_imgui_small_button(s7_scheme *sc, s7_pointer args)
{
	s7_pointer label = s7_car(args);
	bool       hit   = false;

	if (s7_is_string(label))
		hit = ImGui::SmallButton(s7_string(label));
	return s7_make_boolean(sc, hit);
}

/* (imgui-same-line) -> unspecified. Keep the next widget on this line. */
static s7_pointer sp_imgui_same_line(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	ImGui::SameLine();
	return s7_unspecified(sc);
}

/*
 * (imgui-checkbox label state) -> the (possibly toggled) boolean. s7 has no
 * out-parameter, so the image passes the current state and stores the result,
 * which flips on a click.
 */
static s7_pointer sp_imgui_checkbox(s7_scheme *sc, s7_pointer args)
{
	s7_pointer label = s7_car(args);
	bool       state = s7_boolean(sc, s7_cadr(args));

	if (s7_is_string(label))
		ImGui::Checkbox(s7_string(label), &state);
	return s7_make_boolean(sc, state);
}

/* (imgui-separator) -> unspecified. */
static s7_pointer sp_imgui_separator(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	ImGui::Separator();
	return s7_unspecified(sc);
}

/*
 * (imgui-collapsing-header label [default-open]) -> #t when the section is
 * open. default-open is optional and defaults to #t, matching the C headers
 * that passed ImGuiTreeNodeFlags_DefaultOpen; pass #f to start the section
 * collapsed instead (used by the Subsystems section, which starts rolled up).
 */
static s7_pointer sp_imgui_collapsing_header(s7_scheme *sc, s7_pointer args)
{
	s7_pointer label        = s7_car(args);
	s7_pointer rest         = s7_cdr(args);
	bool       default_open = s7_is_pair(rest) ?
				   s7_boolean(sc, s7_car(rest)) : true;
	bool       open         = false;

	if (s7_is_string(label)) {
		open = ImGui::CollapsingHeader(s7_string(label),
					      default_open ?
					      ImGuiTreeNodeFlags_DefaultOpen : 0);
	}
	return s7_make_boolean(sc, open);
}

/*
 * (imgui-begin-table id ncols) -> #t when the table opened. Bordered, row-
 * striped, proportional-stretch — the flag set every kruddboard table used. A
 * #t result must be paired with imgui-end-table.
 */
static s7_pointer sp_imgui_begin_table(s7_scheme *sc, s7_pointer args)
{
	s7_pointer id     = s7_car(args);
	s7_pointer ncols  = s7_cadr(args);
	bool       opened = false;

	if (s7_is_string(id) && s7_is_integer(ncols) && s7_integer(ncols) > 0)
		opened = ImGui::BeginTable(s7_string(id),
					   (int)s7_integer(ncols),
					   ImGuiTableFlags_Borders |
					   ImGuiTableFlags_RowBg   |
					   ImGuiTableFlags_SizingStretchProp);
	return s7_make_boolean(sc, opened);
}

/* (imgui-table-setup-column label) -> unspecified. */
static s7_pointer sp_imgui_table_setup_column(s7_scheme *sc, s7_pointer args)
{
	s7_pointer label = s7_car(args);

	if (s7_is_string(label))
		ImGui::TableSetupColumn(s7_string(label));
	return s7_unspecified(sc);
}

/* (imgui-table-headers-row) -> unspecified. */
static s7_pointer sp_imgui_table_headers_row(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	ImGui::TableHeadersRow();
	return s7_unspecified(sc);
}

/* (imgui-table-next-row) -> unspecified. */
static s7_pointer sp_imgui_table_next_row(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	ImGui::TableNextRow();
	return s7_unspecified(sc);
}

/*
 * (imgui-table-next-column) -> #t if the new cell is visible. Advances to the
 * next column (ImGui::TableNextColumn), which after a row lands on column 0.
 */
static s7_pointer sp_imgui_table_next_column(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return s7_make_boolean(sc, ImGui::TableNextColumn());
}

/* (imgui-end-table) -> unspecified. */
static s7_pointer sp_imgui_end_table(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	ImGui::EndTable();
	return s7_unspecified(sc);
}

/*
 * (imgui-begin-child id w h) -> unspecified. A fixed-size scroll region with a
 * horizontal scrollbar, as the log view used. Always paired with
 * imgui-end-child. A zero width/height means "fill the available axis".
 */
static s7_pointer sp_imgui_begin_child(s7_scheme *sc, s7_pointer args)
{
	s7_pointer p  = args;
	s7_pointer id = s7_car(p); p = s7_cdr(p);
	double     w  = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double     h  = s7_number_to_real(sc, s7_car(p));

	if (s7_is_string(id))
		ImGui::BeginChild(s7_string(id),
				  ImVec2((float)w, (float)h), false,
				  ImGuiWindowFlags_HorizontalScrollbar);
	return s7_unspecified(sc);
}

/* (imgui-end-child) -> unspecified. */
static s7_pointer sp_imgui_end_child(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	ImGui::EndChild();
	return s7_unspecified(sc);
}

/* (imgui-set-scroll-here-y ratio) -> unspecified. Snap the scroll to ratio. */
static s7_pointer sp_imgui_set_scroll_here_y(s7_scheme *sc, s7_pointer args)
{
	ImGui::SetScrollHereY((float)s7_number_to_real(sc, s7_car(args)));
	return s7_unspecified(sc);
}

/* (imgui-viewport-work-height) -> the main viewport's usable height in px. */
static s7_pointer sp_imgui_viewport_work_height(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return s7_make_real(sc,
			    (s7_double)ImGui::GetMainViewport()->WorkSize.y);
}

/* One (name api? tick? wasm-size) row for the subsystems table. */
static s7_pointer subsystem_row(s7_scheme *sc, const struct subsystem *s)
{
	return s7_list(sc, 4,
		       s7_make_string(sc, s->name),
		       s7_make_boolean(sc, s->api  != NULL),
		       s7_make_boolean(sc, s->tick != NULL),
		       s7_make_integer(sc, (s7_int)s->wasm_size));
}

/*
 * (krudd-subsystems) -> a list of (name api? tick? wasm-size) rows in table
 * order (static table then dynamic), or #f when the manager is absent. api?
 * and tick? are booleans; wasm-size is an integer (0 = unknown).
 */
static s7_pointer sp_krudd_subsystems(s7_scheme *sc, s7_pointer args)
{
	s7_pointer out;
	int        i;

	(void)args;
	if (!g_mgr)
		return s7_f(sc);

	out = s7_nil(sc);
	for (i = 0; g_mgr->static_table[i].name; i++)
		out = s7_cons(sc, subsystem_row(sc, &g_mgr->static_table[i]),
			      out);
	for (i = 0; i < g_mgr->dynamic_count; i++)
		out = s7_cons(sc, subsystem_row(sc, &g_mgr->dynamic[i]), out);
	return s7_reverse(sc, out);
}

/*
 * (krudd-log-history) -> a list of (level . text) pairs oldest-first, or #f
 * when the log subsystem is absent. level is a log_level integer.
 */
static s7_pointer sp_krudd_log_history(s7_scheme *sc, s7_pointer args)
{
	static struct log_message msgs[LOG_HISTORY_CAP];
	uint32_t                  count, i;
	s7_pointer                out;

	(void)args;
	if (!g_log)
		return s7_f(sc);

	count = g_log->get_history(msgs, LOG_HISTORY_CAP);
	out   = s7_nil(sc);
	for (i = 0; i < count; i++) {
		uint32_t j = count - 1 - i;

		out = s7_cons(sc,
			      s7_cons(sc,
				      s7_make_integer(sc, (s7_int)msgs[j].level),
				      s7_make_string(sc, msgs[j].text)),
			      out);
	}
	return out;
}

/* ------------------------------------------------------------------ */
/* World-tab primitives (#401)                                         */
/* ------------------------------------------------------------------ */

/* Read n reals off a Scheme list into a float array (short lists pad nothing). */
static void read_reals(s7_scheme *sc, s7_pointer lst, float *out, int n)
{
	int i;

	for (i = 0; i < n && s7_is_pair(lst); i++) {
		out[i] = (float)s7_number_to_real(sc, s7_car(lst));
		lst    = s7_cdr(lst);
	}
}

/* Build a fresh (x y z ...) list of n reals from a float array. */
static s7_pointer real_list(s7_scheme *sc, const float *v, int n)
{
	s7_pointer out = s7_nil(sc);
	int        i;

	for (i = n - 1; i >= 0; i--)
		out = s7_cons(sc, s7_make_real(sc, (s7_double)v[i]), out);
	return out;
}

/*
 * (imgui-begin-table-plain id ncols) -> #t when the table opened. Proportional-
 * stretch but borderless and unstriped — the layout-only tables the inspector
 * used (transform, details, bindings), distinct from the bordered
 * imgui-begin-table. A #t result must be paired with imgui-end-table.
 */
static s7_pointer sp_imgui_begin_table_plain(s7_scheme *sc, s7_pointer args)
{
	s7_pointer id     = s7_car(args);
	s7_pointer ncols  = s7_cadr(args);
	bool       opened = false;

	if (s7_is_string(id) && s7_is_integer(ncols) && s7_integer(ncols) > 0)
		opened = ImGui::BeginTable(s7_string(id),
					   (int)s7_integer(ncols),
					   ImGuiTableFlags_SizingStretchProp);
	return s7_make_boolean(sc, opened);
}

/* (imgui-table-setup-column-fixed label width) -> unspecified. Fixed px width. */
static s7_pointer sp_imgui_table_setup_column_fixed(s7_scheme *sc,
						    s7_pointer args)
{
	s7_pointer label = s7_car(args);
	float      width = (float)s7_number_to_real(sc, s7_cadr(args));

	if (s7_is_string(label))
		ImGui::TableSetupColumn(s7_string(label),
					ImGuiTableColumnFlags_WidthFixed, width);
	return s7_unspecified(sc);
}

/*
 * (imgui-begin-disabled flag) -> unspecified. Grey out and swallow input for
 * every widget until imgui-end-disabled — the BeginDisabled/EndDisabled pairs
 * the World tab wrapped its create/edit widgets in when the scene api is absent.
 */
static s7_pointer sp_imgui_begin_disabled(s7_scheme *sc, s7_pointer args)
{
	ImGui::BeginDisabled(s7_boolean(sc, s7_car(args)));
	return s7_unspecified(sc);
}

/* (imgui-end-disabled) -> unspecified. */
static s7_pointer sp_imgui_end_disabled(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	ImGui::EndDisabled();
	return s7_unspecified(sc);
}

/* (imgui-set-next-item-width w) -> unspecified. A negative w fills to the right. */
static s7_pointer sp_imgui_set_next_item_width(s7_scheme *sc, s7_pointer args)
{
	ImGui::SetNextItemWidth((float)s7_number_to_real(sc, s7_car(args)));
	return s7_unspecified(sc);
}

/*
 * (imgui-input-text id text) -> (current-text . committed?). Seeds a single-line
 * field from text each frame; ImGui keeps the in-progress edit alive while the
 * field is focused, so re-seeding from the model is a no-op mid-edit. committed?
 * is #t only on the frame focus is lost after an edit — the "commit once, on
 * deactivate" the C name field used so the name blob doesn't churn per keystroke.
 */
static s7_pointer sp_imgui_input_text(s7_scheme *sc, s7_pointer args)
{
	static char buf[256];
	s7_pointer  id   = s7_car(args);
	s7_pointer  text = s7_cadr(args);
	bool        done;

	buf[0] = '\0';
	if (s7_is_string(text)) {
		strncpy(buf, s7_string(text), sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = '\0';
	}
	if (s7_is_string(id))
		ImGui::InputText(s7_string(id), buf, sizeof(buf));
	done = ImGui::IsItemDeactivatedAfterEdit();
	return s7_cons(sc, s7_make_string(sc, buf), s7_make_boolean(sc, done));
}

/* (imgui-input-float3 id (x y z)) -> ((x y z) . changed?). */
static s7_pointer sp_imgui_input_float3(s7_scheme *sc, s7_pointer args)
{
	s7_pointer id      = s7_car(args);
	float      v[3];
	bool       changed = false;

	read_reals(sc, s7_cadr(args), v, 3);
	if (s7_is_string(id))
		changed = ImGui::InputFloat3(s7_string(id), v);
	return s7_cons(sc, real_list(sc, v, 3), s7_make_boolean(sc, changed));
}

/* (imgui-input-float4 id (x y z w)) -> ((x y z w) . changed?). */
static s7_pointer sp_imgui_input_float4(s7_scheme *sc, s7_pointer args)
{
	s7_pointer id      = s7_car(args);
	float      v[4];
	bool       changed = false;

	read_reals(sc, s7_cadr(args), v, 4);
	if (s7_is_string(id))
		changed = ImGui::InputFloat4(s7_string(id), v);
	return s7_cons(sc, real_list(sc, v, 4), s7_make_boolean(sc, changed));
}

/*
 * (imgui-begin-combo id preview) -> #t when the dropdown is open. A #t result
 * must be paired with imgui-end-combo and holds the selectable rows.
 */
static s7_pointer sp_imgui_begin_combo(s7_scheme *sc, s7_pointer args)
{
	s7_pointer id      = s7_car(args);
	s7_pointer preview = s7_cadr(args);
	bool       open    = false;

	if (s7_is_string(id) && s7_is_string(preview))
		open = ImGui::BeginCombo(s7_string(id), s7_string(preview));
	return s7_make_boolean(sc, open);
}

/* (imgui-end-combo) -> unspecified. */
static s7_pointer sp_imgui_end_combo(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	ImGui::EndCombo();
	return s7_unspecified(sc);
}

/*
 * (imgui-selectable label selected? span-all?) -> #t when clicked this frame.
 * span-all? spans every table column, as the entity-list rows did; the combo
 * rows pass #f.
 */
static s7_pointer sp_imgui_selectable(s7_scheme *sc, s7_pointer args)
{
	s7_pointer           label = s7_car(args);
	bool                 sel   = s7_boolean(sc, s7_cadr(args));
	bool                 span  = s7_boolean(sc, s7_caddr(args));
	ImGuiSelectableFlags flags = span ? ImGuiSelectableFlags_SpanAllColumns
					  : 0;
	bool                 hit   = false;

	if (s7_is_string(label))
		hit = ImGui::Selectable(s7_string(label), sel, flags);
	return s7_make_boolean(sc, hit);
}

/*
 * (imgui-tree-node id label leaf? selected?) -> (open? . clicked?). Draws a
 * tree node in the table's current cell. id is the ImGui identifier and must
 * be unique across the whole tree (an asset's own path, or an accumulated
 * folder prefix, both satisfy this); label is the text actually shown. A
 * non-leaf node only toggles open on its arrow or a double-click, so a plain
 * click on the label is unambiguously a select, mirroring imgui-selectable's
 * contract; a leaf node draws a bullet instead of an arrow, never carries
 * children, and always reports open? as #f. When open? is #t for a non-leaf
 * node the caller must draw its children and then call imgui-tree-pop.
 */
static s7_pointer sp_imgui_tree_node(s7_scheme *sc, s7_pointer args)
{
	s7_pointer id       = s7_car(args);
	s7_pointer label    = s7_cadr(args);
	bool       leaf     = s7_boolean(sc, s7_caddr(args));
	bool       selected = s7_boolean(sc, s7_cadddr(args));
	bool       open     = false;
	bool       clicked  = false;

	if (s7_is_string(id) && s7_is_string(label)) {
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;

		if (selected)
			flags |= ImGuiTreeNodeFlags_Selected;
		if (leaf)
			flags |= ImGuiTreeNodeFlags_Leaf |
				 ImGuiTreeNodeFlags_Bullet |
				 ImGuiTreeNodeFlags_NoTreePushOnOpen;
		else
			flags |= ImGuiTreeNodeFlags_OpenOnArrow |
				 ImGuiTreeNodeFlags_OpenOnDoubleClick;
		open    = ImGui::TreeNodeEx(s7_string(id), flags, "%s",
					    s7_string(label));
		clicked = ImGui::IsItemClicked();
	}
	return s7_cons(sc, s7_make_boolean(sc, open),
		       s7_make_boolean(sc, clicked));
}

/*
 * (imgui-tree-pop) -> unspecified. Pairs with a non-leaf imgui-tree-node
 * call whose open? came back #t.
 */
static s7_pointer sp_imgui_tree_pop(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	ImGui::TreePop();
	return s7_unspecified(sc);
}

/* (imgui-set-item-default-focus) -> unspecified. Focus the last item on open. */
static s7_pointer sp_imgui_set_item_default_focus(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	ImGui::SetItemDefaultFocus();
	return s7_unspecified(sc);
}

/* (imgui-calc-text-width str) -> the rendered width of str in pixels. */
static s7_pointer sp_imgui_calc_text_width(s7_scheme *sc, s7_pointer args)
{
	s7_pointer str = s7_car(args);
	float      w   = 0.0f;

	if (s7_is_string(str))
		w = ImGui::CalcTextSize(s7_string(str)).x;
	return s7_make_real(sc, (s7_double)w);
}

/*
 * (imgui-same-line-right px) -> unspecified. Place the next widget flush to the
 * window's right edge, reserving px for its own width — the CalcTextSize-based
 * right alignment the C scene header used for the "Save As..." button.
 */
static s7_pointer sp_imgui_same_line_right(s7_scheme *sc, s7_pointer args)
{
	float       px = (float)s7_number_to_real(sc, s7_car(args));
	ImGuiStyle &st = ImGui::GetStyle();

	ImGui::SameLine();
	ImGui::SetCursorPosX(ImGui::GetWindowWidth() - px
			     - st.FramePadding.x * 2.0f - st.WindowPadding.x);
	return s7_unspecified(sc);
}

/* (imgui-push-style-color-button r g b a) -> unspecified. Tint the button fill. */
static s7_pointer sp_imgui_push_style_color_button(s7_scheme *sc, s7_pointer args)
{
	s7_pointer p = args;
	double     r = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double     g = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double     b = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double     a = s7_number_to_real(sc, s7_car(p));

	ImGui::PushStyleColor(ImGuiCol_Button,
			      ImVec4((float)r, (float)g, (float)b, (float)a));
	return s7_unspecified(sc);
}

/* (imgui-pop-style-color) -> unspecified. Undo one imgui-push-style-color-*. */
static s7_pointer sp_imgui_pop_style_color(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	ImGui::PopStyleColor();
	return s7_unspecified(sc);
}

/*
 * (imgui-push-id str) -> unspecified. Open an id scope: every widget id drawn
 * until the matching imgui-pop-id is hashed against str, so two draws of the
 * same panel under different ids (the World tree opens one inspector body per
 * entity, all sharing "##ename" / "##pos" / "##sp") never collide. Pairs with
 * imgui-pop-id exactly as ImGui::PushID/PopID do.
 */
static s7_pointer sp_imgui_push_id(s7_scheme *sc, s7_pointer args)
{
	s7_pointer id = s7_car(args);

	if (s7_is_string(id))
		ImGui::PushID(s7_string(id));
	else
		ImGui::PushID((int)s7_integer(id));
	return s7_unspecified(sc);
}

/* (imgui-pop-id) -> unspecified. Close one imgui-push-id scope. */
static s7_pointer sp_imgui_pop_id(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	ImGui::PopID();
	return s7_unspecified(sc);
}

/*
 * (imgui-dot r g b a) -> unspecified. Draw a small filled circle inline at the
 * cursor and advance past it, so a caller can same-line one after a widget as
 * an "overridden" marker. Uses the window draw list, not a font glyph — the
 * built-in ProggyClean face has no bullet, so text "*" would be all that is
 * available; a draw-list circle reads as the mockup's dot on any font.
 */
static s7_pointer sp_imgui_dot(s7_scheme *sc, s7_pointer args)
{
	s7_pointer p = args;
	double     r = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double     g = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double     b = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double     a = s7_number_to_real(sc, s7_car(p));

	ImDrawList *dl  = ImGui::GetWindowDrawList();
	ImVec2      pos = ImGui::GetCursorScreenPos();
	float       lh  = ImGui::GetTextLineHeight();
	float       rad = lh * 0.28f;
	ImVec2      c(pos.x + rad + 2.0f, pos.y + lh * 0.5f);

	dl->AddCircleFilled(c, rad,
			    ImGui::GetColorU32(ImVec4((float)r, (float)g,
						      (float)b, (float)a)), 12);
	ImGui::Dummy(ImVec2(rad * 2.0f + 4.0f, lh));
	return s7_unspecified(sc);
}

/* Selection id honouring the api / g_entity_sel fallback the World tab uses. */
static int32_t world_sel(void)
{
	return g_entity_api ? g_entity_api->get_selected() : g_entity_sel;
}

/*
 * (krudd-world-caps) -> (entity-api? asset-api?). The tab greys the create/edit
 * widgets when the scene api is absent and the binding combos when either the
 * scene or asset api is; these two booleans drive both.
 */
static s7_pointer sp_krudd_world_caps(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return s7_list(sc, 2,
		       s7_make_boolean(sc, g_entity_api != NULL),
		       s7_make_boolean(sc, g_asset_api != NULL));
}

/* (krudd-selected) -> the selected entity id, or -1 when nothing is selected. */
static s7_pointer sp_krudd_selected(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return s7_make_integer(sc, (s7_int)world_sel());
}

/*
 * (krudd-world-entities) -> a list of (id . name) for each live entity in id
 * order; name is a string or #f when the entity has no name. Returns #f when the
 * scene api or its world is absent, which the tab renders as "(no entities)".
 */
static s7_pointer sp_krudd_world_entities(s7_scheme *sc, s7_pointer args)
{
	const struct world *w = NULL;
	s7_pointer          out;
	uint32_t            i;

	(void)args;
	if (g_entity_api)
		w = g_entity_api->get_world();
	if (!w)
		return s7_f(sc);

	out = s7_nil(sc);
	for (i = 0; i < w->count; i++) {
		s7_pointer name;

		if (!w->alive[i])
			continue;
		if ((w->mask[i] & COMPONENT_NAME) &&
		    w->name_off[i] != SCENE_NO_NAME)
			name = s7_make_string(sc, w->names + w->name_off[i]);
		else
			name = s7_f(sc);
		out = s7_cons(sc,
			      s7_cons(sc, s7_make_integer(sc, (s7_int)i), name),
			      out);
	}
	return s7_reverse(sc, out);
}

/*
 * (krudd-entity-inspect id) -> the inspector bundle for a live entity, else #f:
 *   (name (px py pz) (rx ry rz rw) (sx sy sz) parent
 *    has-name? has-render? has-material? render-ref material-ref
 *    has-script? script-ref)
 * name is a string ("" when unnamed); parent is #f for a root or (pid . pname)
 * where pname is a string or #f; the refs are asset ids (0 = unbound).
 */
static s7_pointer sp_krudd_entity_inspect(s7_scheme *sc, s7_pointer args)
{
	const struct world     *w  = NULL;
	const struct transform *t;
	int32_t                 id = (int32_t)s7_integer(s7_car(args));
	uint32_t                e;
	s7_pointer              parent;
	const char             *name;

	if (g_entity_api)
		w = g_entity_api->get_world();
	if (!w || id < 0 || (uint32_t)id >= w->count ||
	    !w->alive[(uint32_t)id])
		return s7_f(sc);
	e = (uint32_t)id;
	t = &w->local[e];

	name = NULL;
	if ((w->mask[e] & COMPONENT_NAME) && w->name_off[e] != SCENE_NO_NAME)
		name = w->names + w->name_off[e];

	if (w->parent[e] < 0) {
		parent = s7_f(sc);
	} else {
		uint32_t    p  = (uint32_t)w->parent[e];
		const char *pn = NULL;

		if ((w->mask[p] & COMPONENT_NAME) &&
		    w->name_off[p] != SCENE_NO_NAME)
			pn = w->names + w->name_off[p];
		parent = s7_cons(sc, s7_make_integer(sc, (s7_int)p),
				 pn ? s7_make_string(sc, pn) : s7_f(sc));
	}

	return s7_list(sc, 12,
		s7_make_string(sc, name ? name : ""),
		real_list(sc, t->position, 3),
		real_list(sc, t->rotation, 4),
		real_list(sc, t->scale, 3),
		parent,
		s7_make_boolean(sc, (w->mask[e] & COMPONENT_NAME)     != 0),
		s7_make_boolean(sc, (w->mask[e] & COMPONENT_RENDER)   != 0),
		s7_make_boolean(sc, (w->mask[e] & COMPONENT_MATERIAL) != 0),
		s7_make_integer(sc, (s7_int)((w->mask[e] & COMPONENT_RENDER)
					     ? w->render_ref[e] : 0u)),
		s7_make_integer(sc, (s7_int)((w->mask[e] & COMPONENT_MATERIAL)
					     ? w->material_ref[e] : 0u)),
		s7_make_boolean(sc, (w->mask[e] & COMPONENT_SCRIPT)   != 0),
		s7_make_integer(sc, (s7_int)((w->mask[e] & COMPONENT_SCRIPT)
					     ? w->script_ref[e] : 0u)));
}

/* (id . path) rows for every catalog asset of the given ASSET_TYPE_*. */
static s7_pointer assets_of_type(s7_scheme *sc, int type)
{
	s7_pointer out = s7_nil(sc);
	uint32_t   k, n;

	if (!g_asset_api)
		return out;
	n = g_asset_api->count();
	for (k = 0; k < n; k++) {
		struct asset_info mi;

		if (g_asset_api->info(k, &mi) != 0 || mi.type != type ||
		    mi.id == 0)
			continue;
		out = s7_cons(sc,
			      s7_cons(sc, s7_make_integer(sc, (s7_int)mi.id),
				      s7_make_string(sc, mi.path)),
			      out);
	}
	return s7_reverse(sc, out);
}

/* (krudd-mesh-assets) -> ((id . path) ...) for the mesh-binding combo. */
static s7_pointer sp_krudd_mesh_assets(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return assets_of_type(sc, ASSET_TYPE_MESH);
}

/* (krudd-material-assets) -> ((id . path) ...) for the material-binding combo. */
static s7_pointer sp_krudd_material_assets(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return assets_of_type(sc, ASSET_TYPE_MATERIAL);
}

/* (krudd-shader-assets) -> ((id . path) ...) for the material's shader combo. */
static s7_pointer sp_krudd_shader_assets(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return assets_of_type(sc, ASSET_TYPE_SHADER);
}

/* (krudd-script-assets) -> ((id . path) ...) for the script-binding combo. */
static s7_pointer sp_krudd_script_assets(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return assets_of_type(sc, ASSET_TYPE_SCRIPT);
}

/* (krudd-texture-assets) -> ((id . path) ...) for a material's texture combo. */
static s7_pointer sp_krudd_texture_assets(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return assets_of_type(sc, ASSET_TYPE_TEXTURE);
}

/* (krudd-asset-find ref) -> the asset's path, or #f when ref is 0 / unknown. */
static s7_pointer sp_krudd_asset_find(s7_scheme *sc, s7_pointer args)
{
	uint32_t          ref = (uint32_t)s7_integer(s7_car(args));
	struct asset_info bi;

	if (ref == 0 || !g_asset_api || !g_asset_api->find ||
	    g_asset_api->find(ref, &bi) != 0)
		return s7_f(sc);
	return s7_make_string(sc, bi.path);
}

/*
 * (krudd-entity-create) -> the new entity id, or -1 on failure. Appends a root
 * entity with an identity transform at the origin, a box mesh, and the
 * default scene material — what the "+ Entity" button seeds; the caller
 * names and selects it. Undo is recorded by the scene api.
 */
static s7_pointer sp_krudd_entity_create(s7_scheme *sc, s7_pointer args)
{
	struct transform seed;
	int32_t          id = -1;

	(void)args;
	if (g_entity_api && g_entity_api->create_entity) {
		uint32_t box = asset_id_by_path("builtin://mesh/box");
		uint32_t material =
			asset_id_by_path("builtin://material/default");

		memset(&seed, 0, sizeof(seed));
		seed.rotation[3] = 1.0f;
		seed.scale[0] = seed.scale[1] = seed.scale[2] = 1.0f;
		id = g_entity_api->create_entity(WORLD_NO_PARENT, &seed, 0u,
						 box);
		if (id >= 0 && material && g_entity_api->set_material_ref)
			g_entity_api->set_material_ref(id, material);
	}
	return s7_make_integer(sc, (s7_int)id);
}

/* (krudd-entity-destroy id) -> unspecified. Tombstones the entity and subtree. */
static s7_pointer sp_krudd_entity_destroy(s7_scheme *sc, s7_pointer args)
{
	int32_t id = (int32_t)s7_integer(s7_car(args));

	if (g_entity_api && g_entity_api->destroy_entity)
		g_entity_api->destroy_entity(id);
	return s7_unspecified(sc);
}

/* (krudd-entity-select id) -> unspecified. Uses g_entity_sel without the api. */
static s7_pointer sp_krudd_entity_select(s7_scheme *sc, s7_pointer args)
{
	int32_t id = (int32_t)s7_integer(s7_car(args));

	if (g_entity_api)
		g_entity_api->set_selected(id);
	else
		g_entity_sel = id;
	return s7_unspecified(sc);
}

/* (krudd-entity-set-name id str) -> unspecified. Empty str clears the name. */
static s7_pointer sp_krudd_entity_set_name(s7_scheme *sc, s7_pointer args)
{
	int32_t    id = (int32_t)s7_integer(s7_car(args));
	s7_pointer nm = s7_cadr(args);

	if (g_entity_api && g_entity_api->set_name && s7_is_string(nm))
		g_entity_api->set_name(id, s7_string(nm));
	return s7_unspecified(sc);
}

/* (krudd-entity-set-transform id (px py pz) (rx ry rz rw) (sx sy sz)). */
static s7_pointer sp_krudd_entity_set_transform(s7_scheme *sc, s7_pointer args)
{
	s7_pointer       p  = args;
	int32_t          id = (int32_t)s7_integer(s7_car(p)); p = s7_cdr(p);
	struct transform nt;

	read_reals(sc, s7_car(p), nt.position, 3); p = s7_cdr(p);
	read_reals(sc, s7_car(p), nt.rotation, 4); p = s7_cdr(p);
	read_reals(sc, s7_car(p), nt.scale, 3);
	if (g_entity_api && g_entity_api->set_transform)
		g_entity_api->set_transform(id, &nt);
	return s7_unspecified(sc);
}

/* (krudd-entity-set-render-ref id ref) -> unspecified. ref 0 unbinds the mesh. */
static s7_pointer sp_krudd_entity_set_render_ref(s7_scheme *sc, s7_pointer args)
{
	int32_t  id  = (int32_t)s7_integer(s7_car(args));
	uint32_t ref = (uint32_t)s7_integer(s7_cadr(args));

	if (g_entity_api && g_entity_api->set_render_ref)
		g_entity_api->set_render_ref(id, ref);
	return s7_unspecified(sc);
}

/* (krudd-entity-set-material-ref id ref) -> unspecified. ref 0 unbinds it. */
static s7_pointer sp_krudd_entity_set_material_ref(s7_scheme *sc, s7_pointer args)
{
	int32_t  id  = (int32_t)s7_integer(s7_car(args));
	uint32_t ref = (uint32_t)s7_integer(s7_cadr(args));

	if (g_entity_api && g_entity_api->set_material_ref)
		g_entity_api->set_material_ref(id, ref);
	return s7_unspecified(sc);
}

/* (krudd-entity-set-script-ref id ref) -> unspecified. ref 0 unbinds it. */
static s7_pointer sp_krudd_entity_set_script_ref(s7_scheme *sc, s7_pointer args)
{
	int32_t  id  = (int32_t)s7_integer(s7_car(args));
	uint32_t ref = (uint32_t)s7_integer(s7_cadr(args));

	if (g_entity_api && g_entity_api->set_script_ref)
		g_entity_api->set_script_ref(id, ref);
	return s7_unspecified(sc);
}

/* (krudd-gizmo-mode) -> the shared gizmo tool: 0 move, 1 rotate, 2 scale. */
static s7_pointer sp_krudd_gizmo_mode(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return s7_make_integer(sc, (s7_int)g_gizmo_mode);
}

/* (krudd-set-gizmo-mode m) -> unspecified. Out-of-range m is ignored. */
static s7_pointer sp_krudd_set_gizmo_mode(s7_scheme *sc, s7_pointer args)
{
	int m = (int)s7_integer(s7_car(args));

	if (m >= GIZMO_MOVE && m <= GIZMO_SCALE)
		g_gizmo_mode = (enum gizmo_mode)m;
	return s7_unspecified(sc);
}

/* ------------------------------------------------------------------ */
/* Assets-tab primitives (#402)                                        */
/* ------------------------------------------------------------------ */

/* Buffer cap for the multiline text-edit and asset-data primitives below;
 * matches the pre-port g_edit static buffer size. */
#define ASSETS_EDIT_MAX (64 * 1024)

/*
 * Shader authoring metadata.  A krudd shader is a single DSL source that
 * embeds every stage it defines (see shader.scm) — there is no per-asset
 * stage or dialect to pick, so the editor has nothing to ask for beyond a
 * name; the source itself is the only thing to author.
 */
#define SHADER_FORMAT "krudd-shader"

/*
 * Script authoring metadata.  A krudd script is a single (script NAME ...) form
 * carrying its lifecycle hooks (see core/entity_script.scm) — like a shader,
 * the source itself is the only thing to author, so the editor asks only for a
 * name and derives everything else from the text.
 */
#define SCRIPT_FORMAT "krudd-script"

/*
 * Mesh authoring metadata. A krudd mesh is a single
 * (mesh NAME (generate () ...)) form carrying its generator (see
 * core/mesh_script.scm) — there is no other kind of mesh asset, so the
 * source itself is the only thing to author, the same as a script.
 */
#define MESH_FORMAT "krudd-mesh"

/*
 * Report the stage blocks SRC defines, the same way the built-in shaders
 * advertise theirs via describe() — a comma-joined "vertex, fragment" list
 * in declaration order.  Used both to display the (derived, read-only)
 * declaration and to publish it back on Save.
 */
static const char *shader_stages_from_source(const char *src)
{
	static char buf[32];
	int         has_vertex   = src && strstr(src, "(vertex")   != NULL;
	int         has_fragment = src && strstr(src, "(fragment") != NULL;

	if (has_vertex && has_fragment)
		snprintf(buf, sizeof(buf), "vertex, fragment");
	else if (has_vertex)
		snprintf(buf, sizeof(buf), "vertex");
	else if (has_fragment)
		snprintf(buf, sizeof(buf), "fragment");
	else
		buf[0] = '\0';
	return buf;
}

/*
 * Try to transpile every stage SRC declares. A declared stage that fails to
 * transpile — or no declared stage at all — means the shader can't bind, so
 * treat it as a compile failure rather than committing broken source.
 */
static bool shader_compiles(const char *src)
{
	int has_vertex   = src && strstr(src, "(vertex")   != NULL;
	int has_fragment = src && strstr(src, "(fragment") != NULL;

	if (!has_vertex && !has_fragment)
		return false;
	if (has_vertex && !script_shader_transpile(src, "vertex"))
		return false;
	if (has_fragment && !script_shader_transpile(src, "fragment"))
		return false;
	return true;
}

/*
 * The engine's built-in scene shader — always present, read-only — used to
 * seed new shader assets so they start from working source instead of blank.
 */
static const char *default_shader_src(void)
{
	uint32_t          n, i;
	struct asset_info info;

	if (!g_asset_api)
		return "";
	n = g_asset_api->count();
	for (i = 0; i < n; i++) {
		if (g_asset_api->info(i, &info) != 0 || !info.read_only ||
		    info.type != ASSET_TYPE_SHADER)
			continue;
		if (strcmp(info.path, "builtin://shader/scene") == 0) {
			const void *data = g_asset_api->get_data(info.id, NULL);
			return data ? (const char *)data : "";
		}
	}
	return "";
}

/*
 * Report the lifecycle hooks SRC defines, the same comma-joined form the
 * built-in scripts advertise via describe() ("on-begin, on-tick"), in the
 * canonical begin/tick/destroy order.  Used both to display the (derived,
 * read-only) declaration and to publish it back on Save.  The buffer holds the
 * longest possible list ("on-begin, on-tick, on-destroy") with room to spare.
 */
static const char *script_hooks_from_source(const char *src)
{
	static char buf[64];
	int         has_begin   = src && strstr(src, "(on-begin")   != NULL;
	int         has_tick    = src && strstr(src, "(on-tick")    != NULL;
	int         has_destroy = src && strstr(src, "(on-destroy") != NULL;
	int         n           = 0;

	buf[0] = '\0';
	if (has_begin) {
		strcpy(buf, "on-begin");
		n++;
	}
	if (has_tick) {
		if (n)
			strcat(buf, ", ");
		strcat(buf, "on-tick");
		n++;
	}
	if (has_destroy) {
		if (n)
			strcat(buf, ", ");
		strcat(buf, "on-destroy");
	}
	return buf;
}

/*
 * A script is well-formed enough to commit when it is a (script ...) form that
 * declares at least one lifecycle hook — the script analogue of the shader
 * save's compile gate, kept to a cheap substring test (matching shader_stages_
 * from_source's style) so a Save never has to eval untrusted source.  Blank or
 * hookless text is rejected, so a broken edit never reaches the live asset.
 */
static bool script_form_ok(const char *src)
{
	if (!src || !strstr(src, "(script"))
		return false;
	return strstr(src, "(on-begin") != NULL
	    || strstr(src, "(on-tick") != NULL
	    || strstr(src, "(on-destroy") != NULL;
}

/*
 * The engine's built-in spinner script — always present, read-only — used to
 * seed new script assets so they start from a working (script ...) form
 * instead of blank, the way default_shader_src seeds new shaders.
 */
static const char *default_script_src(void)
{
	uint32_t          n, i;
	struct asset_info info;

	if (!g_asset_api)
		return "";
	n = g_asset_api->count();
	for (i = 0; i < n; i++) {
		if (g_asset_api->info(i, &info) != 0 || !info.read_only ||
		    info.type != ASSET_TYPE_SCRIPT)
			continue;
		if (strcmp(info.path, "builtin://script/spinner") == 0) {
			const void *data = g_asset_api->get_data(info.id, NULL);
			return data ? (const char *)data : "";
		}
	}
	return "";
}

/*
 * A mesh is well-formed enough to commit when it is a (mesh ...) form
 * declaring a (generate ...) clause — the mesh analogue of script_form_ok's
 * cheap substring gate, so a Save never has to eval untrusted source. Blank
 * or generate-less text is rejected, so a broken edit never reaches the live
 * asset.
 */
static bool mesh_form_ok(const char *src)
{
	if (!src || !strstr(src, "(mesh"))
		return false;
	return strstr(src, "(generate") != NULL;
}

/*
 * The engine's built-in grid mesh — always present, read-only — used to seed
 * new mesh assets so they start from working source instead of blank, the
 * way default_script_src seeds new entity scripts.
 */
static const char *default_mesh_src(void)
{
	uint32_t          n, i;
	struct asset_info info;

	if (!g_asset_api)
		return "";
	n = g_asset_api->count();
	for (i = 0; i < n; i++) {
		if (g_asset_api->info(i, &info) != 0 || !info.read_only ||
		    info.type != ASSET_TYPE_MESH)
			continue;
		if (strcmp(info.path, "builtin://mesh/grid") == 0) {
			const void *data = g_asset_api->get_data(info.id, NULL);
			return data ? (const char *)data : "";
		}
	}
	return "";
}

/*
 * Persist an authored asset's current bytes through the backend, if the
 * backend is present and capable — the same "write it live even without
 * persistence" fallback every Assets save button used pre-port (#390).
 * Looks the path up fresh via find() so every mutation primitive below can
 * share this instead of threading an asset_info through each call.
 */
static void maybe_persist_asset(uint32_t id, int32_t type, const void *bytes,
				uint32_t size)
{
	struct asset_info info;

	if (!g_backend || !(g_backend->get_caps() & BACKEND_CAP_PROJECT_PERSIST))
		return;
	if (!g_asset_api || g_asset_api->find(id, &info) != 0)
		return;
	g_backend->persist_asset(id, info.path, type, bytes, size);
}

/* (imgui-button label) -> #t when clicked this frame, else #f. */
static s7_pointer sp_imgui_button(s7_scheme *sc, s7_pointer args)
{
	s7_pointer label = s7_car(args);
	bool       hit   = false;

	if (s7_is_string(label))
		hit = ImGui::Button(s7_string(label));
	return s7_make_boolean(sc, hit);
}

/*
 * (imgui-input-text-enter id text) -> (text . entered?). Like imgui-input-
 * text, but entered? is #t the instant Enter is pressed inside the field
 * (ImGuiInputTextFlags_EnterReturnsTrue) rather than on focus loss — the New
 * Asset name field and the shader clone-name field both confirm on Enter.
 */
static s7_pointer sp_imgui_input_text_enter(s7_scheme *sc, s7_pointer args)
{
	static char buf[256];
	s7_pointer  id      = s7_car(args);
	s7_pointer  text    = s7_cadr(args);
	bool        entered = false;

	buf[0] = '\0';
	if (s7_is_string(text)) {
		strncpy(buf, s7_string(text), sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = '\0';
	}
	if (s7_is_string(id))
		entered = ImGui::InputText(s7_string(id), buf, sizeof(buf),
					   ImGuiInputTextFlags_EnterReturnsTrue);
	return s7_cons(sc, s7_make_string(sc, buf), s7_make_boolean(sc, entered));
}

/*
 * (imgui-input-text-multiline id text h readonly?) -> (text . changed?).
 * changed? is #t on any edit this frame (InputTextMultiline's own return),
 * not just on commit — the markdown/shader source boxes reparse or redraw
 * live as the source changes, unlike the single-line name field.
 */
static s7_pointer sp_imgui_input_text_multiline(s7_scheme *sc, s7_pointer args)
{
	static char buf[ASSETS_EDIT_MAX];
	s7_pointer  p        = args;
	s7_pointer  id       = s7_car(p); p = s7_cdr(p);
	s7_pointer  text     = s7_car(p); p = s7_cdr(p);
	float       h        = (float)s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	bool        readonly = s7_boolean(sc, s7_car(p));
	bool        changed  = false;

	buf[0] = '\0';
	if (s7_is_string(text)) {
		strncpy(buf, s7_string(text), sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = '\0';
	}
	if (s7_is_string(id))
		changed = ImGui::InputTextMultiline(
			s7_string(id), buf, sizeof(buf), ImVec2(-1.0f, h),
			readonly ? ImGuiInputTextFlags_ReadOnly
				 : ImGuiInputTextFlags_None);
	return s7_cons(sc, s7_make_string(sc, buf), s7_make_boolean(sc, changed));
}

/*
 * (imgui-combo id items current) -> the (possibly new) selected index. items
 * is a list of label strings; the New Asset type picker is the only caller,
 * capped at 8 entries (it uses 3).
 */
static s7_pointer sp_imgui_combo(s7_scheme *sc, s7_pointer args)
{
	s7_pointer  id    = s7_car(args);
	s7_pointer  items = s7_cadr(args);
	int         cur   = (int)s7_integer(s7_caddr(args));
	const char *labels[8];
	int         n     = 0;
	s7_pointer  p;

	for (p = items; s7_is_pair(p) && n < 8; p = s7_cdr(p))
		labels[n++] = s7_string(s7_car(p));
	if (s7_is_string(id))
		ImGui::Combo(s7_string(id), &cur, labels, n);
	return s7_make_integer(sc, cur);
}

/* (imgui-color-edit4 id rgba) -> (rgba . changed?). */
static s7_pointer sp_imgui_color_edit4(s7_scheme *sc, s7_pointer args)
{
	s7_pointer id      = s7_car(args);
	float      v[4];
	bool       changed = false;

	read_reals(sc, s7_cadr(args), v, 4);
	if (s7_is_string(id))
		changed = ImGui::ColorEdit4(s7_string(id), v);
	return s7_cons(sc, real_list(sc, v, 4), s7_make_boolean(sc, changed));
}

/* (imgui-color-edit3 id (r g b)) -> ((r g b) . changed?). */
static s7_pointer sp_imgui_color_edit3(s7_scheme *sc, s7_pointer args)
{
	s7_pointer id      = s7_car(args);
	float      v[3];
	bool       changed = false;

	read_reals(sc, s7_cadr(args), v, 3);
	if (s7_is_string(id))
		changed = ImGui::ColorEdit3(s7_string(id), v);
	return s7_cons(sc, real_list(sc, v, 3), s7_make_boolean(sc, changed));
}

/* (imgui-input-float id x) -> (x . changed?). */
static s7_pointer sp_imgui_input_float(s7_scheme *sc, s7_pointer args)
{
	s7_pointer id      = s7_car(args);
	float      v       = (float)s7_number_to_real(sc, s7_cadr(args));
	bool       changed = false;

	if (s7_is_string(id))
		changed = ImGui::InputFloat(s7_string(id), &v);
	return s7_cons(sc, s7_make_real(sc, v), s7_make_boolean(sc, changed));
}

/* (imgui-input-float2 id (x y)) -> ((x y) . changed?). */
static s7_pointer sp_imgui_input_float2(s7_scheme *sc, s7_pointer args)
{
	s7_pointer id      = s7_car(args);
	float      v[2];
	bool       changed = false;

	read_reals(sc, s7_cadr(args), v, 2);
	if (s7_is_string(id))
		changed = ImGui::InputFloat2(s7_string(id), v);
	return s7_cons(sc, real_list(sc, v, 2), s7_make_boolean(sc, changed));
}

/* (imgui-slider-float id x lo hi) -> (x . changed?). */
static s7_pointer sp_imgui_slider_float(s7_scheme *sc, s7_pointer args)
{
	s7_pointer p       = args;
	s7_pointer id      = s7_car(p); p = s7_cdr(p);
	float      v       = (float)s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	float      lo      = (float)s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	float      hi      = (float)s7_number_to_real(sc, s7_car(p));
	bool       changed = false;

	if (s7_is_string(id))
		changed = ImGui::SliderFloat(s7_string(id), &v, lo, hi);
	return s7_cons(sc, s7_make_real(sc, v), s7_make_boolean(sc, changed));
}

/*
 * (imgui-mesh-drag-source id label) -> unspecified. A mesh asset row is a
 * drag source the whole frame it's drawn; the payload is the raw asset id,
 * which draw_spawn_drop_target's viewport-wide target reads back to spawn
 * an entity bound to that mesh (#176). id must fit a uint32_t.
 */
static s7_pointer sp_imgui_mesh_drag_source(s7_scheme *sc, s7_pointer args)
{
	uint32_t   id    = (uint32_t)s7_integer(s7_car(args));
	s7_pointer label = s7_cadr(args);

	if (s7_is_string(label) &&
	    ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
		ImGui::SetDragDropPayload("ASSET_ID", &id, sizeof(id));
		ImGui::Text("Spawn %s", s7_string(label));
		ImGui::EndDragDropSource();
	}
	return s7_unspecified(sc);
}

/* One (id path type kind state size refs) row for the asset browser. */
static s7_pointer asset_row(s7_scheme *sc, const struct asset_info *info)
{
	return s7_list(sc, 7,
		s7_make_integer(sc, (s7_int)info->id),
		s7_make_string(sc, info->path),
		s7_make_integer(sc, info->type),
		s7_make_integer(sc, info->kind),
		s7_make_integer(sc, info->state),
		s7_make_integer(sc, (s7_int)info->size),
		s7_make_integer(sc, info->refs));
}

/*
 * (krudd-assets) -> (builtin-rows project-rows), each a list of (id path
 * type kind state size refs) rows in catalog order, or #f when the asset
 * api is absent. Split by read_only here so the browser table's two labeled
 * groups need no filtering in Scheme.
 */
static s7_pointer sp_krudd_assets(s7_scheme *sc, s7_pointer args)
{
	s7_pointer builtin = s7_nil(sc);
	s7_pointer project = s7_nil(sc);
	uint32_t   n, i;

	(void)args;
	if (!g_asset_api)
		return s7_f(sc);
	n = g_asset_api->count();
	for (i = 0; i < n; i++) {
		struct asset_info info;

		if (g_asset_api->info(i, &info) != 0)
			continue;
		if (info.read_only)
			builtin = s7_cons(sc, asset_row(sc, &info), builtin);
		else
			project = s7_cons(sc, asset_row(sc, &info), project);
	}
	return s7_list(sc, 2, s7_reverse(sc, builtin), s7_reverse(sc, project));
}

/* (krudd-asset-mut?) -> #t when the mutation api is present (editor build). */
static s7_pointer sp_krudd_asset_mut(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return s7_make_boolean(sc, g_asset_mut != NULL);
}

/*
 * (krudd-asset-info id) -> (path type kind state size refs read-only?
 * origin), or #f when id is unknown.
 */
static s7_pointer sp_krudd_asset_info(s7_scheme *sc, s7_pointer args)
{
	uint32_t          id = (uint32_t)s7_integer(s7_car(args));
	struct asset_info info;

	if (!g_asset_api || g_asset_api->find(id, &info) != 0)
		return s7_f(sc);
	return s7_list(sc, 8,
		s7_make_string(sc, info.path),
		s7_make_integer(sc, info.type),
		s7_make_integer(sc, info.kind),
		s7_make_integer(sc, info.state),
		s7_make_integer(sc, (s7_int)info.size),
		s7_make_integer(sc, info.refs),
		s7_make_boolean(sc, info.read_only),
		s7_make_integer(sc, info.origin));
}

/*
 * (krudd-asset-describe id) -> ((key . value) ...), the declaration fields
 * describe() reports for id, or '() when id is unknown or has none. describe
 * is index-addressed, so this scans the catalog once to find id's index.
 */
static s7_pointer sp_krudd_asset_describe(s7_scheme *sc, s7_pointer args)
{
	uint32_t                 id = (uint32_t)s7_integer(s7_car(args));
	struct asset_decl_field  fields[16];
	struct asset_info        tmp;
	s7_pointer               out = s7_nil(sc);
	uint32_t                 n, i, idx, nf;

	if (!g_asset_api)
		return out;
	n   = g_asset_api->count();
	idx = n;
	for (i = 0; i < n; i++) {
		if (g_asset_api->info(i, &tmp) == 0 && tmp.id == id) {
			idx = i;
			break;
		}
	}
	if (idx >= n)
		return out;
	nf = g_asset_api->describe(idx, fields, 16);
	for (i = nf; i > 0; i--)
		out = s7_cons(sc,
			s7_cons(sc, s7_make_string(sc, fields[i - 1].key),
				s7_make_string(sc, fields[i - 1].value)),
			out);
	return out;
}

/*
 * (krudd-asset-data id) -> the asset's bytes as a string, clamped to
 * ASSETS_EDIT_MAX - 1 and NUL-terminated; "" when id is unknown or has no
 * data. Used to (re)load the text/shader source edit buffer on selection.
 */
static s7_pointer sp_krudd_asset_data(s7_scheme *sc, s7_pointer args)
{
	static char buf[ASSETS_EDIT_MAX];
	uint32_t    id = (uint32_t)s7_integer(s7_car(args));
	const void *src;
	uint32_t    sz = 0;

	buf[0] = '\0';
	if (g_asset_api && g_asset_api->get_data &&
	    (src = g_asset_api->get_data(id, &sz)) != NULL) {
		if (sz >= (uint32_t)ASSETS_EDIT_MAX)
			sz = (uint32_t)ASSETS_EDIT_MAX - 1;
		memcpy(buf, src, (size_t)sz);
		buf[sz] = '\0';
	}
	return s7_make_string(sc, buf);
}

/*
 * A material's wire form is a leading uint32 shader-ref (asset id — a material
 * always names its shader) followed by the shader's std140 Material block. The
 * schema (which params, their layout) lives entirely in the shader; these
 * helpers introspect it (via the runtime image's shader-material-params) and
 * pack/unpack a material's values against it.
 */
#define MATERIAL_MAX_PARAMS 32
#define MATERIAL_HEADER_BYTES ((uint32_t)sizeof(uint32_t))
#define MATERIAL_WIRE_CAP (MATERIAL_HEADER_BYTES + 256u)

/*
 * Borrow a shader asset's source as a NUL-terminated C string in a static
 * buffer. Authored shader bytes are stored without a trailing NUL (set_data
 * takes strlen()), so copy-and-terminate rather than handing get_data() straight
 * to the parser. Single static buffer — each caller consumes it before the next.
 */
static const char *shader_src_cstr(uint32_t shader_ref)
{
	static char buf[8192];
	const void *d;
	uint32_t    sz = 0;

	if (!g_asset_api || !g_asset_api->get_data)
		return NULL;
	d = g_asset_api->get_data(shader_ref, &sz);
	if (!d)
		return NULL;
	if (sz >= sizeof(buf))
		sz = sizeof(buf) - 1;
	memcpy(buf, d, sz);
	buf[sz] = '\0';
	return buf;
}

/* The default value of editable component k: an authored (default V ...) if the
 * field declares one, else the value implied by its edit hint. */
static float param_default(const struct shader_param *p, uint32_t k)
{
	if (k < p->default_count)
		return p->edit_default[k];
	if (strcmp(p->edit, "color") == 0)
		return 1.0f;                 /* opaque white / full */
	if (strcmp(p->edit, "range") == 0)
		return p->edit_min;
	return 0.0f;
}

/* Resolve a catalog path to its stable asset id, or 0 when absent. */
static uint32_t asset_id_by_path(const char *path)
{
	uint32_t i, n;

	if (!g_asset_api)
		return 0;
	n = g_asset_api->count();
	for (i = 0; i < n; i++) {
		struct asset_info info;

		if (g_asset_api->info(i, &info) == 0 &&
		    strcmp(info.path, path) == 0)
			return info.id;
	}
	return 0;
}

/*
 * Pack a material's wire bytes: the shader-ref header then each field's floats
 * at its std140 offset, per the shader's layout. Every field is written — from
 * field_values (a Scheme list of per-field component lists) where supplied, else
 * from its edit-hint default — so a material created with no values still lands
 * on sensible defaults (white for a color) rather than zeros. Returns the total
 * byte length written into out (capped at cap).
 */
static uint32_t pack_material(s7_scheme *sc, uint32_t shader_ref,
			      const char *src, s7_pointer field_values,
			      unsigned char *out, uint32_t cap)
{
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint32_t            total = 0, len;
	int                 n, i;
	s7_pointer          fv = field_values;

	n = script_shader_material_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (n < 0)
		n = 0;
	len = MATERIAL_HEADER_BYTES + total;
	if (len > cap)
		len = cap;
	memset(out, 0, len);
	memcpy(out, &shader_ref, sizeof(shader_ref));

	for (i = 0; i < n; i++) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		uint32_t off = MATERIAL_HEADER_BYTES + p[i].offset;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (s7_is_pair(fv)) {
			read_reals(sc, s7_car(fv), v, (int)c);
			fv = s7_cdr(fv);
		}
		if (off + c * sizeof(float) <= len)
			memcpy(out + off, v, c * sizeof(float));
	}
	return len;
}

/* The std140 byte size of a shader's Material block (0 when it has none). */
static uint32_t mat_block_len(uint32_t shader_ref)
{
	const char *src   = shader_src_cstr(shader_ref);
	uint32_t    total = 0;

	if (src)
		script_shader_material_params(src, NULL, 0, &total);
	return total;
}

/*
 * Locate a material's optional texture slot — the trailer after the shader-ref
 * and the shader's Material block: [tex-ref u32][width u32][height u32]. Returns
 * 1 and fills the outs when present, 0 otherwise. The block size comes from the
 * material's own shader (its leading ref), the same split the renderer makes, so
 * a material with no trailer reports no texture.
 */
static int mat_texture_slot(uint32_t id, uint32_t *tex, uint32_t *w, uint32_t *h)
{
	const uint8_t *bytes;
	uint32_t       sz = 0, shader_ref = 0, block, off;

	if (!g_asset_api || !g_asset_api->get_data)
		return 0;
	bytes = (const uint8_t *)g_asset_api->get_data(id, &sz);
	if (!bytes || sz < MATERIAL_HEADER_BYTES)
		return 0;
	memcpy(&shader_ref, bytes, sizeof(shader_ref));
	block = mat_block_len(shader_ref);
	if (block == 0)
		return 0;
	off = MATERIAL_HEADER_BYTES + block;
	if (sz < off + 3u * sizeof(uint32_t))
		return 0;
	memcpy(tex, bytes + off,      sizeof(uint32_t));
	memcpy(w,   bytes + off + 4u, sizeof(uint32_t));
	memcpy(h,   bytes + off + 8u, sizeof(uint32_t));
	return *tex != 0;
}

/*
 * (krudd-material-texture id) -> (tex-ref width height) when the material binds a
 * texture, else '(). The material editor reads this to seed its texture picker
 * and resolution control.
 */
static s7_pointer sp_krudd_material_texture(s7_scheme *sc, s7_pointer args)
{
	uint32_t id = (uint32_t)s7_integer(s7_car(args));
	uint32_t tex = 0, w = 0, h = 0;

	if (!mat_texture_slot(id, &tex, &w, &h))
		return s7_nil(sc);
	return s7_list(sc, 3, s7_make_integer(sc, (s7_int)tex),
		       s7_make_integer(sc, (s7_int)w),
		       s7_make_integer(sc, (s7_int)h));
}

/*
 * (krudd-shader-material-params shader-ref) -> ((name type off size components
 * edit-kind edit-min edit-max) ...), the shader's Material block as editable
 * parameters. Empty when the shader is gone or declares no Material block.
 */
static s7_pointer sp_krudd_shader_material_params(s7_scheme *sc, s7_pointer args)
{
	uint32_t            shader_ref = (uint32_t)s7_integer(s7_car(args));
	const char         *src        = shader_src_cstr(shader_ref);
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint32_t            total = 0;
	int                 n, i;
	s7_pointer          out = s7_nil(sc);

	if (!src)
		return out;
	n = script_shader_material_params(src, p, MATERIAL_MAX_PARAMS, &total);
	for (i = n - 1; i >= 0; i--)
		out = s7_cons(sc,
			s7_list(sc, 8,
				s7_make_string(sc, p[i].name),
				s7_make_string(sc, p[i].type),
				s7_make_integer(sc, (s7_int)p[i].offset),
				s7_make_integer(sc, (s7_int)p[i].size),
				s7_make_integer(sc, (s7_int)p[i].components),
				s7_make_string(sc, p[i].edit),
				s7_make_real(sc, p[i].edit_min),
				s7_make_real(sc, p[i].edit_max)),
			out);
	return out;
}

/*
 * (krudd-asset-shader-ref id) -> the material's shader asset id (its leading
 * uint32), or 0 when it has none. The editor reads this to seed the shader
 * picker with the material's current shader.
 */
static s7_pointer sp_krudd_asset_shader_ref(s7_scheme *sc, s7_pointer args)
{
	uint32_t     id  = (uint32_t)s7_integer(s7_car(args));
	uint32_t     ref = 0;
	const void  *src;
	uint32_t     sz  = 0;

	if (g_asset_api && g_asset_api->get_data &&
	    (src = g_asset_api->get_data(id, &sz)) != NULL &&
	    sz >= MATERIAL_HEADER_BYTES)
		memcpy(&ref, src, sizeof(ref));
	return s7_make_integer(sc, (s7_int)ref);
}

/*
 * (krudd-material-values material-id shader-ref) -> a per-field list of
 * component lists ((v0 v1 ..) ...), the material's current parameter values
 * laid out for shader-ref. Values are read from the material's packed bytes only
 * when it already targets this shader (its leading shader-ref matches); switching
 * to a different shader yields that shader's per-hint defaults instead.
 */
static s7_pointer sp_krudd_material_values(s7_scheme *sc, s7_pointer args)
{
	uint32_t             mat_id     = (uint32_t)s7_integer(s7_car(args));
	uint32_t             shader_ref = (uint32_t)s7_integer(s7_cadr(args));
	const char          *src        = shader_src_cstr(shader_ref);
	struct shader_param  p[MATERIAL_MAX_PARAMS];
	uint32_t             total = 0, msz = 0, stored = 0;
	const unsigned char *mb = NULL;
	int                  n, i, use_stored = 0;
	s7_pointer           out = s7_nil(sc);

	if (!src)
		return out;
	n = script_shader_material_params(src, p, MATERIAL_MAX_PARAMS, &total);

	if (g_asset_api && g_asset_api->get_data)
		mb = (const unsigned char *)g_asset_api->get_data(mat_id, &msz);
	if (mb && msz >= MATERIAL_HEADER_BYTES) {
		memcpy(&stored, mb, sizeof(stored));
		use_stored = (stored == shader_ref);
	}

	for (i = n - 1; i >= 0; i--) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		uint32_t off = MATERIAL_HEADER_BYTES + p[i].offset;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (use_stored && off + c * sizeof(float) <= msz)
			memcpy(v, mb + off, c * sizeof(float));
		out = s7_cons(sc, real_list(sc, v, (int)c), out);
	}
	return out;
}

/*
 * Entity script parameters — the CPU-side twin of the shader/material helpers
 * above. A script's params clause is its schema (introspected tight-packed, no
 * std140 header); an entity's override blob holds the values. These three feed
 * the same widget dispatcher the material editor uses, keyed on the entity.
 */

/*
 * (krudd-script-params script-ref) -> ((name type off size components edit-kind
 * edit-min edit-max) ...), the bound script's params clause as editable
 * parameters. Empty when the asset is gone or declares no params.
 */
static s7_pointer sp_krudd_script_params(s7_scheme *sc, s7_pointer args)
{
	uint32_t            script_ref = (uint32_t)s7_integer(s7_car(args));
	const char         *src        = shader_src_cstr(script_ref);
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint32_t            total = 0;
	int                 n, i;
	s7_pointer          out = s7_nil(sc);

	if (!src)
		return out;
	n = script_entity_params(src, p, MATERIAL_MAX_PARAMS, &total);
	for (i = n - 1; i >= 0; i--)
		out = s7_cons(sc,
			s7_list(sc, 8,
				s7_make_string(sc, p[i].name),
				s7_make_string(sc, p[i].type),
				s7_make_integer(sc, (s7_int)p[i].offset),
				s7_make_integer(sc, (s7_int)p[i].size),
				s7_make_integer(sc, (s7_int)p[i].components),
				s7_make_string(sc, p[i].edit),
				s7_make_real(sc, p[i].edit_min),
				s7_make_real(sc, p[i].edit_max)),
			out);
	return out;
}

/*
 * (krudd-entity-script-values entity-id script-ref) -> a per-field list of
 * component lists, entity-id's current values for script-ref's params: read from
 * its override blob where present, else the param's edit-hint default. This is
 * the override ⊕ defaults the entity menu edits (and the script sees).
 */
static s7_pointer sp_krudd_entity_script_values(s7_scheme *sc, s7_pointer args)
{
	int32_t              eid        = (int32_t)s7_integer(s7_car(args));
	uint32_t             script_ref = (uint32_t)s7_integer(s7_cadr(args));
	const char          *src        = shader_src_cstr(script_ref);
	struct shader_param  p[MATERIAL_MAX_PARAMS];
	uint32_t             total = 0, blen = 0;
	const uint8_t       *blob = NULL;
	const struct world  *w    = NULL;
	int                  n, i;
	s7_pointer           out = s7_nil(sc);

	if (!src)
		return out;
	n = script_entity_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (g_entity_api && g_entity_api->get_world)
		w = g_entity_api->get_world();
	if (w && eid >= 0 && (uint32_t)eid < w->count)
		blob = world_script_params(w, (uint32_t)eid, &blen);

	for (i = n - 1; i >= 0; i--) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (blob && p[i].offset + c * sizeof(float) <= blen)
			memcpy(v, blob + p[i].offset, c * sizeof(float));
		out = s7_cons(sc, real_list(sc, v, (int)c), out);
	}
	return out;
}

/*
 * (krudd-entity-save-script-params entity-id script-ref field-values) ->
 * unspecified. Packs the per-field values into script-ref's tight params layout
 * and stores them as entity-id's override (through the scene api, so it records
 * an undo step). field-values is a list of per-field component lists.
 */
static s7_pointer sp_krudd_entity_save_script_params(s7_scheme *sc,
						     s7_pointer args)
{
	s7_pointer          a          = args;
	int32_t             eid        = (int32_t)s7_integer(s7_car(a));
	uint32_t            script_ref;
	s7_pointer          values;
	const char         *src;
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint8_t             bytes[WORLD_SCRIPT_PARAM_CAP];
	uint32_t            total = 0, len;
	int                 n, i;
	s7_pointer          fv;

	a          = s7_cdr(a);
	script_ref = (uint32_t)s7_integer(s7_car(a));
	a          = s7_cdr(a);
	values     = s7_car(a);
	fv         = values;
	src        = shader_src_cstr(script_ref);
	if (!src)
		return s7_unspecified(sc);

	n = script_entity_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (n < 0)
		n = 0;
	len = total > sizeof(bytes) ? (uint32_t)sizeof(bytes) : total;
	memset(bytes, 0, len);

	for (i = 0; i < n; i++) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		uint32_t off = p[i].offset;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (s7_is_pair(fv)) {
			read_reals(sc, s7_car(fv), v, (int)c);
			fv = s7_cdr(fv);
		}
		if (off + c * sizeof(float) <= len)
			memcpy(bytes + off, v, c * sizeof(float));
	}
	if (g_entity_api && g_entity_api->set_script_params)
		g_entity_api->set_script_params(eid, bytes, len);
	return s7_unspecified(sc);
}

/*
 * (krudd-entity-material-values entity-id material-id shader-ref) -> a per-field
 * list of component lists, entity-id's current values for the material's params:
 * its per-entity override where present, else the shared material asset's stored
 * values, else the shader's per-hint defaults. This is the override ⊕ material ⊕
 * defaults the entity inspector edits (and the renderer draws with). Mirrors
 * krudd-material-values, but overlaid with the entity's override so the swatch
 * shows what this one entity draws, not what the shared material stores.
 */
static s7_pointer sp_krudd_entity_material_values(s7_scheme *sc, s7_pointer args)
{
	s7_pointer           a          = args;
	int32_t              eid        = (int32_t)s7_integer(s7_car(a));
	uint32_t             mat_id;
	uint32_t             shader_ref;
	const char          *src;
	struct shader_param  p[MATERIAL_MAX_PARAMS];
	uint32_t             total = 0, msz = 0, stored = 0, blen = 0;
	const unsigned char *mb   = NULL;
	const uint8_t       *blob = NULL;
	const struct world  *w    = NULL;
	int                  n, i, use_stored = 0;
	s7_pointer           out = s7_nil(sc);

	a          = s7_cdr(a);
	mat_id     = (uint32_t)s7_integer(s7_car(a));
	a          = s7_cdr(a);
	shader_ref = (uint32_t)s7_integer(s7_car(a));
	src        = shader_src_cstr(shader_ref);
	if (!src)
		return out;
	n = script_shader_material_params(src, p, MATERIAL_MAX_PARAMS, &total);

	if (g_asset_api && g_asset_api->get_data)
		mb = (const unsigned char *)g_asset_api->get_data(mat_id, &msz);
	if (mb && msz >= MATERIAL_HEADER_BYTES) {
		memcpy(&stored, mb, sizeof(stored));
		use_stored = (stored == shader_ref);
	}
	if (g_entity_api && g_entity_api->get_world)
		w = g_entity_api->get_world();
	if (w && eid >= 0 && (uint32_t)eid < w->count)
		blob = world_material_params(w, (uint32_t)eid, &blen);

	for (i = n - 1; i >= 0; i--) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		uint32_t off = MATERIAL_HEADER_BYTES + p[i].offset;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		/* Shared material's stored value, then the entity override on top
		 * (offsets: the material carries the shader-ref header, the
		 * override is header-less std140 like the UBO the renderer binds). */
		if (use_stored && off + c * sizeof(float) <= msz)
			memcpy(v, mb + off, c * sizeof(float));
		if (blob && p[i].offset + c * sizeof(float) <= blen)
			memcpy(v, blob + p[i].offset, c * sizeof(float));
		out = s7_cons(sc, real_list(sc, v, (int)c), out);
	}
	return out;
}

/*
 * (krudd-entity-save-material-params entity-id shader-ref field-values) ->
 * unspecified. Packs the per-field values into shader-ref's std140 Material
 * layout — header-less, exactly the bytes the renderer uploads to the Material
 * UBO — and stores them as entity-id's per-entity override (through the scene
 * api, so it records an undo step). The mirror of krudd-entity-save-script-params
 * for materials; field-values is a list of per-field component lists.
 */
static s7_pointer sp_krudd_entity_save_material_params(s7_scheme *sc,
						       s7_pointer args)
{
	s7_pointer          a          = args;
	int32_t             eid        = (int32_t)s7_integer(s7_car(a));
	uint32_t            shader_ref;
	s7_pointer          values;
	const char         *src;
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint8_t             bytes[WORLD_MATERIAL_PARAM_CAP];
	uint32_t            total = 0, len;
	int                 n, i;
	s7_pointer          fv;

	a          = s7_cdr(a);
	shader_ref = (uint32_t)s7_integer(s7_car(a));
	a          = s7_cdr(a);
	values     = s7_car(a);
	fv         = values;
	src        = shader_src_cstr(shader_ref);
	if (!src)
		return s7_unspecified(sc);

	n = script_shader_material_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (n < 0)
		n = 0;
	len = total > sizeof(bytes) ? (uint32_t)sizeof(bytes) : total;
	memset(bytes, 0, len);

	for (i = 0; i < n; i++) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		uint32_t off = p[i].offset;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (s7_is_pair(fv)) {
			read_reals(sc, s7_car(fv), v, (int)c);
			fv = s7_cdr(fv);
		}
		if (off + c * sizeof(float) <= len)
			memcpy(bytes + off, v, c * sizeof(float));
	}
	if (g_entity_api && g_entity_api->set_material_params)
		g_entity_api->set_material_params(eid, bytes, len);
	return s7_unspecified(sc);
}

/*
 * Mesh parameters — the geometry twin of the script helpers above. A mesh's
 * params clause is its schema (introspected tight-packed, like a script's, no
 * std140 header); an entity's override blob holds the values. These three feed
 * the same widget dispatcher the script and material editors use, keyed on the
 * entity, so editing a box's width is the same gesture as editing a tint.
 */

/*
 * (krudd-mesh-params mesh-ref) -> ((name type off size components edit-kind
 * edit-min edit-max) ...), the bound mesh's params clause as editable
 * parameters. Empty when the asset is gone or declares no params.
 */
static s7_pointer sp_krudd_mesh_params(s7_scheme *sc, s7_pointer args)
{
	uint32_t            mesh_ref = (uint32_t)s7_integer(s7_car(args));
	const char         *src      = shader_src_cstr(mesh_ref);
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint32_t            total = 0;
	int                 n, i;
	s7_pointer          out = s7_nil(sc);

	if (!src)
		return out;
	n = script_mesh_params(src, p, MATERIAL_MAX_PARAMS, &total);
	for (i = n - 1; i >= 0; i--)
		out = s7_cons(sc,
			s7_list(sc, 8,
				s7_make_string(sc, p[i].name),
				s7_make_string(sc, p[i].type),
				s7_make_integer(sc, (s7_int)p[i].offset),
				s7_make_integer(sc, (s7_int)p[i].size),
				s7_make_integer(sc, (s7_int)p[i].components),
				s7_make_string(sc, p[i].edit),
				s7_make_real(sc, p[i].edit_min),
				s7_make_real(sc, p[i].edit_max)),
			out);
	return out;
}

/*
 * (krudd-entity-mesh-values entity-id mesh-ref) -> a per-field list of component
 * lists, entity-id's current values for mesh-ref's params: read from its override
 * blob where present, else the param's edit-hint default. This is the override ⊕
 * defaults the entity inspector edits (and the generator sees). The mesh mirror
 * of krudd-entity-script-values.
 */
static s7_pointer sp_krudd_entity_mesh_values(s7_scheme *sc, s7_pointer args)
{
	int32_t              eid      = (int32_t)s7_integer(s7_car(args));
	uint32_t             mesh_ref = (uint32_t)s7_integer(s7_cadr(args));
	const char          *src      = shader_src_cstr(mesh_ref);
	struct shader_param  p[MATERIAL_MAX_PARAMS];
	uint32_t             total = 0, blen = 0;
	const uint8_t       *blob = NULL;
	const struct world  *w    = NULL;
	int                  n, i;
	s7_pointer           out = s7_nil(sc);

	if (!src)
		return out;
	n = script_mesh_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (g_entity_api && g_entity_api->get_world)
		w = g_entity_api->get_world();
	if (w && eid >= 0 && (uint32_t)eid < w->count)
		blob = world_mesh_params(w, (uint32_t)eid, &blen);

	for (i = n - 1; i >= 0; i--) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (blob && p[i].offset + c * sizeof(float) <= blen)
			memcpy(v, blob + p[i].offset, c * sizeof(float));
		out = s7_cons(sc, real_list(sc, v, (int)c), out);
	}
	return out;
}

/*
 * (krudd-entity-save-mesh-params entity-id mesh-ref field-values) -> unspecified.
 * Packs the per-field values into mesh-ref's tight params layout and stores them
 * as entity-id's override (through the scene api, so it records an undo step).
 * The mirror of krudd-entity-save-script-params for meshes; field-values is a
 * list of per-field component lists.
 */
static s7_pointer sp_krudd_entity_save_mesh_params(s7_scheme *sc,
						   s7_pointer args)
{
	s7_pointer          a        = args;
	int32_t             eid      = (int32_t)s7_integer(s7_car(a));
	uint32_t            mesh_ref;
	s7_pointer          values;
	const char         *src;
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint8_t             bytes[WORLD_MESH_PARAM_CAP];
	uint32_t            total = 0, len;
	int                 n, i;
	s7_pointer          fv;

	a        = s7_cdr(a);
	mesh_ref = (uint32_t)s7_integer(s7_car(a));
	a        = s7_cdr(a);
	values   = s7_car(a);
	fv       = values;
	src      = shader_src_cstr(mesh_ref);
	if (!src)
		return s7_unspecified(sc);

	n = script_mesh_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (n < 0)
		n = 0;
	len = total > sizeof(bytes) ? (uint32_t)sizeof(bytes) : total;
	memset(bytes, 0, len);

	for (i = 0; i < n; i++) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		uint32_t off = p[i].offset;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (s7_is_pair(fv)) {
			read_reals(sc, s7_car(fv), v, (int)c);
			fv = s7_cdr(fv);
		}
		if (off + c * sizeof(float) <= len)
			memcpy(bytes + off, v, c * sizeof(float));
	}
	if (g_entity_api && g_entity_api->set_mesh_params)
		g_entity_api->set_mesh_params(eid, bytes, len);
	return s7_unspecified(sc);
}

/*
 * (krudd-texture-params texture-ref) -> ((name type off size components edit-kind
 * edit-min edit-max) ...), the texture's params clause as editable parameters —
 * the pixel mirror of krudd-mesh-params. Empty when the asset is gone or declares
 * no params.
 */
static s7_pointer sp_krudd_texture_params(s7_scheme *sc, s7_pointer args)
{
	uint32_t            tex_ref = (uint32_t)s7_integer(s7_car(args));
	const char         *src     = shader_src_cstr(tex_ref);
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint32_t            total = 0;
	int                 n, i;
	s7_pointer          out = s7_nil(sc);

	if (!src)
		return out;
	n = script_texture_params(src, p, MATERIAL_MAX_PARAMS, &total);
	for (i = n - 1; i >= 0; i--)
		out = s7_cons(sc,
			s7_list(sc, 8,
				s7_make_string(sc, p[i].name),
				s7_make_string(sc, p[i].type),
				s7_make_integer(sc, (s7_int)p[i].offset),
				s7_make_integer(sc, (s7_int)p[i].size),
				s7_make_integer(sc, (s7_int)p[i].components),
				s7_make_string(sc, p[i].edit),
				s7_make_real(sc, p[i].edit_min),
				s7_make_real(sc, p[i].edit_max)),
			out);
	return out;
}

/*
 * (krudd-entity-texture-values entity-id texture-ref) -> a per-field list of
 * component lists, entity-id's current values for texture-ref's params: read from
 * its override blob where present, else the param's edit-hint default — the
 * override ⊕ defaults the entity inspector edits and the baker sees. The texture
 * mirror of krudd-entity-mesh-values.
 */
static s7_pointer sp_krudd_entity_texture_values(s7_scheme *sc, s7_pointer args)
{
	int32_t              eid     = (int32_t)s7_integer(s7_car(args));
	uint32_t             tex_ref = (uint32_t)s7_integer(s7_cadr(args));
	const char          *src     = shader_src_cstr(tex_ref);
	struct shader_param  p[MATERIAL_MAX_PARAMS];
	uint32_t             total = 0, blen = 0;
	const uint8_t       *blob = NULL;
	const struct world  *w    = NULL;
	int                  n, i;
	s7_pointer           out = s7_nil(sc);

	if (!src)
		return out;
	n = script_texture_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (g_entity_api && g_entity_api->get_world)
		w = g_entity_api->get_world();
	if (w && eid >= 0 && (uint32_t)eid < w->count)
		blob = world_texture_params(w, (uint32_t)eid, &blen);

	for (i = n - 1; i >= 0; i--) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (blob && p[i].offset + c * sizeof(float) <= blen)
			memcpy(v, blob + p[i].offset, c * sizeof(float));
		out = s7_cons(sc, real_list(sc, v, (int)c), out);
	}
	return out;
}

/*
 * (krudd-entity-save-texture-params entity-id texture-ref field-values) ->
 * unspecified. Packs the per-field values into texture-ref's tight params layout
 * and stores them as entity-id's override (through the scene api, so it records
 * an undo step). The texture mirror of krudd-entity-save-mesh-params.
 */
static s7_pointer sp_krudd_entity_save_texture_params(s7_scheme *sc,
						      s7_pointer args)
{
	s7_pointer          a       = args;
	int32_t             eid     = (int32_t)s7_integer(s7_car(a));
	uint32_t            tex_ref;
	s7_pointer          values;
	const char         *src;
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint8_t             bytes[WORLD_TEXTURE_PARAM_CAP];
	uint32_t            total = 0, len;
	int                 n, i;
	s7_pointer          fv;

	a       = s7_cdr(a);
	tex_ref = (uint32_t)s7_integer(s7_car(a));
	a       = s7_cdr(a);
	values  = s7_car(a);
	fv      = values;
	src     = shader_src_cstr(tex_ref);
	if (!src)
		return s7_unspecified(sc);

	n = script_texture_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (n < 0)
		n = 0;
	len = total > sizeof(bytes) ? (uint32_t)sizeof(bytes) : total;
	memset(bytes, 0, len);

	for (i = 0; i < n; i++) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		uint32_t off = p[i].offset;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (s7_is_pair(fv)) {
			read_reals(sc, s7_car(fv), v, (int)c);
			fv = s7_cdr(fv);
		}
		if (off + c * sizeof(float) <= len)
			memcpy(bytes + off, v, c * sizeof(float));
	}
	if (g_entity_api && g_entity_api->set_texture_params)
		g_entity_api->set_texture_params(eid, bytes, len);
	return s7_unspecified(sc);
}

/*
 * (krudd-texture-values texture-ref) -> a per-field list of component lists, the
 * texture's declared parameter defaults (its params clause, no override) — the
 * seed the Assets-tab texture inspector's live-preview sliders start from, the
 * asset-level twin of krudd-entity-texture-values (which overlays an entity's
 * override). Empty when the asset is gone or declares no params.
 */
static s7_pointer sp_krudd_texture_values(s7_scheme *sc, s7_pointer args)
{
	uint32_t             tex_ref = (uint32_t)s7_integer(s7_car(args));
	const char          *src     = shader_src_cstr(tex_ref);
	struct shader_param  p[MATERIAL_MAX_PARAMS];
	uint32_t             total = 0;
	int                  n, i;
	s7_pointer           out = s7_nil(sc);

	if (!src)
		return out;
	n = script_texture_params(src, p, MATERIAL_MAX_PARAMS, &total);
	for (i = n - 1; i >= 0; i--) {
		uint32_t c = p[i].components > 4 ? 4 : p[i].components;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		out = s7_cons(sc, real_list(sc, v, (int)c), out);
	}
	return out;
}

/*
 * Live texture-preview cache. One GL texture is baked from the inspected
 * texture's (source, params, resolution) and re-uploaded only when that key
 * changes, so a still frame costs nothing and dragging a slider re-bakes. The
 * single object is reused across selections (glTexImage2D reallocates its
 * storage), so there is nothing to free until the module unloads.
 */
static GLuint   g_tex_prev_gl;    /* 0 = not yet allocated                 */
static uint32_t g_tex_prev_ref;   /* asset id of the last successful bake  */
static uint32_t g_tex_prev_res;   /* edge length of the last bake          */
static uint32_t g_tex_prev_hash;  /* FNV-1a of the last packed params      */
static int      g_tex_prev_valid; /* did the last bake succeed?            */

static uint32_t tex_prev_hash(const uint8_t *b, uint32_t n)
{
	uint32_t h = 2166136261u, i;

	for (i = 0; i < n; i++) {
		h ^= b[i];
		h *= 16777619u;
	}
	return h;
}

/*
 * (krudd-texture-preview texture-ref field-values res) draws a live res x res
 * bake of the texture as an inline image. field-values is the per-field list the
 * Parameters sliders edit; it is packed into the texture's tight params layout
 * (the same packing the entity texture-param save does, minus the world write)
 * and fed to texture_script_generate, so a slider drag re-bakes the swatch. res
 * is clamped to a preview-sized edge. Draws a disabled note instead when the
 * source is gone, the memory api is absent, or the shade clause faults.
 */
static s7_pointer sp_krudd_texture_preview(s7_scheme *sc, s7_pointer args)
{
	s7_pointer          a       = args;
	uint32_t            tex_ref = (uint32_t)s7_integer(s7_car(a)); a = s7_cdr(a);
	s7_pointer          values  = s7_car(a);                      a = s7_cdr(a);
	uint32_t            res     = (uint32_t)s7_integer(s7_car(a));
	const char         *src     = shader_src_cstr(tex_ref);
	struct shader_param p[MATERIAL_MAX_PARAMS];
	uint8_t             bytes[WORLD_TEXTURE_PARAM_CAP];
	uint32_t            total = 0, len, hash;
	int                 n, i;
	s7_pointer          fv = values;

	if (!src || res == 0) {
		ImGui::TextDisabled("(no preview)");
		return s7_unspecified(sc);
	}
	if (res > 256)
		res = 256;

	n = script_texture_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (n < 0)
		n = 0;
	len = total > sizeof(bytes) ? (uint32_t)sizeof(bytes) : total;
	memset(bytes, 0, len);
	for (i = 0; i < n; i++) {
		uint32_t c   = p[i].components > 4 ? 4 : p[i].components;
		uint32_t off = p[i].offset;
		float    v[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			v[k] = param_default(&p[i], k);
		if (s7_is_pair(fv)) {
			read_reals(sc, s7_car(fv), v, (int)c);
			fv = s7_cdr(fv);
		}
		if (off + c * sizeof(float) <= len)
			memcpy(bytes + off, v, c * sizeof(float));
	}
	hash = tex_prev_hash(bytes, len);

	if (!(g_tex_prev_gl && g_tex_prev_valid && g_tex_prev_ref == tex_ref &&
	      g_tex_prev_res == res && g_tex_prev_hash == hash)) {
		struct texture_blob *b =
			g_mem ? texture_script_generate(src, bytes, len, res, res,
							g_mem, NULL)
			      : NULL;

		g_tex_prev_ref   = tex_ref;
		g_tex_prev_res   = res;
		g_tex_prev_hash  = hash;
		g_tex_prev_valid = (b != NULL);
		if (b) {
			if (!g_tex_prev_gl) {
				glGenTextures(1, &g_tex_prev_gl);
				glBindTexture(GL_TEXTURE_2D, g_tex_prev_gl);
				glTexParameteri(GL_TEXTURE_2D,
						GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D,
						GL_TEXTURE_MAG_FILTER, GL_NEAREST);
				glTexParameteri(GL_TEXTURE_2D,
						GL_TEXTURE_WRAP_S,
						GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D,
						GL_TEXTURE_WRAP_T,
						GL_CLAMP_TO_EDGE);
			} else {
				glBindTexture(GL_TEXTURE_2D, g_tex_prev_gl);
			}
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)res,
				     (GLsizei)res, 0, GL_RGBA, GL_UNSIGNED_BYTE,
				     (const uint8_t *)(b + 1));
			glBindTexture(GL_TEXTURE_2D, 0);
			g_mem->free(b);
		}
	}

	if (g_tex_prev_valid && g_tex_prev_gl)
		ImGui::Image((ImTextureID)(intptr_t)g_tex_prev_gl,
			     ImVec2(128.0f, 128.0f));
	else
		ImGui::TextDisabled("(bake failed)");
	return s7_unspecified(sc);
}

/*
 * (krudd-mesh-preview mesh-ref material-ref res) renders mesh-ref shaded — the
 * real scene pipeline into an offscreen target, via scene_renderer's preview
 * service (preview_api.h) — and draws the result inline as a slowly rotating
 * thumbnail. material-ref 0 uses the built-in default material, so the mesh
 * inspector shows pure lit geometry regardless of any authored material. Draws a
 * disabled note when the preview service is absent (no renderer) or the render
 * fails. Unlike the texture preview's CPU bake, the pixels come from a GPU render
 * pass into a render-target texture — the first consumer of the FBO path.
 */
static s7_pointer sp_krudd_mesh_preview(s7_scheme *sc, s7_pointer args)
{
	s7_pointer a        = args;
	uint32_t   mesh_ref = (uint32_t)s7_integer(s7_car(a)); a = s7_cdr(a);
	uint32_t   mat_ref  = (uint32_t)s7_integer(s7_car(a)); a = s7_cdr(a);
	uint32_t   res      = (uint32_t)s7_integer(s7_car(a));
	float      yaw      = (float)ImGui::GetTime() * 0.5f;
	uint32_t   tex;

	if (!g_preview_api || !g_preview_api->render_mesh || res == 0) {
		ImGui::TextDisabled("(no preview)");
		return s7_unspecified(sc);
	}
	tex = g_preview_api->render_mesh(mesh_ref, mat_ref, res, yaw);
	if (tex)
		ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(192.0f, 192.0f));
	else
		ImGui::TextDisabled("(preview unavailable)");
	return s7_unspecified(sc);
}

/*
 * Override flags — the per-field twin of the *-values accessors above, one
 * boolean per param: is this entity's value an override that differs from the
 * baseline it would draw without one? The World tree lights a dot beside a
 * widget when its flag is #t. For a script or mesh the baseline is the param's
 * declared default; for a material it is the shared asset's stored value (else
 * the shader default), so the dot means "this one entity overrides the shared
 * material", never "the material author set a non-default". A value edited back
 * to its baseline clears the flag, so the dot tracks "customized", not "ever
 * touched". Order matches the *-values list exactly, so map can walk both.
 */

/* True when any of C floats in an override blob differ from base[]; #f (0)
 * when the entity has no override covering them, so an untouched param reads
 * as not-overridden. */
static int blob_differs(const uint8_t *blob, uint32_t off, uint32_t blen,
			const float *base, uint32_t c)
{
	uint32_t k;
	float    ov;

	if (!blob || off + c * sizeof(float) > blen)
		return 0;
	for (k = 0; k < c; k++) {
		memcpy(&ov, blob + off + k * sizeof(float), sizeof(float));
		if (ov != base[k])
			return 1;
	}
	return 0;
}

static s7_pointer sp_krudd_entity_script_overrides(s7_scheme *sc,
						   s7_pointer args)
{
	int32_t              eid = (int32_t)s7_integer(s7_car(args));
	uint32_t             ref = (uint32_t)s7_integer(s7_cadr(args));
	const char          *src = shader_src_cstr(ref);
	struct shader_param  p[MATERIAL_MAX_PARAMS];
	uint32_t             total = 0, blen = 0;
	const uint8_t       *blob = NULL;
	const struct world  *w    = NULL;
	int                  n, i;
	s7_pointer           out = s7_nil(sc);

	if (!src)
		return out;
	n = script_entity_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (g_entity_api && g_entity_api->get_world)
		w = g_entity_api->get_world();
	if (w && eid >= 0 && (uint32_t)eid < w->count)
		blob = world_script_params(w, (uint32_t)eid, &blen);

	for (i = n - 1; i >= 0; i--) {
		uint32_t c = p[i].components > 4 ? 4 : p[i].components;
		float    base[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			base[k] = param_default(&p[i], k);
		out = s7_cons(sc, s7_make_boolean(sc,
			blob_differs(blob, p[i].offset, blen, base, c)), out);
	}
	return out;
}

static s7_pointer sp_krudd_entity_mesh_overrides(s7_scheme *sc, s7_pointer args)
{
	int32_t              eid = (int32_t)s7_integer(s7_car(args));
	uint32_t             ref = (uint32_t)s7_integer(s7_cadr(args));
	const char          *src = shader_src_cstr(ref);
	struct shader_param  p[MATERIAL_MAX_PARAMS];
	uint32_t             total = 0, blen = 0;
	const uint8_t       *blob = NULL;
	const struct world  *w    = NULL;
	int                  n, i;
	s7_pointer           out = s7_nil(sc);

	if (!src)
		return out;
	n = script_mesh_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (g_entity_api && g_entity_api->get_world)
		w = g_entity_api->get_world();
	if (w && eid >= 0 && (uint32_t)eid < w->count)
		blob = world_mesh_params(w, (uint32_t)eid, &blen);

	for (i = n - 1; i >= 0; i--) {
		uint32_t c = p[i].components > 4 ? 4 : p[i].components;
		float    base[4];
		uint32_t k;

		for (k = 0; k < c; k++)
			base[k] = param_default(&p[i], k);
		out = s7_cons(sc, s7_make_boolean(sc,
			blob_differs(blob, p[i].offset, blen, base, c)), out);
	}
	return out;
}

static s7_pointer sp_krudd_entity_material_overrides(s7_scheme *sc,
						     s7_pointer args)
{
	s7_pointer           a          = args;
	int32_t              eid        = (int32_t)s7_integer(s7_car(a));
	uint32_t             mat_id, shader_ref;
	const char          *src;
	struct shader_param  p[MATERIAL_MAX_PARAMS];
	uint32_t             total = 0, msz = 0, stored = 0, blen = 0;
	const unsigned char *mb   = NULL;
	const uint8_t       *blob = NULL;
	const struct world  *w    = NULL;
	int                  n, i, use_stored = 0;
	s7_pointer           out = s7_nil(sc);

	a          = s7_cdr(a);
	mat_id     = (uint32_t)s7_integer(s7_car(a));
	a          = s7_cdr(a);
	shader_ref = (uint32_t)s7_integer(s7_car(a));
	src        = shader_src_cstr(shader_ref);
	if (!src)
		return out;
	n = script_shader_material_params(src, p, MATERIAL_MAX_PARAMS, &total);
	if (g_asset_api && g_asset_api->get_data)
		mb = (const unsigned char *)g_asset_api->get_data(mat_id, &msz);
	if (mb && msz >= MATERIAL_HEADER_BYTES) {
		memcpy(&stored, mb, sizeof(stored));
		use_stored = (stored == shader_ref);
	}
	if (g_entity_api && g_entity_api->get_world)
		w = g_entity_api->get_world();
	if (w && eid >= 0 && (uint32_t)eid < w->count)
		blob = world_material_params(w, (uint32_t)eid, &blen);

	for (i = n - 1; i >= 0; i--) {
		uint32_t c    = p[i].components > 4 ? 4 : p[i].components;
		uint32_t moff = MATERIAL_HEADER_BYTES + p[i].offset;
		float    base[4];
		uint32_t k;

		/* Baseline the entity draws WITHOUT its override: the shared
		 * material's stored value, else the shader default. */
		for (k = 0; k < c; k++)
			base[k] = param_default(&p[i], k);
		if (use_stored && moff + c * sizeof(float) <= msz)
			memcpy(base, mb + moff, c * sizeof(float));
		out = s7_cons(sc, s7_make_boolean(sc,
			blob_differs(blob, p[i].offset, blen, base, c)), out);
	}
	return out;
}

/* (krudd-shader-stages src) -> the declared stage list, or "" if none. */
static s7_pointer sp_krudd_shader_stages(s7_scheme *sc, s7_pointer args)
{
	s7_pointer src = s7_car(args);

	return s7_make_string(sc,
		shader_stages_from_source(s7_is_string(src) ? s7_string(src) : ""));
}

/* (krudd-asset-save-text id text) -> unspecified. Writes live and persists. */
static s7_pointer sp_krudd_asset_save_text(s7_scheme *sc, s7_pointer args)
{
	uint32_t    id  = (uint32_t)s7_integer(s7_car(args));
	const char *txt = s7_is_string(s7_cadr(args)) ? s7_string(s7_cadr(args)) : "";
	uint32_t    len = (uint32_t)strlen(txt);

	if (g_asset_mut)
		g_asset_mut->set_data(id, txt, len);
	maybe_persist_asset(id, ASSET_TYPE_TEXT, txt, len);
	return s7_unspecified(sc);
}

/*
 * (krudd-asset-save-shader id text) -> #t on a successful compile+save, #f
 * on a failed compile (nothing is committed, so a broken edit never reaches
 * the live asset). Preserves the shader-transpile validation gate #390 added.
 */
static s7_pointer sp_krudd_asset_save_shader(s7_scheme *sc, s7_pointer args)
{
	uint32_t                 id  = (uint32_t)s7_integer(s7_car(args));
	const char               *txt = s7_is_string(s7_cadr(args))
		? s7_string(s7_cadr(args)) : "";
	uint32_t                 len = (uint32_t)strlen(txt);
	struct asset_decl_field  decl[2];

	if (!shader_compiles(txt))
		return s7_f(sc);

	decl[0].key   = "format";
	decl[0].value = SHADER_FORMAT;
	decl[1].key   = "stages";
	decl[1].value = shader_stages_from_source(txt);

	if (g_asset_mut) {
		g_asset_mut->set_data(id, txt, len);
		if (g_asset_mut->set_decl)
			g_asset_mut->set_decl(id, decl, 2);
	}
	maybe_persist_asset(id, ASSET_TYPE_SHADER, txt, len);
	return s7_t(sc);
}

/*
 * (krudd-asset-save-material id shader-ref field-values) -> unspecified. Packs
 * the per-field values into the shader-ref's std140 Material layout and stores
 * the v3 wire form; field-values is a list of per-field component lists.
 */
static s7_pointer sp_krudd_asset_save_material(s7_scheme *sc, s7_pointer args)
{
	s7_pointer    p          = args;
	uint32_t      id         = (uint32_t)s7_integer(s7_car(p)); p = s7_cdr(p);
	uint32_t      shader_ref = (uint32_t)s7_integer(s7_car(p)); p = s7_cdr(p);
	s7_pointer    values     = s7_car(p);                       p = s7_cdr(p);
	const char   *src        = shader_src_cstr(shader_ref);
	unsigned char bytes[MATERIAL_WIRE_CAP + 3 * sizeof(uint32_t)];
	uint32_t      len;
	uint32_t      tex = 0, tw = 0, th = 0;

	if (!src)
		return s7_unspecified(sc);
	len = pack_material(sc, shader_ref, src, values, bytes, MATERIAL_WIRE_CAP);
	/*
	 * Optional texture slot: trailing (tex-ref width height) appends the
	 * trailer the renderer reads to bake and bind this material's procedural
	 * texture. A tex-ref of 0, absent args, or a zero dimension leaves the
	 * material texture-less — byte-for-byte the pre-texture wire form.
	 */
	if (s7_is_pair(p)) { tex = (uint32_t)s7_integer(s7_car(p)); p = s7_cdr(p); }
	if (s7_is_pair(p)) { tw  = (uint32_t)s7_integer(s7_car(p)); p = s7_cdr(p); }
	if (s7_is_pair(p)) { th  = (uint32_t)s7_integer(s7_car(p)); p = s7_cdr(p); }
	if (tex != 0 && tw > 0 && th > 0 &&
	    len + 3u * sizeof(uint32_t) <= sizeof(bytes)) {
		memcpy(bytes + len, &tex, sizeof(tex)); len += (uint32_t)sizeof(tex);
		memcpy(bytes + len, &tw,  sizeof(tw));  len += (uint32_t)sizeof(tw);
		memcpy(bytes + len, &th,  sizeof(th));  len += (uint32_t)sizeof(th);
	}
	if (g_asset_mut)
		g_asset_mut->set_data(id, bytes, len);
	maybe_persist_asset(id, ASSET_TYPE_MATERIAL, bytes, len);
	return s7_unspecified(sc);
}

/* (krudd-asset-delete id) -> unspecified. Works for any authored type. */
static s7_pointer sp_krudd_asset_delete(s7_scheme *sc, s7_pointer args)
{
	uint32_t id = (uint32_t)s7_integer(s7_car(args));

	if (g_asset_mut)
		g_asset_mut->destroy(id);
	if (g_backend && (g_backend->get_caps() & BACKEND_CAP_PROJECT_PERSIST))
		g_backend->delete_asset(id);
	return s7_unspecified(sc);
}

/* (krudd-asset-create-text path) -> the new id, or 0 on failure. */
static s7_pointer sp_krudd_asset_create_text(s7_scheme *sc, s7_pointer args)
{
	s7_pointer  path = s7_car(args);
	const char *p    = s7_is_string(path) ? s7_string(path) : "";
	uint32_t    nid  = 0;

	if (g_asset_mut)
		nid = g_asset_mut->create(p, ASSET_TYPE_TEXT, "", 0);
	if (nid != 0)
		maybe_persist_asset(nid, ASSET_TYPE_TEXT, "", 0);
	return s7_make_integer(sc, (s7_int)nid);
}

/*
 * (krudd-asset-create-shader path) -> the new id, or 0 on failure. Seeds
 * from the built-in scene shader so authoring starts from working source;
 * no declaration is set yet, matching the pre-port New Asset flow — the
 * first successful Save publishes it.
 */
static s7_pointer sp_krudd_asset_create_shader(s7_scheme *sc, s7_pointer args)
{
	s7_pointer  path = s7_car(args);
	const char *p    = s7_is_string(path) ? s7_string(path) : "";
	const char *seed = default_shader_src();
	uint32_t    len  = (uint32_t)strlen(seed);
	uint32_t    nid  = 0;

	if (g_asset_mut)
		nid = g_asset_mut->create(p, ASSET_TYPE_SHADER, seed, len);
	if (nid != 0)
		maybe_persist_asset(nid, ASSET_TYPE_SHADER, seed, len);
	return s7_make_integer(sc, (s7_int)nid);
}

/*
 * (krudd-asset-create-material path) -> the new id, or 0 on failure. A new
 * material names the built-in scene shader and takes that shader's default
 * parameter values (its base_color defaults to white), packed in the v3 wire
 * form — a material is never shaderless.
 */
static s7_pointer sp_krudd_asset_create_material(s7_scheme *sc, s7_pointer args)
{
	const char   *p          = s7_is_string(s7_car(args))
		? s7_string(s7_car(args)) : "";
	uint32_t      shader_ref = asset_id_by_path("builtin://shader/scene");
	const char   *src        = shader_src_cstr(shader_ref);
	unsigned char bytes[MATERIAL_WIRE_CAP];
	uint32_t      len        = MATERIAL_HEADER_BYTES;
	uint32_t      nid        = 0;

	if (src)
		len = pack_material(sc, shader_ref, src, s7_nil(sc), bytes,
				    sizeof(bytes));
	else
		memcpy(bytes, &shader_ref, sizeof(shader_ref));
	if (g_asset_mut)
		nid = g_asset_mut->create(p, ASSET_TYPE_MATERIAL, bytes, len);
	if (nid != 0)
		maybe_persist_asset(nid, ASSET_TYPE_MATERIAL, bytes, len);
	return s7_make_integer(sc, (s7_int)nid);
}

/*
 * (krudd-asset-clone-shader name text) -> the new id, or 0 on failure (e.g.
 * a duplicate path) — the built-in shader "Clone" flow's whole commit:
 * create, publish the derived declaration, persist.
 */
static s7_pointer sp_krudd_asset_clone_shader(s7_scheme *sc, s7_pointer args)
{
	s7_pointer               name = s7_car(args);
	s7_pointer               text = s7_cadr(args);
	const char              *nm   = s7_is_string(name) ? s7_string(name) : "";
	const char              *txt  = s7_is_string(text) ? s7_string(text) : "";
	uint32_t                 len  = (uint32_t)strlen(txt);
	uint32_t                 nid  = 0;
	struct asset_decl_field  decl[2];

	if (g_asset_mut)
		nid = g_asset_mut->create(nm, ASSET_TYPE_SHADER, txt, len);
	if (nid == 0)
		return s7_make_integer(sc, 0);

	decl[0].key   = "format";
	decl[0].value = SHADER_FORMAT;
	decl[1].key   = "stages";
	decl[1].value = shader_stages_from_source(txt);
	if (g_asset_mut->set_decl)
		g_asset_mut->set_decl(nid, decl, 2);
	maybe_persist_asset(nid, ASSET_TYPE_SHADER, txt, len);
	return s7_make_integer(sc, (s7_int)nid);
}

/* (krudd-script-hooks src) -> the declared hook list, or "" if none. */
static s7_pointer sp_krudd_script_hooks(s7_scheme *sc, s7_pointer args)
{
	s7_pointer src = s7_car(args);

	return s7_make_string(sc,
		script_hooks_from_source(s7_is_string(src) ? s7_string(src) : ""));
}

/*
 * (krudd-asset-save-script id text) -> #t on a well-formed save, #f when the
 * source is not a (script ...) form with at least one hook (nothing is
 * committed, so a broken edit never reaches the live asset) — the script
 * analogue of krudd-asset-save-shader's compile gate.
 */
static s7_pointer sp_krudd_asset_save_script(s7_scheme *sc, s7_pointer args)
{
	uint32_t                 id  = (uint32_t)s7_integer(s7_car(args));
	const char               *txt = s7_is_string(s7_cadr(args))
		? s7_string(s7_cadr(args)) : "";
	uint32_t                 len = (uint32_t)strlen(txt);
	struct asset_decl_field  decl[2];

	if (!script_form_ok(txt))
		return s7_f(sc);

	decl[0].key   = "format";
	decl[0].value = SCRIPT_FORMAT;
	decl[1].key   = "hooks";
	decl[1].value = script_hooks_from_source(txt);

	if (g_asset_mut) {
		g_asset_mut->set_data(id, txt, len);
		if (g_asset_mut->set_decl)
			g_asset_mut->set_decl(id, decl, 2);
	}
	maybe_persist_asset(id, ASSET_TYPE_SCRIPT, txt, len);
	return s7_t(sc);
}

/*
 * (krudd-asset-create-script path) -> the new id, or 0 on failure. Seeds from
 * the built-in spinner script so authoring starts from a working (script ...)
 * form; no declaration is set yet — the first successful Save publishes it,
 * mirroring krudd-asset-create-shader.
 */
static s7_pointer sp_krudd_asset_create_script(s7_scheme *sc, s7_pointer args)
{
	s7_pointer  path = s7_car(args);
	const char *p    = s7_is_string(path) ? s7_string(path) : "";
	const char *seed = default_script_src();
	uint32_t    len  = (uint32_t)strlen(seed);
	uint32_t    nid  = 0;

	if (g_asset_mut)
		nid = g_asset_mut->create(p, ASSET_TYPE_SCRIPT, seed, len);
	if (nid != 0)
		maybe_persist_asset(nid, ASSET_TYPE_SCRIPT, seed, len);
	return s7_make_integer(sc, (s7_int)nid);
}

/*
 * (krudd-asset-clone-script name text) -> the new id, or 0 on failure (e.g. a
 * duplicate path) — the built-in script "Clone" flow's whole commit: create,
 * publish the derived declaration, persist. Mirrors krudd-asset-clone-shader.
 */
static s7_pointer sp_krudd_asset_clone_script(s7_scheme *sc, s7_pointer args)
{
	s7_pointer               name = s7_car(args);
	s7_pointer               text = s7_cadr(args);
	const char              *nm   = s7_is_string(name) ? s7_string(name) : "";
	const char              *txt  = s7_is_string(text) ? s7_string(text) : "";
	uint32_t                 len  = (uint32_t)strlen(txt);
	uint32_t                 nid  = 0;
	struct asset_decl_field  decl[2];

	if (g_asset_mut)
		nid = g_asset_mut->create(nm, ASSET_TYPE_SCRIPT, txt, len);
	if (nid == 0)
		return s7_make_integer(sc, 0);

	decl[0].key   = "format";
	decl[0].value = SCRIPT_FORMAT;
	decl[1].key   = "hooks";
	decl[1].value = script_hooks_from_source(txt);
	if (g_asset_mut->set_decl)
		g_asset_mut->set_decl(nid, decl, 2);
	maybe_persist_asset(nid, ASSET_TYPE_SCRIPT, txt, len);
	return s7_make_integer(sc, (s7_int)nid);
}

/*
 * (krudd-asset-clone-material name shader-ref field-values [tex-ref w h]) ->
 * the new id, or 0 on failure (e.g. a duplicate path) — the built-in material
 * "Clone" flow's whole commit: pack the current shader + values (plus the
 * optional bound texture trailer, mirroring krudd-asset-save-material) into
 * the v3 wire form, create, persist. Mirrors krudd-asset-clone-shader above.
 */
static s7_pointer sp_krudd_asset_clone_material(s7_scheme *sc, s7_pointer args)
{
	s7_pointer    p          = args;
	const char   *nm         = s7_is_string(s7_car(p))
		? s7_string(s7_car(p)) : "";
	uint32_t      shader_ref;
	s7_pointer    values;
	const char   *src;
	unsigned char bytes[MATERIAL_WIRE_CAP + 3 * sizeof(uint32_t)];
	uint32_t      len;
	uint32_t      nid = 0;
	uint32_t      tex = 0, tw = 0, th = 0;

	p          = s7_cdr(p);
	shader_ref = (uint32_t)s7_integer(s7_car(p)); p = s7_cdr(p);
	values     = s7_car(p);                       p = s7_cdr(p);
	src        = shader_src_cstr(shader_ref);
	if (!src)
		return s7_make_integer(sc, 0);

	len = pack_material(sc, shader_ref, src, values, bytes, MATERIAL_WIRE_CAP);
	if (s7_is_pair(p)) { tex = (uint32_t)s7_integer(s7_car(p)); p = s7_cdr(p); }
	if (s7_is_pair(p)) { tw  = (uint32_t)s7_integer(s7_car(p)); p = s7_cdr(p); }
	if (s7_is_pair(p)) { th  = (uint32_t)s7_integer(s7_car(p)); p = s7_cdr(p); }
	if (tex != 0 && tw > 0 && th > 0 &&
	    len + 3u * sizeof(uint32_t) <= sizeof(bytes)) {
		memcpy(bytes + len, &tex, sizeof(tex)); len += (uint32_t)sizeof(tex);
		memcpy(bytes + len, &tw,  sizeof(tw));  len += (uint32_t)sizeof(tw);
		memcpy(bytes + len, &th,  sizeof(th));  len += (uint32_t)sizeof(th);
	}
	if (g_asset_mut)
		nid = g_asset_mut->create(nm, ASSET_TYPE_MATERIAL, bytes, len);
	if (nid != 0)
		maybe_persist_asset(nid, ASSET_TYPE_MATERIAL, bytes, len);
	return s7_make_integer(sc, (s7_int)nid);
}

/*
 * (krudd-asset-save-mesh id text) -> #t on a well-formed save, #f when
 * the source is not a (mesh ...) form with a (generate ...) clause (nothing
 * is committed, so a broken edit never reaches the live asset) — the mesh
 * analogue of krudd-asset-save-script.
 */
static s7_pointer sp_krudd_asset_save_mesh(s7_scheme *sc, s7_pointer args)
{
	uint32_t                 id  = (uint32_t)s7_integer(s7_car(args));
	const char               *txt = s7_is_string(s7_cadr(args))
		? s7_string(s7_cadr(args)) : "";
	uint32_t                 len = (uint32_t)strlen(txt);
	struct asset_decl_field  decl[1];

	if (!mesh_form_ok(txt))
		return s7_f(sc);

	decl[0].key   = "format";
	decl[0].value = MESH_FORMAT;

	if (g_asset_mut) {
		g_asset_mut->set_data(id, txt, len);
		if (g_asset_mut->set_decl)
			g_asset_mut->set_decl(id, decl, 1);
	}
	maybe_persist_asset(id, ASSET_TYPE_MESH, txt, len);
	return s7_t(sc);
}

/*
 * (krudd-asset-create-mesh path) -> the new id, or 0 on failure. Seeds
 * from the built-in grid mesh so authoring starts from a working (mesh ...)
 * form; no declaration is set yet — the first successful Save publishes it,
 * mirroring krudd-asset-create-script.
 */
static s7_pointer sp_krudd_asset_create_mesh(s7_scheme *sc, s7_pointer args)
{
	s7_pointer  path = s7_car(args);
	const char *p    = s7_is_string(path) ? s7_string(path) : "";
	const char *seed = default_mesh_src();
	uint32_t    len  = (uint32_t)strlen(seed);
	uint32_t    nid  = 0;

	if (g_asset_mut)
		nid = g_asset_mut->create(p, ASSET_TYPE_MESH, seed, len);
	if (nid != 0)
		maybe_persist_asset(nid, ASSET_TYPE_MESH, seed, len);
	return s7_make_integer(sc, (s7_int)nid);
}

/*
 * (krudd-asset-clone-mesh name text) -> the new id, or 0 on failure
 * (e.g. a duplicate path) — the built-in mesh "Clone" flow's whole commit:
 * create, publish the derived declaration, persist. Mirrors
 * krudd-asset-clone-script.
 */
static s7_pointer sp_krudd_asset_clone_mesh(s7_scheme *sc, s7_pointer args)
{
	s7_pointer               name = s7_car(args);
	s7_pointer               text = s7_cadr(args);
	const char              *nm   = s7_is_string(name) ? s7_string(name) : "";
	const char              *txt  = s7_is_string(text) ? s7_string(text) : "";
	uint32_t                 len  = (uint32_t)strlen(txt);
	uint32_t                 nid  = 0;
	struct asset_decl_field  decl[1];

	if (g_asset_mut)
		nid = g_asset_mut->create(nm, ASSET_TYPE_MESH, txt, len);
	if (nid == 0)
		return s7_make_integer(sc, 0);

	decl[0].key   = "format";
	decl[0].value = MESH_FORMAT;
	if (g_asset_mut->set_decl)
		g_asset_mut->set_decl(nid, decl, 1);
	maybe_persist_asset(nid, ASSET_TYPE_MESH, txt, len);
	return s7_make_integer(sc, (s7_int)nid);
}

/*
 * (krudd-md-preview text h) -> unspecified. Parses text fresh every call
 * (cheap relative to a 16ms frame budget) and draws it in a bordered,
 * horizontally-scrolling child of height h — folding md_parse.h's block
 * array entirely behind this primitive rather than marshalling md_block/
 * md_span structs into Scheme data no caller needs to see.
 */
static s7_pointer sp_krudd_md_preview(s7_scheme *sc, s7_pointer args)
{
	static struct md_block blocks[MD_BLOCKS_MAX];
	s7_pointer              text = s7_car(args);
	float                   h    = (float)s7_number_to_real(sc, s7_cadr(args));
	const char             *src  = s7_is_string(text) ? s7_string(text) : "";
	int32_t                 n;

	n = md_parse(src, blocks, MD_BLOCKS_MAX);
	ImGui::BeginChild("##mdpreview", ImVec2(0.0f, h), true,
			  ImGuiWindowFlags_HorizontalScrollbar);
	md_draw_blocks(blocks, n);
	ImGui::EndChild();
	return s7_unspecified(sc);
}

} /* extern "C" — s7 callbacks */

/*
 * Start the interpreter (if needed), register the panel primitives, and load
 * the image — once. Idempotent and lazy: it costs nothing until the first
 * Scheme-drawn panel renders. Returns the interpreter, or NULL if it failed
 * to start, in which case callers fall back.
 */
static s7_scheme *ensure_panel_scm(void)
{
	static bool ready;
	s7_scheme  *sc = script_s7();

	if (ready || !sc)
		return sc;
	s7_define_function(sc, "imgui-text", sp_imgui_text, 1, 0, false,
			   "(imgui-text str) draw a line of text");
	s7_define_function(sc, "imgui-text-disabled", sp_imgui_text_disabled,
			   1, 0, false,
			   "(imgui-text-disabled str) draw a dimmed line of text");
	s7_define_function(sc, "krudd-stats", sp_krudd_stats, 0, 0, false,
			   "(krudd-stats) -> (fps frame-ms frame-count) or #f");
	s7_define_function(sc, "imgui-text-colored", sp_imgui_text_colored,
			   5, 0, false,
			   "(imgui-text-colored r g b a str) coloured line");
	s7_define_function(sc, "imgui-small-button", sp_imgui_small_button,
			   1, 0, false,
			   "(imgui-small-button label) -> #t when clicked");
	s7_define_function(sc, "imgui-same-line", sp_imgui_same_line, 0, 0,
			   false, "(imgui-same-line) stay on this line");
	s7_define_function(sc, "imgui-checkbox", sp_imgui_checkbox, 2, 0, false,
			   "(imgui-checkbox label state) -> new state");
	s7_define_function(sc, "imgui-separator", sp_imgui_separator, 0, 0,
			   false, "(imgui-separator) horizontal rule");
	s7_define_function(sc, "imgui-collapsing-header",
			   sp_imgui_collapsing_header, 1, 1, false,
			   "(imgui-collapsing-header label [default-open]) -> #t when open");
	s7_define_function(sc, "imgui-begin-table", sp_imgui_begin_table, 2, 0,
			   false, "(imgui-begin-table id ncols) -> #t if opened");
	s7_define_function(sc, "imgui-table-setup-column",
			   sp_imgui_table_setup_column, 1, 0, false,
			   "(imgui-table-setup-column label) declare a column");
	s7_define_function(sc, "imgui-table-headers-row",
			   sp_imgui_table_headers_row, 0, 0, false,
			   "(imgui-table-headers-row) draw the header row");
	s7_define_function(sc, "imgui-table-next-row", sp_imgui_table_next_row,
			   0, 0, false, "(imgui-table-next-row) start a row");
	s7_define_function(sc, "imgui-table-next-column",
			   sp_imgui_table_next_column, 0, 0, false,
			   "(imgui-table-next-column) advance to the next cell");
	s7_define_function(sc, "imgui-end-table", sp_imgui_end_table, 0, 0,
			   false, "(imgui-end-table) close the table");
	s7_define_function(sc, "imgui-begin-child", sp_imgui_begin_child, 3, 0,
			   false, "(imgui-begin-child id w h) scroll region");
	s7_define_function(sc, "imgui-end-child", sp_imgui_end_child, 0, 0,
			   false, "(imgui-end-child) close the scroll region");
	s7_define_function(sc, "imgui-set-scroll-here-y",
			   sp_imgui_set_scroll_here_y, 1, 0, false,
			   "(imgui-set-scroll-here-y ratio) snap the scroll");
	s7_define_function(sc, "imgui-viewport-work-height",
			   sp_imgui_viewport_work_height, 0, 0, false,
			   "(imgui-viewport-work-height) -> usable height px");
	s7_define_function(sc, "krudd-subsystems", sp_krudd_subsystems, 0, 0,
			   false,
			   "(krudd-subsystems) -> rows of (name api? tick? size)");
	s7_define_function(sc, "krudd-log-history", sp_krudd_log_history, 0, 0,
			   false,
			   "(krudd-log-history) -> ((level . text) ...) or #f");
	s7_define_function(sc, "imgui-begin-disabled", sp_imgui_begin_disabled,
			   1, 0, false,
			   "(imgui-begin-disabled flag) grey out widgets");
	s7_define_function(sc, "imgui-end-disabled", sp_imgui_end_disabled, 0, 0,
			   false, "(imgui-end-disabled) end a disabled block");
	s7_define_function(sc, "imgui-set-next-item-width",
			   sp_imgui_set_next_item_width, 1, 0, false,
			   "(imgui-set-next-item-width w) width the next widget");
	s7_define_function(sc, "imgui-input-text", sp_imgui_input_text, 2, 0,
			   false,
			   "(imgui-input-text id text) -> (text . committed?)");
	s7_define_function(sc, "imgui-input-float3", sp_imgui_input_float3, 2, 0,
			   false,
			   "(imgui-input-float3 id vec) -> (vec . changed?)");
	s7_define_function(sc, "imgui-input-float4", sp_imgui_input_float4, 2, 0,
			   false,
			   "(imgui-input-float4 id vec) -> (vec . changed?)");
	s7_define_function(sc, "imgui-input-float", sp_imgui_input_float, 2, 0,
			   false, "(imgui-input-float id x) -> (x . changed?)");
	s7_define_function(sc, "imgui-input-float2", sp_imgui_input_float2, 2, 0,
			   false,
			   "(imgui-input-float2 id vec) -> (vec . changed?)");
	s7_define_function(sc, "imgui-color-edit3", sp_imgui_color_edit3, 2, 0,
			   false,
			   "(imgui-color-edit3 id rgb) -> (rgb . changed?)");
	s7_define_function(sc, "imgui-slider-float", sp_imgui_slider_float, 4, 0,
			   false,
			   "(imgui-slider-float id x lo hi) -> (x . changed?)");
	s7_define_function(sc, "imgui-begin-combo", sp_imgui_begin_combo, 2, 0,
			   false, "(imgui-begin-combo id preview) -> #t if open");
	s7_define_function(sc, "imgui-end-combo", sp_imgui_end_combo, 0, 0,
			   false, "(imgui-end-combo) close the dropdown");
	s7_define_function(sc, "imgui-selectable", sp_imgui_selectable, 3, 0,
			   false,
			   "(imgui-selectable label sel? span?) -> #t if clicked");
	s7_define_function(sc, "imgui-tree-node", sp_imgui_tree_node, 4, 0,
			   false,
			   "(imgui-tree-node id label leaf? sel?) -> (open? . clicked?)");
	s7_define_function(sc, "imgui-tree-pop", sp_imgui_tree_pop, 0, 0, false,
			   "(imgui-tree-pop) close a tree-node opened as non-leaf");
	s7_define_function(sc, "imgui-set-item-default-focus",
			   sp_imgui_set_item_default_focus, 0, 0, false,
			   "(imgui-set-item-default-focus) focus the last item");
	s7_define_function(sc, "imgui-calc-text-width",
			   sp_imgui_calc_text_width, 1, 0, false,
			   "(imgui-calc-text-width str) -> width px");
	s7_define_function(sc, "imgui-same-line-right", sp_imgui_same_line_right,
			   1, 0, false,
			   "(imgui-same-line-right px) right-align the next item");
	s7_define_function(sc, "imgui-push-style-color-button",
			   sp_imgui_push_style_color_button, 4, 0, false,
			   "(imgui-push-style-color-button r g b a) tint button");
	s7_define_function(sc, "imgui-pop-style-color", sp_imgui_pop_style_color,
			   0, 0, false,
			   "(imgui-pop-style-color) undo a pushed colour");
	s7_define_function(sc, "imgui-push-id", sp_imgui_push_id, 1, 0, false,
			   "(imgui-push-id str) open a widget-id scope");
	s7_define_function(sc, "imgui-pop-id", sp_imgui_pop_id, 0, 0, false,
			   "(imgui-pop-id) close one imgui-push-id scope");
	s7_define_function(sc, "imgui-dot", sp_imgui_dot, 4, 0, false,
			   "(imgui-dot r g b a) inline filled circle");
	s7_define_function(sc, "krudd-world-caps", sp_krudd_world_caps, 0, 0,
			   false,
			   "(krudd-world-caps) -> (entity-api? asset-api?)");
	s7_define_function(sc, "krudd-selected", sp_krudd_selected, 0, 0, false,
			   "(krudd-selected) -> selected entity id or -1");
	s7_define_function(sc, "krudd-world-entities", sp_krudd_world_entities,
			   0, 0, false,
			   "(krudd-world-entities) -> ((id . name) ...) or #f");
	s7_define_function(sc, "krudd-entity-inspect", sp_krudd_entity_inspect,
			   1, 0, false,
			   "(krudd-entity-inspect id) -> inspector bundle or #f");
	s7_define_function(sc, "krudd-mesh-assets", sp_krudd_mesh_assets, 0, 0,
			   false, "(krudd-mesh-assets) -> ((id . path) ...)");
	s7_define_function(sc, "krudd-material-assets", sp_krudd_material_assets,
			   0, 0, false,
			   "(krudd-material-assets) -> ((id . path) ...)");
	s7_define_function(sc, "krudd-shader-assets", sp_krudd_shader_assets,
			   0, 0, false,
			   "(krudd-shader-assets) -> ((id . path) ...)");
	s7_define_function(sc, "krudd-script-assets", sp_krudd_script_assets,
			   0, 0, false,
			   "(krudd-script-assets) -> ((id . path) ...)");
	s7_define_function(sc, "krudd-texture-assets", sp_krudd_texture_assets,
			   0, 0, false,
			   "(krudd-texture-assets) -> ((id . path) ...)");
	s7_define_function(sc, "krudd-material-texture", sp_krudd_material_texture,
			   1, 0, false,
			   "(krudd-material-texture id) -> (tex-ref w h) or ()");
	s7_define_function(sc, "krudd-asset-find", sp_krudd_asset_find, 1, 0,
			   false, "(krudd-asset-find ref) -> path or #f");
	s7_define_function(sc, "krudd-entity-create", sp_krudd_entity_create, 0,
			   0, false, "(krudd-entity-create) -> new id or -1");
	s7_define_function(sc, "krudd-entity-destroy", sp_krudd_entity_destroy,
			   1, 0, false, "(krudd-entity-destroy id) tombstone it");
	s7_define_function(sc, "krudd-entity-select", sp_krudd_entity_select, 1,
			   0, false, "(krudd-entity-select id) set the selection");
	s7_define_function(sc, "krudd-entity-set-name", sp_krudd_entity_set_name,
			   2, 0, false,
			   "(krudd-entity-set-name id str) rename the entity");
	s7_define_function(sc, "krudd-entity-set-transform",
			   sp_krudd_entity_set_transform, 4, 0, false,
			   "(krudd-entity-set-transform id pos rot scl)");
	s7_define_function(sc, "krudd-entity-set-render-ref",
			   sp_krudd_entity_set_render_ref, 2, 0, false,
			   "(krudd-entity-set-render-ref id ref) bind a mesh");
	s7_define_function(sc, "krudd-entity-set-material-ref",
			   sp_krudd_entity_set_material_ref, 2, 0, false,
			   "(krudd-entity-set-material-ref id ref) bind material");
	s7_define_function(sc, "krudd-entity-set-script-ref",
			   sp_krudd_entity_set_script_ref, 2, 0, false,
			   "(krudd-entity-set-script-ref id ref) bind script");
	s7_define_function(sc, "imgui-begin-table-plain",
			   sp_imgui_begin_table_plain, 2, 0, false,
			   "(imgui-begin-table-plain id ncols) borderless table");
	s7_define_function(sc, "imgui-table-setup-column-fixed",
			   sp_imgui_table_setup_column_fixed, 2, 0, false,
			   "(imgui-table-setup-column-fixed label w) fixed column");
	s7_define_function(sc, "krudd-gizmo-mode", sp_krudd_gizmo_mode, 0, 0,
			   false, "(krudd-gizmo-mode) -> 0 move 1 rotate 2 scale");
	s7_define_function(sc, "krudd-set-gizmo-mode", sp_krudd_set_gizmo_mode,
			   1, 0, false, "(krudd-set-gizmo-mode m) set the tool");
	s7_define_function(sc, "imgui-button", sp_imgui_button, 1, 0, false,
			   "(imgui-button label) -> #t when clicked");
	s7_define_function(sc, "imgui-input-text-enter",
			   sp_imgui_input_text_enter, 2, 0, false,
			   "(imgui-input-text-enter id text) -> (text . entered?)");
	s7_define_function(sc, "imgui-input-text-multiline",
			   sp_imgui_input_text_multiline, 4, 0, false,
			   "(imgui-input-text-multiline id text h ro?) -> (text . changed?)");
	s7_define_function(sc, "imgui-combo", sp_imgui_combo, 3, 0, false,
			   "(imgui-combo id items current) -> new index");
	s7_define_function(sc, "imgui-color-edit4", sp_imgui_color_edit4, 2, 0,
			   false, "(imgui-color-edit4 id rgba) -> (rgba . changed?)");
	s7_define_function(sc, "imgui-mesh-drag-source",
			   sp_imgui_mesh_drag_source, 2, 0, false,
			   "(imgui-mesh-drag-source id label) drag-to-spawn source");
	s7_define_function(sc, "krudd-assets", sp_krudd_assets, 0, 0, false,
			   "(krudd-assets) -> (builtin-rows project-rows) or #f");
	s7_define_function(sc, "krudd-asset-mut?", sp_krudd_asset_mut, 0, 0,
			   false, "(krudd-asset-mut?) -> #t when mutation is available");
	s7_define_function(sc, "krudd-asset-info", sp_krudd_asset_info, 1, 0,
			   false, "(krudd-asset-info id) -> info tuple or #f");
	s7_define_function(sc, "krudd-asset-describe", sp_krudd_asset_describe,
			   1, 0, false, "(krudd-asset-describe id) -> ((key . value) ...)");
	s7_define_function(sc, "krudd-asset-data", sp_krudd_asset_data, 1, 0,
			   false, "(krudd-asset-data id) -> bytes as a string");
	s7_define_function(sc, "krudd-asset-shader-ref",
			   sp_krudd_asset_shader_ref, 1, 0, false,
			   "(krudd-asset-shader-ref id) -> shader asset id or 0");
	s7_define_function(sc, "krudd-shader-material-params",
			   sp_krudd_shader_material_params, 1, 0, false,
			   "(krudd-shader-material-params shader-ref) -> "
			   "((name type off size comps kind min max) ...)");
	s7_define_function(sc, "krudd-material-values",
			   sp_krudd_material_values, 2, 0, false,
			   "(krudd-material-values mat-id shader-ref) -> "
			   "(component-list ...)");
	s7_define_function(sc, "krudd-script-params",
			   sp_krudd_script_params, 1, 0, false,
			   "(krudd-script-params script-ref) -> "
			   "((name type off size comps kind min max) ...)");
	s7_define_function(sc, "krudd-entity-script-values",
			   sp_krudd_entity_script_values, 2, 0, false,
			   "(krudd-entity-script-values entity-id script-ref) -> "
			   "(component-list ...)");
	s7_define_function(sc, "krudd-entity-save-script-params",
			   sp_krudd_entity_save_script_params, 3, 0, false,
			   "(krudd-entity-save-script-params id script-ref values)");
	s7_define_function(sc, "krudd-entity-material-values",
			   sp_krudd_entity_material_values, 3, 0, false,
			   "(krudd-entity-material-values id mat-id shader-ref) -> "
			   "(component-list ...)");
	s7_define_function(sc, "krudd-entity-save-material-params",
			   sp_krudd_entity_save_material_params, 3, 0, false,
			   "(krudd-entity-save-material-params id shader-ref values)");
	s7_define_function(sc, "krudd-mesh-params",
			   sp_krudd_mesh_params, 1, 0, false,
			   "(krudd-mesh-params mesh-ref) -> "
			   "((name type off size comps kind min max) ...)");
	s7_define_function(sc, "krudd-entity-mesh-values",
			   sp_krudd_entity_mesh_values, 2, 0, false,
			   "(krudd-entity-mesh-values entity-id mesh-ref) -> "
			   "(component-list ...)");
	s7_define_function(sc, "krudd-entity-save-mesh-params",
			   sp_krudd_entity_save_mesh_params, 3, 0, false,
			   "(krudd-entity-save-mesh-params id mesh-ref values)");
	s7_define_function(sc, "krudd-texture-params", sp_krudd_texture_params,
			   1, 0, false,
			   "(krudd-texture-params texture-ref) -> "
			   "((name type off size comps kind min max) ...)");
	s7_define_function(sc, "krudd-entity-texture-values",
			   sp_krudd_entity_texture_values, 2, 0, false,
			   "(krudd-entity-texture-values entity-id texture-ref) -> "
			   "(component-list ...)");
	s7_define_function(sc, "krudd-entity-save-texture-params",
			   sp_krudd_entity_save_texture_params, 3, 0, false,
			   "(krudd-entity-save-texture-params id texture-ref values)");
	s7_define_function(sc, "krudd-texture-values", sp_krudd_texture_values,
			   1, 0, false,
			   "(krudd-texture-values texture-ref) -> "
			   "(component-list ...) of declared defaults");
	s7_define_function(sc, "krudd-texture-preview", sp_krudd_texture_preview,
			   3, 0, false,
			   "(krudd-texture-preview texture-ref values res) draw a "
			   "live bake as an inline image");
	s7_define_function(sc, "krudd-mesh-preview", sp_krudd_mesh_preview,
			   3, 0, false,
			   "(krudd-mesh-preview mesh-ref material-ref res) draw a "
			   "shaded offscreen render as an inline image");
	s7_define_function(sc, "krudd-entity-script-overrides",
			   sp_krudd_entity_script_overrides, 2, 0, false,
			   "(krudd-entity-script-overrides id script-ref) -> "
			   "(overridden? ...)");
	s7_define_function(sc, "krudd-entity-material-overrides",
			   sp_krudd_entity_material_overrides, 3, 0, false,
			   "(krudd-entity-material-overrides id mat shader) -> "
			   "(bool ...)");
	s7_define_function(sc, "krudd-entity-mesh-overrides",
			   sp_krudd_entity_mesh_overrides, 2, 0, false,
			   "(krudd-entity-mesh-overrides id mesh-ref) -> "
			   "(overridden? ...)");
	s7_define_function(sc, "krudd-shader-stages", sp_krudd_shader_stages, 1,
			   0, false, "(krudd-shader-stages src) -> stage list string");
	s7_define_function(sc, "krudd-asset-save-text", sp_krudd_asset_save_text,
			   2, 0, false, "(krudd-asset-save-text id text)");
	s7_define_function(sc, "krudd-asset-save-shader",
			   sp_krudd_asset_save_shader, 2, 0, false,
			   "(krudd-asset-save-shader id text) -> compiled-ok?");
	s7_define_function(sc, "krudd-asset-save-material",
			   sp_krudd_asset_save_material, 3, 3, false,
			   "(krudd-asset-save-material id shader-ref values "
			   "[tex-ref w h])");
	s7_define_function(sc, "krudd-asset-delete", sp_krudd_asset_delete, 1,
			   0, false, "(krudd-asset-delete id)");
	s7_define_function(sc, "krudd-asset-create-text",
			   sp_krudd_asset_create_text, 1, 0, false,
			   "(krudd-asset-create-text path) -> new id or 0");
	s7_define_function(sc, "krudd-asset-create-shader",
			   sp_krudd_asset_create_shader, 1, 0, false,
			   "(krudd-asset-create-shader path) -> new id or 0");
	s7_define_function(sc, "krudd-asset-create-material",
			   sp_krudd_asset_create_material, 1, 0, false,
			   "(krudd-asset-create-material path) -> new id or 0");
	s7_define_function(sc, "krudd-asset-clone-shader",
			   sp_krudd_asset_clone_shader, 2, 0, false,
			   "(krudd-asset-clone-shader name text) -> new id or 0");
	s7_define_function(sc, "krudd-script-hooks", sp_krudd_script_hooks, 1,
			   0, false, "(krudd-script-hooks src) -> hook list string");
	s7_define_function(sc, "krudd-asset-save-script",
			   sp_krudd_asset_save_script, 2, 0, false,
			   "(krudd-asset-save-script id text) -> saved-ok?");
	s7_define_function(sc, "krudd-asset-create-script",
			   sp_krudd_asset_create_script, 1, 0, false,
			   "(krudd-asset-create-script path) -> new id or 0");
	s7_define_function(sc, "krudd-asset-clone-script",
			   sp_krudd_asset_clone_script, 2, 0, false,
			   "(krudd-asset-clone-script name text) -> new id or 0");
	s7_define_function(sc, "krudd-asset-save-mesh",
			   sp_krudd_asset_save_mesh, 2, 0, false,
			   "(krudd-asset-save-mesh id text) -> #t/#f");
	s7_define_function(sc, "krudd-asset-create-mesh",
			   sp_krudd_asset_create_mesh, 1, 0, false,
			   "(krudd-asset-create-mesh path) -> new id or 0");
	s7_define_function(sc, "krudd-asset-clone-mesh",
			   sp_krudd_asset_clone_mesh, 2, 0, false,
			   "(krudd-asset-clone-mesh name text) -> new id or 0");
	s7_define_function(sc, "krudd-asset-clone-material",
			   sp_krudd_asset_clone_material, 3, 3, false,
			   "(krudd-asset-clone-material name shader-ref values "
			   "[tex-ref w h]) -> new id or 0");
	s7_define_function(sc, "krudd-md-preview", sp_krudd_md_preview, 2, 0,
			   false, "(krudd-md-preview text h) scrolling preview child");
	script_eval(KRUDDBOARD_SCM);
	script_eval(ASSETS_SCM);
	ready = true;
	return sc;
}

/*
 * Call a nullary panel procedure the image defines (e.g. kruddboard-draw-stats)
 * inside the current frame. Returns true if it ran, false if the interpreter
 * is down or the image never defined it, so the caller can fall back.
 */
static bool call_scm_panel(const char *proc)
{
	s7_scheme *sc = ensure_panel_scm();
	s7_pointer fn;

	if (!sc)
		return false;
	fn = s7_name_to_value(sc, proc);
	if (!s7_is_procedure(fn))
		return false;
	s7_call(sc, fn, s7_nil(sc));
	return true;
}
#endif /* __EMSCRIPTEN__ */

/* ------------------------------------------------------------------ */
/* Tab: Frame Stats                                                    */
/* ------------------------------------------------------------------ */

static void draw_tab_stats(void)
{
#ifdef __EMSCRIPTEN__
	/* Ported to Scheme (kruddboard.scm). Fall back only if the image
	 * can't run at all — an empty panel would read as "no stats". */
	if (call_scm_panel("kruddboard-draw-stats"))
		return;
	ImGui::TextDisabled("(stats unavailable)");
#else
	if (!g_stats) {
		ImGui::TextDisabled("(stats unavailable)");
		return;
	}
	ImGui::Text("FPS (avg): %.1f", (double)g_stats->fps_avg);
	ImGui::Text("Frame ms:  %.2f", (double)g_stats->last_frame_ms);
	ImGui::Text("Frame:     %u",   g_stats->frame_count);
#endif
}

/* ------------------------------------------------------------------ */
/* Tab: Log                                                            */
/* ------------------------------------------------------------------ */

static void draw_tab_log(void)
{
#ifdef __EMSCRIPTEN__
	/* Ported to Scheme (kruddboard.scm). Fall back only if the image can't
	 * run at all — an empty panel would read as "no log". */
	if (call_scm_panel("kruddboard-draw-log"))
		return;
	ImGui::TextDisabled("(log unavailable)");
#else
	static struct log_message msgs[LOG_HISTORY_CAP];
	static int   filter     = LOG_LEVEL_DEBUG;
	static bool  autoscroll = true;
	uint32_t     count;
	uint32_t     i;
	float        vp_h;
	float        scroll_h;

	if (!g_log) {
		ImGui::TextDisabled("(log unavailable)");
		return;
	}

	vp_h     = ImGui::GetMainViewport()->WorkSize.y;
	scroll_h = vp_h * 0.88f - 120.0f;
	if (scroll_h < 80.0f)
		scroll_h = 80.0f;

	count = g_log->get_history(msgs, LOG_HISTORY_CAP);

	if (ImGui::SmallButton("DEBUG")) filter = LOG_LEVEL_DEBUG;
	ImGui::SameLine();
	if (ImGui::SmallButton("INFO"))  filter = LOG_LEVEL_INFO;
	ImGui::SameLine();
	if (ImGui::SmallButton("WARN"))  filter = LOG_LEVEL_WARN;
	ImGui::SameLine();
	if (ImGui::SmallButton("ERROR")) filter = LOG_LEVEL_ERROR;
	ImGui::SameLine();
	ImGui::Checkbox("Auto-scroll", &autoscroll);

	ImGui::Separator();

	ImGui::BeginChild("##logscroll", ImVec2(0.0f, scroll_h),
			  false, ImGuiWindowFlags_HorizontalScrollbar);

	static const ImVec4 level_colors[] = {
		{ 0.6f, 0.6f, 0.6f, 1.0f }, /* DEBUG — grey   */
		{ 1.0f, 1.0f, 1.0f, 1.0f }, /* INFO  — white  */
		{ 1.0f, 0.8f, 0.2f, 1.0f }, /* WARN  — yellow */
		{ 1.0f, 0.3f, 0.3f, 1.0f }, /* ERROR — red    */
	};

	for (i = 0; i < count; i++) {
		if ((int)msgs[i].level < filter)
			continue;
		ImGui::TextColored(level_colors[msgs[i].level],
				   "%s", msgs[i].text);
	}

	if (autoscroll)
		ImGui::SetScrollHereY(1.0f);

	ImGui::EndChild();
#endif
}

/* ------------------------------------------------------------------ */
/* Tab: Subsystems                                                     */
/* ------------------------------------------------------------------ */

static void draw_tab_subsystems(void)
{
#ifdef __EMSCRIPTEN__
	/* Ported to Scheme (kruddboard.scm). Fall back only if the image can't
	 * run at all — an empty panel would read as "no subsystems". */
	if (call_scm_panel("kruddboard-draw-subsystems"))
		return;
	ImGui::TextDisabled("(subsystem manager unavailable)");
#else
	int i;

	if (!g_mgr) {
		ImGui::TextDisabled("(subsystem manager unavailable)");
		return;
	}

	if (ImGui::BeginTable("##subsys", 3,
			      ImGuiTableFlags_Borders        |
			      ImGuiTableFlags_RowBg          |
			      ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Name");
		ImGui::TableSetupColumn("API");
		ImGui::TableSetupColumn("Tick");
		ImGui::TableHeadersRow();

		for (i = 0; g_mgr->static_table[i].name; i++) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(g_mgr->static_table[i].name);
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(
				g_mgr->static_table[i].api  ? "yes" : "-");
			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(
				g_mgr->static_table[i].tick ? "yes" : "-");
		}

		for (i = 0; i < g_mgr->dynamic_count; i++) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(g_mgr->dynamic[i].name);
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(
				g_mgr->dynamic[i].api  ? "yes" : "-");
			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(
				g_mgr->dynamic[i].tick ? "yes" : "-");
		}

		ImGui::EndTable();
	}
#endif
}

/* ------------------------------------------------------------------ */
/* Tab: Assets                                                         */
/* ------------------------------------------------------------------ */

/*
 * Pre-port native fallback for the whole Assets tab (#402).  This library
 * only ever builds wasm-only (see build.scm), so __EMSCRIPTEN__ is always
 * defined in the one configuration that actually compiles; everything in
 * this #ifndef block is dead code kept for the same reason draw_tab_world's
 * native fallback was (#401) — a hypothetical native ImGui build would fall
 * through to this instead of the Scheme-driven browser path below.
 */
#ifndef __EMSCRIPTEN__
static uint32_t g_asset_sel; /* 0 = none */

static const char *asset_state_str(int32_t s)
{
	switch (s) {
	case 0:  return "Pending";
	case 1:  return "Loaded";
	default: return "Error";
	}
}

static const char *asset_kind_str(int32_t k)
{
	if (k == ASSET_KIND_PRIMITIVE)
		return "Primitive";
	return "Normal";
}

static const char *asset_type_str(int32_t t)
{
	switch (t) {
	case ASSET_TYPE_MESH:     return "Mesh";
	case ASSET_TYPE_TEXTURE:  return "Texture";
	case ASSET_TYPE_MATERIAL: return "Material";
	case ASSET_TYPE_SHADER:   return "Shader";
	case ASSET_TYPE_FONT:     return "Font";
	case ASSET_TYPE_SCENE:    return "Scene";
	case ASSET_TYPE_TEXT:     return "Text";
	default:                  return "Unknown";
	}
}

/*
 * Markdown editor state.  Tracks which asset is loaded into the edit
 * buffer so we only reload on selection change, not every frame.
 */
#define EDIT_BUF_MAX (64 * 1024)
static char     g_edit[EDIT_BUF_MAX];
static uint32_t g_edit_id; /* id whose bytes are in g_edit; 0 = none */
static int      g_shader_compile_ok = -1; /* -1 = untried, 0 = fail, 1 = ok */

static struct md_block g_blocks[MD_BLOCKS_MAX];
static int32_t         g_nblocks;

/* Material color editor state — the RGBA base_color the fragment shader
 * multiplies in (see the Material uniform block in SCENE_SHADER_SRC). */
static float    g_material_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
static uint32_t g_material_color_id; /* id whose bytes are in g_material_color */

/*
 * (Re)load the color editor from the asset catalog when the selection
 * changes. A missing or short (pre-Save) asset falls back to white, same as
 * the renderer's resolve_material_color default.
 */
static void maybe_reload_material_color(uint32_t id)
{
	const void *src;
	uint32_t    sz = 0;

	if (g_material_color_id == id)
		return;
	g_material_color_id = id;
	g_material_color[0] = g_material_color[1] =
		g_material_color[2] = g_material_color[3] = 1.0f;
	if (!g_asset_api || !g_asset_api->get_data)
		return;
	src = g_asset_api->get_data(id, &sz);
	if (src && sz >= sizeof(g_material_color))
		memcpy(g_material_color, src, sizeof(g_material_color));
}

/*
 * (Re)load the edit buffer from the asset catalog when the selection
 * changes.  Clamps to EDIT_BUF_MAX - 1 bytes and NUL-terminates.
 */
static void maybe_reload_edit(uint32_t id)
{
	const void *src;
	uint32_t    sz;

	if (g_edit_id == id)
		return;
	g_edit_id  = id;
	g_edit[0]  = '\0';
	g_nblocks  = 0;
	g_shader_compile_ok = -1;
	if (!g_asset_api || !g_asset_api->get_data)
		return;
	src = g_asset_api->get_data(id, &sz);
	if (!src)
		return;
	if (sz >= (uint32_t)EDIT_BUF_MAX)
		sz = (uint32_t)EDIT_BUF_MAX - 1;
	memcpy(g_edit, src, (size_t)sz);
	g_edit[sz] = '\0';
	g_nblocks  = md_parse(g_edit, g_blocks, MD_BLOCKS_MAX);
}

static void draw_asset_inspector(uint32_t id)
{
	struct asset_info       info;
	struct asset_decl_field fields[16];
	uint32_t                nf;
	uint32_t                i;
	uint32_t                idx;
	uint32_t                n;
	struct asset_info       tmp;
	char                    buf[32];

	if (g_asset_api->find(id, &info) != 0) {
		g_asset_sel = 0;
		return;
	}

	if (ImGui::SmallButton("<- Back"))
		g_asset_sel = 0;
	ImGui::SameLine();
	ImGui::TextUnformatted(info.path);
	ImGui::SameLine();
	ImGui::TextDisabled("[%s | %s | %s]",
		asset_type_str(info.type),
		asset_state_str(info.state),
		info.read_only ? "read-only" : "mutable");

#ifdef __EMSCRIPTEN__
	/*
	 * Authored text assets get the markdown editor/viewer.
	 * All other assets fall through to the read-only inspector tables.
	 */
	if (info.origin == ASSET_ORIGIN_AUTHORED &&
	    info.type   == ASSET_TYPE_TEXT) {
		int    can_persist;
		size_t edit_len;

		can_persist = g_backend &&
			(g_backend->get_caps() &
			 BACKEND_CAP_PROJECT_PERSIST);

		maybe_reload_edit(id);

		ImGui::Separator();

		/* -- Source editor -- */
		if (ImGui::CollapsingHeader("Source",
					    ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::InputTextMultiline(
				    "##md", g_edit, (size_t)EDIT_BUF_MAX,
				    ImVec2(-1.0f, 200.0f))) {
				/* Reparse on every edit keystroke. */
				g_nblocks = md_parse(g_edit, g_blocks,
						     MD_BLOCKS_MAX);
			}
		}

		ImGui::Separator();

		/* -- Rendered preview -- */
		if (ImGui::CollapsingHeader("Preview",
					    ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::BeginChild(
				"##mdpreview", ImVec2(0.0f, 200.0f),
				true, ImGuiWindowFlags_HorizontalScrollbar);
			md_draw_blocks(g_blocks, g_nblocks);
			ImGui::EndChild();
		}

		ImGui::Separator();

		/* -- Action buttons -- */
		edit_len = strlen(g_edit);
		/*
		 * Save always writes the in-memory asset, so this session sees
		 * the change even with no backend to persist it to (mirrors
		 * the shader Save fix in #390).
		 */
		if (ImGui::Button("Save")) {
			if (g_asset_mut)
				g_asset_mut->set_data(
					id, g_edit,
					(uint32_t)edit_len);
			if (can_persist)
				g_backend->persist_asset(
					id, info.path,
					ASSET_TYPE_TEXT, g_edit,
					(uint32_t)edit_len);
		}

		ImGui::SameLine();

		if (ImGui::Button("Delete")) {
			if (g_asset_mut)
				g_asset_mut->destroy(id);
			if (g_backend && can_persist)
				g_backend->delete_asset(id);
			g_edit_id   = 0;
			g_edit[0]   = '\0';
			g_nblocks   = 0;
			g_asset_sel = 0;
		}

		return;
	}

	/*
	 * Shader assets — read-only built-ins and authored/project shaders
	 * alike — get the same screen: a source editor plus its declaration.
	 * A krudd shader source embeds every stage it defines, so there is no
	 * per-asset stage/dialect to pick; "Declaration" is a derived, read-
	 * only summary of what the source contains. The only difference
	 * between a built-in and an authored shader is whether the source box
	 * accepts edits and the Save button is live.
	 */
	if (info.type == ASSET_TYPE_SHADER) {
		bool   editable = !info.read_only;
		int    can_persist;
		size_t edit_len;

		can_persist = editable && g_backend &&
			(g_backend->get_caps() &
			 BACKEND_CAP_PROJECT_PERSIST);

		maybe_reload_edit(id);

		ImGui::Separator();

		/* -- Declaration (derived from the source; read-only) -- */
		if (ImGui::CollapsingHeader("Declaration",
					    ImGuiTreeNodeFlags_DefaultOpen)) {
			const char *stages = shader_stages_from_source(g_edit);

			ImGui::Text("format: %s", SHADER_FORMAT);
			ImGui::Text("stages: %s", stages[0] ? stages : "(none)");
		}

		ImGui::Separator();

		/* -- Source editor (plain; no markdown preview) -- */
		if (ImGui::CollapsingHeader("Source",
					    ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::InputTextMultiline(
				"##shader", g_edit, (size_t)EDIT_BUF_MAX,
				ImVec2(-1.0f, 260.0f),
				editable ? ImGuiInputTextFlags_None
					 : ImGuiInputTextFlags_ReadOnly);
		}

		ImGui::Separator();

		/* -- Action buttons -- */
		edit_len = strlen(g_edit);
		if (!editable) {
			/*
			 * Built-ins can't be saved in place, but they make a good
			 * starting point: name a project shader and clone this
			 * source into it, then land straight in its (editable)
			 * inspector.
			 */
			static uint32_t clone_src_id;
			static char     clone_name[256];
			static int      clone_conflict;
			bool            confirm;

			if (clone_src_id != id) {
				clone_src_id = id;
				snprintf(clone_name, sizeof(clone_name),
					 "%s_copy", info.path);
				clone_conflict = 0;
			}

			ImGui::SetNextItemWidth(240.0f);
			confirm = ImGui::InputText(
				"##clonename", clone_name, sizeof(clone_name),
				ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::SameLine();
			confirm |= ImGui::Button("Clone");

			if (confirm && clone_name[0] != '\0') {
				uint32_t nid = g_asset_mut ?
					g_asset_mut->create(
						clone_name, ASSET_TYPE_SHADER,
						g_edit, (uint32_t)edit_len) : 0;

				if (nid == 0) {
					clone_conflict = 1;
				} else {
					struct asset_decl_field decl[2];

					decl[0].key   = "format";
					decl[0].value = SHADER_FORMAT;
					decl[1].key   = "stages";
					decl[1].value =
						shader_stages_from_source(g_edit);
					if (g_asset_mut->set_decl)
						g_asset_mut->set_decl(
							nid, decl, 2);

					if (g_backend &&
					    (g_backend->get_caps() &
					     BACKEND_CAP_PROJECT_PERSIST))
						g_backend->persist_asset(
							nid, clone_name,
							ASSET_TYPE_SHADER,
							g_edit,
							(uint32_t)edit_len);

					clone_conflict = 0;
					g_asset_sel    = nid;
				}
			}
			if (clone_conflict) {
				ImGui::SameLine();
				ImGui::TextColored(
					ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
					"\"%s\" already exists", clone_name);
			}
		} else {
			/*
			 * Save always writes the in-memory asset, so this
			 * session sees the change even with no backend to
			 * persist it to. A failed compile keeps whatever was
			 * last committed (the seeded default, for a brand new
			 * shader) instead of pushing broken source live.
			 */
			if (ImGui::Button("Save")) {
				if (shader_compiles(g_edit)) {
					struct asset_decl_field decl[2];

					decl[0].key   = "format";
					decl[0].value = SHADER_FORMAT;
					decl[1].key   = "stages";
					decl[1].value =
						shader_stages_from_source(g_edit);

					if (g_asset_mut) {
						g_asset_mut->set_data(
							id, g_edit,
							(uint32_t)edit_len);
						if (g_asset_mut->set_decl)
							g_asset_mut->set_decl(
								id, decl, 2);
					}
					if (can_persist)
						g_backend->persist_asset(
							id, info.path,
							ASSET_TYPE_SHADER, g_edit,
							(uint32_t)edit_len);
					g_shader_compile_ok = 1;
				} else {
					g_shader_compile_ok = 0;
				}
			}
			ImGui::SameLine();
			if (g_shader_compile_ok == 1)
				ImGui::TextColored(
					ImVec4(0.3f, 0.9f, 0.3f, 1.0f),
					"Compiled OK");
			else if (g_shader_compile_ok == 0)
				ImGui::TextColored(
					ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
					"Compile failed");
		}

		if (editable) {
			ImGui::SameLine();
			if (ImGui::Button("Delete")) {
				if (g_asset_mut)
					g_asset_mut->destroy(id);
				if (g_backend && can_persist)
					g_backend->delete_asset(id);
				g_edit_id   = 0;
				g_edit[0]   = '\0';
				g_nblocks   = 0;
				g_asset_sel = 0;
			}
		}

		return;
	}

	/*
	 * Material assets — a single authored RGBA base_color, edited with a
	 * color picker rather than a text box. #materials v0: no fixed-
	 * function state, no textures, one parameter (see scene_renderer.c).
	 */
	if (info.type == ASSET_TYPE_MATERIAL) {
		bool editable    = !info.read_only;
		int  can_persist = editable && g_backend &&
			(g_backend->get_caps() & BACKEND_CAP_PROJECT_PERSIST);

		maybe_reload_material_color(id);

		ImGui::Separator();

		if (ImGui::CollapsingHeader("Color",
					    ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::BeginDisabled(!editable);
			ImGui::ColorEdit4("##basecolor", g_material_color);
			ImGui::EndDisabled();
		}

		ImGui::Separator();

		if (!editable) {
			ImGui::BeginDisabled();
			ImGui::Button("Save");
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextDisabled("read-only");
		} else {
			/*
			 * Save always writes the in-memory asset — the entity
			 * inspector's material combo and the renderer both read
			 * live catalog bytes — so the edit is visible this
			 * session even with no backend to persist it to (mirrors
			 * the shader Save fix in #390).
			 */
			if (ImGui::Button("Save")) {
				if (g_asset_mut)
					g_asset_mut->set_data(
						id, g_material_color,
						(uint32_t)sizeof(g_material_color));
				if (can_persist)
					g_backend->persist_asset(
						id, info.path, ASSET_TYPE_MATERIAL,
						g_material_color,
						(uint32_t)sizeof(g_material_color));
			}
		}

		if (editable) {
			ImGui::SameLine();
			if (ImGui::Button("Delete")) {
				if (g_asset_mut)
					g_asset_mut->destroy(id);
				if (g_backend && can_persist)
					g_backend->delete_asset(id);
				g_material_color_id = 0;
				g_asset_sel          = 0;
			}
		}

		return;
	}
#endif /* __EMSCRIPTEN__ */

	ImGui::Separator();
	ImGui::TextUnformatted("Declaration");

	/* Locate the index for describe(), which is index-addressed. */
	n   = g_asset_api->count();
	idx = n; /* sentinel: not found */
	for (i = 0; i < n; i++) {
		if (g_asset_api->info(i, &tmp) == 0 && tmp.id == id) {
			idx = i;
			break;
		}
	}
	nf = (idx < n) ? g_asset_api->describe(idx, fields, 16) : 0;
	if (nf == 0) {
		ImGui::TextDisabled("(no declaration)");
	} else if (ImGui::BeginTable("##decl", 2,
				     ImGuiTableFlags_Borders |
				     ImGuiTableFlags_RowBg   |
				     ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Property");
		ImGui::TableSetupColumn("Value");
		ImGui::TableHeadersRow();
		for (i = 0; i < nf; i++) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(fields[i].key);
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(fields[i].value);
		}
		ImGui::EndTable();
	}

	ImGui::Separator();
	ImGui::TextUnformatted("Catalog");

	if (!ImGui::BeginTable("##catalog", 2,
			       ImGuiTableFlags_Borders |
			       ImGuiTableFlags_RowBg   |
			       ImGuiTableFlags_SizingStretchProp))
		return;

	ImGui::TableSetupColumn("Field");
	ImGui::TableSetupColumn("Value");
	ImGui::TableHeadersRow();

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::TextUnformatted("path");
	ImGui::TableSetColumnIndex(1);
	ImGui::TextUnformatted(info.path);

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::TextUnformatted("kind");
	ImGui::TableSetColumnIndex(1);
	ImGui::TextUnformatted(asset_kind_str(info.kind));

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::TextUnformatted("type");
	ImGui::TableSetColumnIndex(1);
	ImGui::TextUnformatted(asset_type_str(info.type));

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::TextUnformatted("state");
	ImGui::TableSetColumnIndex(1);
	ImGui::TextUnformatted(asset_state_str(info.state));

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::TextUnformatted("size");
	ImGui::TableSetColumnIndex(1);
	if (info.size) {
		snprintf(buf, sizeof(buf), "%u", info.size);
		ImGui::TextUnformatted(buf);
	} else {
		ImGui::TextDisabled("-");
	}

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::TextUnformatted("refs");
	ImGui::TableSetColumnIndex(1);
	snprintf(buf, sizeof(buf), "%d", info.refs);
	ImGui::TextUnformatted(buf);

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::TextUnformatted("read_only");
	ImGui::TableSetColumnIndex(1);
	ImGui::TextUnformatted(info.read_only ? "yes" : "no");

	ImGui::EndTable();
}

static void draw_tab_assets_native(void)
{
	uint32_t          n;
	uint32_t          i;
	struct asset_info info;
	char              size_buf[32];
	int               has_builtin = 0;
	int               has_project = 0;

	if (!g_asset_api) {
		ImGui::TextDisabled("(assets unavailable)");
		return;
	}

#ifdef __EMSCRIPTEN__
	/*
	 * New Asset button — only shown in the list view, not the inspector.
	 * A small form collects just the name and the asset type (Text /
	 * Shader); a krudd shader's stages live inside its DSL source, not in
	 * a per-asset field, so there is nothing else to ask for here — the
	 * new asset opens straight into the same editor screen used to view
	 * one, ready for source to be typed in and saved.
	 */
	if (g_asset_sel == 0 && g_asset_mut) {
		static char new_name[256];
		static int  naming;   /* 1 while the form is visible */
		static int  new_type; /* 0 = Text, 1 = Shader, 2 = Material */

		static const char *const NEW_TYPES[] =
			{ "Text", "Shader", "Material" };

		if (!naming) {
			if (ImGui::Button("New Asset")) {
				naming      = 1;
				new_name[0] = '\0';
				new_type    = 0;
			}
		} else {
			ImGui::SetNextItemWidth(240.0f);
			bool confirm = ImGui::InputText(
				"name", new_name, sizeof(new_name),
				ImGuiInputTextFlags_EnterReturnsTrue);

			ImGui::SetNextItemWidth(160.0f);
			ImGui::Combo("type", &new_type, NEW_TYPES, 3);

			confirm |= ImGui::Button("Create");
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				naming = 0;

			if (confirm && new_name[0] != '\0') {
				static const float DEFAULT_COLOR[4] =
					{ 1.0f, 1.0f, 1.0f, 1.0f };
				char        path[256];
				int32_t     atype;
				uint32_t    nid;
				int         can_persist;
				const void *bytes;
				uint32_t    size;

				atype = (new_type == 2) ? ASSET_TYPE_MATERIAL
					: (new_type == 1) ? ASSET_TYPE_SHADER
							  : ASSET_TYPE_TEXT;

				/*
				 * A material's bytes are its base_color vec4;
				 * a new shader seeds from the built-in scene
				 * shader so it starts from working source;
				 * text still authors as empty.
				 */
				if (atype == ASSET_TYPE_MATERIAL) {
					bytes = DEFAULT_COLOR;
					size  = (uint32_t)sizeof(DEFAULT_COLOR);
				} else if (atype == ASSET_TYPE_SHADER) {
					const char *seed = default_shader_src();

					bytes = seed;
					size  = (uint32_t)strlen(seed);
				} else {
					bytes = "";
					size  = 0;
				}

				snprintf(path, sizeof(path), "%s", new_name);

				nid = g_asset_mut->create(path, atype, bytes, size);
				if (nid != 0) {
					can_persist =
						g_backend &&
						(g_backend->get_caps() &
						 BACKEND_CAP_PROJECT_PERSIST);
					if (can_persist)
						g_backend->persist_asset(
							nid, path, atype,
							bytes, size);
					g_asset_sel = nid;
				}
				naming = 0;
			}
		}

		ImGui::Separator();
	}
#endif /* __EMSCRIPTEN__ */

	n = g_asset_api->count();
	if (n == 0) {
		ImGui::TextDisabled("(no assets)");
		return;
	}

	if (g_asset_sel != 0) {
		draw_asset_inspector(g_asset_sel);
		return;
	}

	/* Pre-scan to know which groups are present. */
	for (i = 0; i < n; i++) {
		if (g_asset_api->info(i, &info) != 0)
			continue;
		if (info.read_only)
			has_builtin = 1;
		else
			has_project = 1;
	}

	if (!ImGui::BeginTable("##assets", 6,
			       ImGuiTableFlags_Borders        |
			       ImGuiTableFlags_RowBg          |
			       ImGuiTableFlags_SizingStretchProp))
		return;

	ImGui::TableSetupColumn("Path");
	ImGui::TableSetupColumn("Type");
	ImGui::TableSetupColumn("Kind");
	ImGui::TableSetupColumn("State");
	ImGui::TableSetupColumn("Size");
	ImGui::TableSetupColumn("Flags");
	ImGui::TableHeadersRow();

	/* Group 1: built-in (read-only) primitives */
	if (has_builtin) {
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextDisabled("-- BUILT-IN (read-only) --");

		for (i = 0; i < n; i++) {
			if (g_asset_api->info(i, &info) != 0 ||
			    !info.read_only)
				continue;
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			if (ImGui::Selectable(
				    info.path, false,
				    ImGuiSelectableFlags_SpanAllColumns))
				g_asset_sel = info.id;
			/*
			 * Mesh rows are drag sources: the payload is the asset
			 * id, which becomes the spawned entity's render_ref
			 * (drag-to-spawn, #176).
			 */
			if (info.type == ASSET_TYPE_MESH &&
			    ImGui::BeginDragDropSource(
				    ImGuiDragDropFlags_None)) {
				ImGui::SetDragDropPayload("ASSET_ID", &info.id,
							  sizeof(info.id));
				ImGui::Text("Spawn %s", info.path);
				ImGui::EndDragDropSource();
			}
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(asset_type_str(info.type));
			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(asset_kind_str(info.kind));
			ImGui::TableSetColumnIndex(3);
			ImGui::TextUnformatted(asset_state_str(info.state));
			ImGui::TableSetColumnIndex(4);
			if (info.size) {
				snprintf(size_buf, sizeof(size_buf),
					 "%u", info.size);
				ImGui::TextUnformatted(size_buf);
			} else {
				ImGui::TextDisabled("-");
			}
			ImGui::TableSetColumnIndex(5);
			ImGui::TextColored(
				ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "RO");
		}
	}

	/* Group 2: project assets */
	if (has_project) {
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextDisabled("-- PROJECT --");

		for (i = 0; i < n; i++) {
			if (g_asset_api->info(i, &info) != 0 ||
			    info.read_only)
				continue;
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			if (ImGui::Selectable(
				    info.path, false,
				    ImGuiSelectableFlags_SpanAllColumns))
				g_asset_sel = info.id;
			/*
			 * Mesh rows are drag sources: the payload is the asset
			 * id, which becomes the spawned entity's render_ref
			 * (drag-to-spawn, #176).
			 */
			if (info.type == ASSET_TYPE_MESH &&
			    ImGui::BeginDragDropSource(
				    ImGuiDragDropFlags_None)) {
				ImGui::SetDragDropPayload("ASSET_ID", &info.id,
							  sizeof(info.id));
				ImGui::Text("Spawn %s", info.path);
				ImGui::EndDragDropSource();
			}
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(asset_type_str(info.type));
			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(asset_kind_str(info.kind));
			ImGui::TableSetColumnIndex(3);
			ImGui::TextUnformatted(asset_state_str(info.state));
			ImGui::TableSetColumnIndex(4);
			if (info.size) {
				snprintf(size_buf, sizeof(size_buf),
					 "%u", info.size);
				ImGui::TextUnformatted(size_buf);
			} else {
				ImGui::TextDisabled("-");
			}
			ImGui::TableSetColumnIndex(5);
			ImGui::TextDisabled("-");
		}
	}

	ImGui::EndTable();
}
#endif /* !__EMSCRIPTEN__ — native Assets-tab fallback (#402) */

static void draw_tab_assets(void)
{
#ifdef __EMSCRIPTEN__
	/* Ported to Scheme (tabs/Assets.scm). Fall back only if the image
	 * can't run at all — an empty panel would read as "no assets". */
	if (call_scm_panel("kruddboard-draw-assets"))
		return;
	ImGui::TextDisabled("(assets unavailable)");
#else
	draw_tab_assets_native();
#endif
}

/* ------------------------------------------------------------------ */
/* Tab: KRUDD — frame stats, subsystems, log                          */
/* ------------------------------------------------------------------ */

static void draw_tab_krudd(void)
{
#ifdef __EMSCRIPTEN__
	/*
	 * The whole tab — the three sections and their headers — is composed in
	 * Scheme (kruddboard-draw-krudd), whose (imgui-collapsing-header ...)
	 * calls render red instead of the native blue (see
	 * sp_imgui_collapsing_header) — that's the tab's Scheme-driven marker.
	 * Fall back to the C composition below only if the image can't run at
	 * all.
	 */
	if (call_scm_panel("kruddboard-draw-krudd"))
		return;
#endif
	if (ImGui::CollapsingHeader("Frame Stats",
				    ImGuiTreeNodeFlags_DefaultOpen))
		draw_tab_stats();

	if (ImGui::CollapsingHeader("Subsystems"))
		draw_tab_subsystems();

	if (ImGui::CollapsingHeader("Log",
				    ImGuiTreeNodeFlags_DefaultOpen))
		draw_tab_log();
}

/* ------------------------------------------------------------------ */
/* Tab: World — entity list, create/delete, inspector                 */
/* ------------------------------------------------------------------ */

#ifndef __EMSCRIPTEN__
/*
 * The inspector rows below are the native fallback for draw_tab_world; the live
 * (WASM) path draws them from Scheme (kruddboard.scm). kruddboard compiles only
 * for WASM, so this block never builds — it is kept as the established fallback
 * convention (see the #else in draw_tab_world).
 *
 * Read-only identity / hierarchy / component summary for the selected entity.
 * The world exposes more per-entity state than the editable name+transform —
 * the dense id, the parent link, and the component mask — so surface it here.
 */
static void draw_inspector_details(const struct world *w, uint32_t e)
{
	char comps[64];
	char parent_buf[64];

	snprintf(comps, sizeof(comps), "Transform%s%s%s",
		 (w->mask[e] & COMPONENT_NAME)     ? ", Name"     : "",
		 (w->mask[e] & COMPONENT_RENDER)   ? ", Render"   : "",
		 (w->mask[e] & COMPONENT_MATERIAL) ? ", Material" : "");

	if (w->parent[e] < 0) {
		snprintf(parent_buf, sizeof(parent_buf), "(root)");
	} else {
		uint32_t    p  = (uint32_t)w->parent[e];
		const char *pn = NULL;

		if ((w->mask[p] & COMPONENT_NAME) &&
		    w->name_off[p] != SCENE_NO_NAME)
			pn = w->names + w->name_off[p];
		if (pn)
			snprintf(parent_buf, sizeof(parent_buf), "%s (#%u)",
				 pn, p);
		else
			snprintf(parent_buf, sizeof(parent_buf), "entity %u", p);
	}

	if (!ImGui::BeginTable("##edetails", 2,
			       ImGuiTableFlags_SizingStretchProp))
		return;
	ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80.0f);
	ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::TextUnformatted("Entity ID");
	ImGui::TableSetColumnIndex(1);
	ImGui::Text("%u", e);

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::TextUnformatted("Parent");
	ImGui::TableSetColumnIndex(1);
	ImGui::TextUnformatted(parent_buf);

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::TextUnformatted("Components");
	ImGui::TableSetColumnIndex(1);
	ImGui::TextUnformatted(comps);

	ImGui::EndTable();
}

/*
 * Mesh binding row: shows the asset the entity's render_ref resolves to and a
 * dropdown to rebind it to any mesh asset (or "(none)" to unbind). Reads the
 * binding straight off render_ref[e] (valid iff COMPONENT_RENDER) and writes it
 * back through the scene api's set_render_ref, which records an undo step.
 */
static void draw_inspector_mesh(const struct world *w, uint32_t e)
{
	bool     has_render = (w->mask[e] & COMPONENT_RENDER) != 0;
	uint32_t cur_ref    = has_render ? w->render_ref[e] : 0u;
	char     cur_label[160];
	bool     can_edit;

	if (!has_render) {
		snprintf(cur_label, sizeof(cur_label), "(none)");
	} else {
		struct asset_info bi;

		if (g_asset_api && g_asset_api->find &&
		    g_asset_api->find(cur_ref, &bi) == 0)
			snprintf(cur_label, sizeof(cur_label), "%s", bi.path);
		else
			snprintf(cur_label, sizeof(cur_label),
				 "(missing #%u)", cur_ref);
	}

	can_edit = g_entity_api && g_entity_api->set_render_ref && g_asset_api;

	if (!ImGui::BeginTable("##emesh", 2, ImGuiTableFlags_SizingStretchProp))
		return;
	ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80.0f);
	ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::TextUnformatted("Mesh");
	ImGui::TableSetColumnIndex(1);
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::BeginDisabled(!can_edit);
	if (ImGui::BeginCombo("##meshsel", cur_label)) {
		uint32_t k, n;

		/* "(none)" unbinds — render_ref 0 clears COMPONENT_RENDER. */
		if (ImGui::Selectable("(none)", !has_render) && can_edit)
			g_entity_api->set_render_ref((int32_t)e, 0u);

		n = g_asset_api ? g_asset_api->count() : 0u;
		for (k = 0; k < n; k++) {
			struct asset_info mi;
			char              row[176];
			bool              is_cur;

			if (g_asset_api->info(k, &mi) != 0 ||
			    mi.type != ASSET_TYPE_MESH || mi.id == 0)
				continue;
			snprintf(row, sizeof(row), "%s##m%u", mi.path, mi.id);
			is_cur = has_render && mi.id == cur_ref;
			if (ImGui::Selectable(row, is_cur) && can_edit)
				g_entity_api->set_render_ref((int32_t)e, mi.id);
			if (is_cur)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();

	ImGui::EndTable();
}

/*
 * Material binding row: the entity-inspector counterpart of
 * draw_inspector_mesh, for the material asset that tints the entity's draw
 * (see the Material uniform block in SCENE_SHADER_SRC / scene_renderer.c).
 * Reads material_ref[e] (valid iff COMPONENT_MATERIAL) and writes it back
 * through the scene api's set_material_ref, which records an undo step.
 */
static void draw_inspector_material(const struct world *w, uint32_t e)
{
	bool     has_material = (w->mask[e] & COMPONENT_MATERIAL) != 0;
	uint32_t cur_ref      = has_material ? w->material_ref[e] : 0u;
	char     cur_label[160];
	bool     can_edit;

	if (!has_material) {
		snprintf(cur_label, sizeof(cur_label), "(none)");
	} else {
		struct asset_info bi;

		if (g_asset_api && g_asset_api->find &&
		    g_asset_api->find(cur_ref, &bi) == 0)
			snprintf(cur_label, sizeof(cur_label), "%s", bi.path);
		else
			snprintf(cur_label, sizeof(cur_label),
				 "(missing #%u)", cur_ref);
	}

	can_edit = g_entity_api && g_entity_api->set_material_ref && g_asset_api;

	if (!ImGui::BeginTable("##ematerial", 2, ImGuiTableFlags_SizingStretchProp))
		return;
	ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80.0f);
	ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::TextUnformatted("Material");
	ImGui::TableSetColumnIndex(1);
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::BeginDisabled(!can_edit);
	if (ImGui::BeginCombo("##materialsel", cur_label)) {
		uint32_t k, n;

		/* "(none)" unbinds — material_ref 0 clears COMPONENT_MATERIAL. */
		if (ImGui::Selectable("(none)", !has_material) && can_edit)
			g_entity_api->set_material_ref((int32_t)e, 0u);

		n = g_asset_api ? g_asset_api->count() : 0u;
		for (k = 0; k < n; k++) {
			struct asset_info mi;
			char              row[176];
			bool              is_cur;

			if (g_asset_api->info(k, &mi) != 0 ||
			    mi.type != ASSET_TYPE_MATERIAL || mi.id == 0)
				continue;
			snprintf(row, sizeof(row), "%s##m%u", mi.path, mi.id);
			is_cur = has_material && mi.id == cur_ref;
			if (ImGui::Selectable(row, is_cur) && can_edit)
				g_entity_api->set_material_ref((int32_t)e, mi.id);
			if (is_cur)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();

	ImGui::EndTable();
}
#endif /* !__EMSCRIPTEN__ — native inspector fallback */

/* ------------------------------------------------------------------ */
/* Transform gizmo (#178)                                              */
/* ------------------------------------------------------------------ */

/*
 * Hand-rolled against our own camera matrices rather than pulling in ImGuizmo:
 * the engine already exposes view·projection (#171) and a mutable transform API
 * (#173), and driving handles off the ImGui draw list is a precedent already
 * set elsewhere in this file.  A dependency would only wrap the same three
 * primitives (project, hit-test, write-back) we already have the pieces for.
 *
 * Handles are drawn on the background draw list so they sit over the rendered
 * 3D scene but under the editor overlay.  All geometry works in world space and
 * projects through the live camera, so the axes track it for free.
 */

#define GIZMO_AXIS_NONE (-1)

static int32_t          g_gizmo_axis = GIZMO_AXIS_NONE; /* dragging axis, or -1 */
static struct transform g_gizmo_start;                  /* local xform at grab */
static ImVec2           g_gizmo_grab;                   /* mouse pos at grab */
static bool             g_gizmo_gesture;                /* edit begin/commit open */

static const ImU32 GIZMO_AXIS_COL[3] = {
	IM_COL32(230,  70,  70, 255),  /* X — red   */
	IM_COL32( 90, 210,  90, 255),  /* Y — green */
	IM_COL32( 90, 140, 240, 255),  /* Z — blue  */
};

/*
 * Project a world point through view_proj into ImGui display-space pixels.
 * view_proj is column-major (m[col*4+row]); disp is io.DisplaySize.  Returns
 * false when the point is at or behind the camera plane (w <= 0).
 */
static bool gizmo_project(const float vp[16], const float p[3],
			  ImVec2 disp, ImVec2 *out)
{
	float cx = vp[0]*p[0] + vp[4]*p[1] + vp[8]*p[2]  + vp[12];
	float cy = vp[1]*p[0] + vp[5]*p[1] + vp[9]*p[2]  + vp[13];
	float cw = vp[3]*p[0] + vp[7]*p[1] + vp[11]*p[2] + vp[15];

	if (cw <= 1e-4f)
		return false;
	out->x = (cx / cw * 0.5f + 0.5f) * disp.x;
	out->y = (1.0f - (cy / cw * 0.5f + 0.5f)) * disp.y;
	return true;
}

/* Shortest distance from point p to segment ab, in pixels. */
static float gizmo_seg_dist(ImVec2 p, ImVec2 a, ImVec2 b)
{
	float vx = b.x - a.x, vy = b.y - a.y;
	float wx = p.x - a.x, wy = p.y - a.y;
	float len2 = vx*vx + vy*vy;
	float t    = len2 > 1e-6f ? (wx*vx + wy*vy) / len2 : 0.0f;
	float dx, dy;

	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	dx = a.x + t*vx - p.x;
	dy = a.y + t*vy - p.y;
	return sqrtf(dx*dx + dy*dy);
}

/* Hamilton product o = a*b (xyzw), then re-normalize into out. */
static void gizmo_quat_mul(float o[4], const float a[4], const float b[4])
{
	float x = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
	float y = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
	float z = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
	float w = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
	float inv = 1.0f / sqrtf(x*x + y*y + z*z + w*w);

	o[0] = x*inv; o[1] = y*inv; o[2] = z*inv; o[3] = w*inv;
}

/* Push the in-progress local transform back through the mutable API. */
static void gizmo_apply(int32_t e, const struct transform *t)
{
	if (g_entity_api && g_entity_api->set_transform)
		g_entity_api->set_transform(e, t);
}

/*
 * World-space unit length that reads as a constant on-screen handle size:
 * scale the axis by its distance from the eye so far entities don't shrink to
 * nothing and near ones don't fill the view.
 */
static float gizmo_handle_len(const float eye[3], const float origin[3])
{
	float dx = origin[0] - eye[0];
	float dy = origin[1] - eye[1];
	float dz = origin[2] - eye[2];
	float d  = sqrtf(dx*dx + dy*dy + dz*dz);

	return d * 0.18f + 0.05f;
}

/*
 * Draw the selected entity's move/rotate/scale handles over the viewport and
 * process a drag on one of them.  Runs every frame the board is visible; draws
 * nothing (and grabs nothing) when the selection is empty — satisfying "no
 * gizmo when nothing is selected".
 *
 * Returns true when a handle is hot or a drag is in flight, i.e. when the
 * gizmo owns this frame's click, so the click-to-pick pass below stands down
 * instead of reselecting out from under a handle grab.
 */
static bool gizmo_update_and_draw(void)
{
	const struct world *w;
	ImGuiIO            &io = ImGui::GetIO();
	ImDrawList         *dl;
	struct mat4         vp;
	float               eye[3];
	float               origin[3];
	float               len;
	int32_t             sel;
	uint32_t            e;
	ImVec2              o2d;
	ImVec2              tip2d[3];
	int32_t             hot = GIZMO_AXIS_NONE;

	if (!g_camera_api || !g_entity_api)
		return false;

	sel = g_entity_api->get_selected();
	w   = g_entity_api->get_world();
	if (!w || sel < 0 || (uint32_t)sel >= w->count || !w->alive[(uint32_t)sel])
		return false;
	e = (uint32_t)sel;

	/* Anchor at the entity's world origin; write edits to its local xform. */
	origin[0] = w->world_xform[e].position[0];
	origin[1] = w->world_xform[e].position[1];
	origin[2] = w->world_xform[e].position[2];

	g_camera_api->get_view_proj(&vp);
	g_camera_api->get_eye(eye);
	len = gizmo_handle_len(eye, origin);

	if (!gizmo_project(vp.m, origin, io.DisplaySize, &o2d))
		return false; /* entity behind the camera */

	/* Project each axis tip; bail an axis that clips behind the camera. */
	bool tip_ok[3];
	for (int a = 0; a < 3; a++) {
		float tip[3] = { origin[0], origin[1], origin[2] };

		tip[a] += len;
		tip_ok[a] = gizmo_project(vp.m, tip, io.DisplaySize, &tip2d[a]);
	}

	/*
	 * Hit-test against the nearest axis line — but only when the pointer is
	 * unobstructed, i.e. not over an ImGui window, or a drag is already in
	 * flight, so clicking the editor panel never grabs a handle.
	 */
	if ((!io.WantCaptureMouse || g_gizmo_axis != GIZMO_AXIS_NONE)) {
		float best = 10.0f; /* px pick radius */

		for (int a = 0; a < 3; a++) {
			if (!tip_ok[a])
				continue;
			float d = gizmo_seg_dist(io.MousePos, o2d, tip2d[a]);
			if (d < best) {
				best = d;
				hot  = a;
			}
		}
	}

	/* Grab: begin a single-entry undo gesture and snapshot the start xform. */
	if (g_gizmo_axis == GIZMO_AXIS_NONE && hot != GIZMO_AXIS_NONE &&
	    ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
		static const char *const LABEL[3] = {
			"Move Entity", "Rotate Entity", "Scale Entity"
		};

		g_gizmo_axis   = hot;
		g_gizmo_start  = w->local[e];
		g_gizmo_grab   = io.MousePos;
		if (g_edit_api && g_edit_api->begin) {
			g_edit_api->begin(LABEL[g_gizmo_mode]);
			g_gizmo_gesture = true;
		}
	}

	/* Drag: map the pointer motion onto the grabbed axis and write it back. */
	if (g_gizmo_axis != GIZMO_AXIS_NONE &&
	    ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
		int             a  = g_gizmo_axis;
		struct transform t = g_gizmo_start;
		float           ax = tip2d[a].x - o2d.x;
		float           ay = tip2d[a].y - o2d.y;
		float           axis_px = sqrtf(ax*ax + ay*ay);
		/* Signed pointer travel along the axis's screen direction. */
		float           mx = io.MousePos.x - g_gizmo_grab.x;
		float           my = io.MousePos.y - g_gizmo_grab.y;
		float           along = axis_px > 1e-3f
					? (mx*ax + my*ay) / axis_px : 0.0f;

		if (g_gizmo_mode == GIZMO_MOVE) {
			/* along px * (world units per px along this axis). */
			float world = axis_px > 1e-3f
				      ? along * (len / axis_px) : 0.0f;

			t.position[a] = g_gizmo_start.position[a] + world;
		} else if (g_gizmo_mode == GIZMO_SCALE) {
			float s = g_gizmo_start.scale[a] + along * 0.01f;

			t.scale[a] = s < 0.01f ? 0.01f : s;
		} else { /* GIZMO_ROTATE */
			float axis[3] = { 0.0f, 0.0f, 0.0f };
			float ang     = along * 0.01f;
			float dq[4];

			axis[a] = 1.0f;
			dq[0] = axis[0] * sinf(ang * 0.5f);
			dq[1] = axis[1] * sinf(ang * 0.5f);
			dq[2] = axis[2] * sinf(ang * 0.5f);
			dq[3] = cosf(ang * 0.5f);
			gizmo_quat_mul(t.rotation, dq, g_gizmo_start.rotation);
		}
		gizmo_apply((int32_t)e, &t);
		hot = a; /* keep the grabbed axis highlighted while dragging */
	}

	/* Release: close the undo gesture so the whole drag is one entry. */
	if (g_gizmo_axis != GIZMO_AXIS_NONE &&
	    ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
		if (g_gizmo_gesture && g_edit_api && g_edit_api->commit)
			g_edit_api->commit();
		g_gizmo_gesture = false;
		g_gizmo_axis    = GIZMO_AXIS_NONE;
	}

	/* ---- Render the handles ---- */
	dl = ImGui::GetBackgroundDrawList();
	dl->AddCircleFilled(o2d, 4.0f, IM_COL32(230, 230, 235, 255));

	for (int a = 0; a < 3; a++) {
		ImU32 col = GIZMO_AXIS_COL[a];
		float th  = (a == hot) ? 4.0f : 2.5f;

		if (!tip_ok[a])
			continue;
		dl->AddLine(o2d, tip2d[a], col, th);

		if (g_gizmo_mode == GIZMO_MOVE) {
			dl->AddCircleFilled(tip2d[a], (a == hot) ? 7.0f : 5.0f,
					    col);
		} else if (g_gizmo_mode == GIZMO_SCALE) {
			float r = (a == hot) ? 6.0f : 4.5f;

			dl->AddRectFilled(ImVec2(tip2d[a].x - r, tip2d[a].y - r),
					  ImVec2(tip2d[a].x + r, tip2d[a].y + r),
					  col);
		} else { /* rotate: a ring at the tip */
			dl->AddCircle(tip2d[a], (a == hot) ? 8.0f : 6.0f, col,
				      0, (a == hot) ? 3.0f : 2.0f);
		}
	}

	return g_gizmo_axis != GIZMO_AXIS_NONE || hot != GIZMO_AXIS_NONE;
}

/* ------------------------------------------------------------------ */
/* Click-to-pick: raycast the pointer against entity meshes            */
/* ------------------------------------------------------------------ */

/*
 * Inverse of the gizmo's world->screen path: cast a ray from the clicked pixel
 * and return the live render entity whose mesh it strikes nearest the camera,
 * or -1 when the ray misses everything.  Reuses the same view*projection and
 * DisplaySize the gizmo projects with, so a click lands on exactly the pixels
 * a mesh was drawn to.  Brute force over every triangle of every render entity:
 * the world caps at WORLD_MAX_ENTITIES and the meshes are tiny, so no broad-phase
 * or per-mesh bounds are needed yet.
 *
 * A mesh asset stores (mesh ...) source, not a compiled blob, so each entity's
 * geometry is generated on demand through mesh_script_generate — the same call
 * the renderer uploads with, and crucially with this entity's mesh-param override
 * (world_mesh_params), so a resized box's hit-box matches the box that was drawn
 * rather than the un-parameterized default. Generated per click (not per frame),
 * and the param-less fast path is cached in the image, so the cost is bounded.
 */
static int32_t pick_entity_at(float sx, float sy)
{
	const struct world *w;
	ImGuiIO            &io = ImGui::GetIO();
	struct mat4         vp;
	float               origin[3];
	float               dir[3];
	int32_t             best = -1;
	float               best_t = FLT_MAX;
	uint32_t            e;

	if (!g_camera_api || !g_entity_api || !g_asset_api || !g_mem)
		return -1;
	w = g_entity_api->get_world();
	if (!w)
		return -1;

	g_camera_api->get_view_proj(&vp);
	if (ray_from_screen(&vp, sx, sy, io.DisplaySize.x, io.DisplaySize.y,
			    origin, dir) != 0)
		return -1;

	for (e = 0; e < w->count; e++) {
		struct mesh_blob         *blob;
		const struct mesh_vertex *vtx;
		const uint16_t           *idx;
		const char               *src;
		const uint8_t            *mp;
		uint32_t                  mplen = 0;
		struct mat4               model;
		uint32_t                  i;

		if (!w->alive[e] || !(w->mask[e] & COMPONENT_RENDER))
			continue;
		src = (const char *)g_asset_api->get_data(w->render_ref[e], NULL);
		if (!src)
			continue;
		mp   = world_mesh_params(w, e, &mplen);
		blob = mesh_script_generate(src, mp, mplen, g_mem, NULL);
		if (!blob)
			continue;

		mat4_from_transform(&model, &w->world_xform[e]);
		vtx = mesh_blob_vertices(blob);
		idx = mesh_blob_indices(blob);

		for (i = 0; i + 3 <= blob->index_count; i += 3) {
			float a[3], b[3], c[3];
			float t;

			mat4_transform_point(a, &model, vtx[idx[i]].position);
			mat4_transform_point(b, &model, vtx[idx[i + 1]].position);
			mat4_transform_point(c, &model, vtx[idx[i + 2]].position);
			if (ray_tri_intersect(origin, dir, a, b, c, &t) &&
			    t < best_t) {
				best_t = t;
				best   = (int32_t)e;
			}
		}
		g_mem->free(blob);
	}
	return best;
}

/*
 * Left-click over the bare viewport selects the entity under the pointer, or
 * clears the selection on a miss.  Stands down when the gizmo owns the click
 * (a handle grab) or the pointer is over an editor panel, so only clicks that
 * reach the 3D scene ever change the selection.
 *
 * Re-clicking the entity that is already selected doesn't change the
 * selection — instead it cycles the gizmo through Move -> Rotate -> Scale,
 * so repeated clicks step through the transform tools without a trip to
 * the toolbar.
 */
static void pick_update(bool gizmo_active)
{
	ImGuiIO &io = ImGui::GetIO();
	int32_t  hit;

	if (!g_entity_api)
		return;
	if (gizmo_active || io.WantCaptureMouse)
		return;
	if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		return;

	hit = pick_entity_at(io.MousePos.x, io.MousePos.y);
	if (hit != -1 && hit == g_entity_api->get_selected()) {
		g_gizmo_mode = (enum gizmo_mode)((g_gizmo_mode + 1) % 3);
		return;
	}
	g_entity_api->set_selected(hit);
}

#ifndef __EMSCRIPTEN__
/*
 * Move / Rotate / Scale selector — shared state with the viewport handles.
 * Native fallback only; the live tool chips are drawn from Scheme through
 * krudd-gizmo-mode / krudd-set-gizmo-mode.
 */
static void draw_gizmo_mode_chips(void)
{
	static const char *const NAME[3] = { "Move", "Rotate", "Scale" };
	int m;

	ImGui::TextUnformatted("Tool");
	ImGui::SameLine();
	for (m = 0; m < 3; m++) {
		bool active = (int)g_gizmo_mode == m;

		if (m > 0)
			ImGui::SameLine();
		if (active)
			ImGui::PushStyleColor(ImGuiCol_Button,
					      IM_COL32(70, 110, 170, 255));
		if (ImGui::SmallButton(NAME[m]))
			g_gizmo_mode = (enum gizmo_mode)m;
		if (active)
			ImGui::PopStyleColor();
	}
}
#endif /* !__EMSCRIPTEN__ — native gizmo-chip fallback */

static void draw_tab_world(void)
{
#ifdef __EMSCRIPTEN__
	/* Ported to Scheme (kruddboard.scm). Fall back only if the image can't
	 * run at all — an empty panel would read as "no world". */
	if (call_scm_panel("kruddboard-draw-world"))
		return;
	ImGui::TextDisabled("(world unavailable)");
#else
	const struct world     *w = NULL;
	const struct transform *t;
	const char             *ename;
	uint32_t                i;
	uint32_t                e;
	int32_t                 sel;
	float                   pos[3], rot[4], scl[3];
	char                    fallback[32];
	char                    name_buf[256];
	char                    sel_id[72];
	char                    del_id[16];

	if (g_entity_api)
		w = g_entity_api->get_world();

	/* Selection lives in the scene subsystem; fall back if it's absent. */
	sel = g_entity_api ? g_entity_api->get_selected() : g_entity_sel;

	/* ---- Scene header ---- */
	ImGui::TextUnformatted("Untitled Scene");
	ImGui::SameLine();
	ImGui::SetCursorPosX(ImGui::GetWindowWidth()
		- ImGui::CalcTextSize("Save As...").x
		- ImGui::GetStyle().FramePadding.x * 2.0f
		- ImGui::GetStyle().WindowPadding.x);
	ImGui::BeginDisabled();
	ImGui::SmallButton("Save As...");
	ImGui::EndDisabled();

	ImGui::Separator();

	/* ---- Entity list ---- */
	if (ImGui::CollapsingHeader("Entities",
				    ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::BeginDisabled(!g_entity_api);
		if (ImGui::SmallButton("+ Entity") && g_entity_api) {
			struct transform seed = {};  /* identity at origin */
			uint32_t box = asset_id_by_path("builtin://mesh/box");
			uint32_t material =
				asset_id_by_path("builtin://material/default");
			int32_t          id;

			seed.rotation[3] = 1.0f;
			seed.scale[0] = seed.scale[1] = seed.scale[2] = 1.0f;
			id = g_entity_api->create_entity(WORLD_NO_PARENT, &seed,
							 0u, box);
			if (id >= 0) {
				if (material && g_entity_api->set_material_ref)
					g_entity_api->set_material_ref(id,
									material);
				g_entity_api->set_name(id, "Entity");
				g_entity_api->set_selected(id);
				sel = id;
			}
		}
		ImGui::EndDisabled();

		if (!w || w->count == 0) {
			ImGui::TextDisabled("(no entities)");
		} else if (ImGui::BeginTable("##entlist", 2,
				ImGuiTableFlags_Borders        |
				ImGuiTableFlags_RowBg          |
				ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("Name",
				ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("",
				ImGuiTableColumnFlags_WidthFixed, 24.0f);

			for (i = 0; i < w->count; i++) {
				if (!w->alive[i])
					continue;
				ename = NULL;
				if ((w->mask[i] & COMPONENT_NAME) &&
				    w->name_off[i] != SCENE_NO_NAME)
					ename = w->names + w->name_off[i];
				if (!ename) {
					snprintf(fallback, sizeof(fallback),
						 "entity %u", i);
					ename = fallback;
				}
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				snprintf(sel_id, sizeof(sel_id),
					 "%s##e%u", ename, i);
				if (ImGui::Selectable(
					    sel_id,
					    (int32_t)i == sel,
					    ImGuiSelectableFlags_SpanAllColumns)) {
					if (g_entity_api)
						g_entity_api->set_selected(
							(int32_t)i);
					else
						g_entity_sel = (int32_t)i;
					sel = (int32_t)i;
				}
				ImGui::TableSetColumnIndex(1);
				snprintf(del_id, sizeof(del_id), "x##d%u", i);
				ImGui::BeginDisabled(!g_entity_api);
				if (ImGui::SmallButton(del_id) && g_entity_api)
					g_entity_api->destroy_entity((int32_t)i);
				ImGui::EndDisabled();
			}

			ImGui::EndTable();
		}
	}

	ImGui::Separator();

	draw_gizmo_mode_chips();

	ImGui::Separator();

	/* ---- Inspector ---- */
	if (ImGui::CollapsingHeader("Inspector",
				    ImGuiTreeNodeFlags_DefaultOpen)) {
		if (sel < 0 || !w ||
		    (uint32_t)sel >= w->count ||
		    !w->alive[(uint32_t)sel]) {
			ImGui::TextDisabled("(nothing selected)");
		} else {
			e     = (uint32_t)sel;
			t     = &w->local[e];
			ename = NULL;
			if ((w->mask[e] & COMPONENT_NAME) &&
			    w->name_off[e] != SCENE_NO_NAME)
				ename = w->names + w->name_off[e];
			snprintf(name_buf, sizeof(name_buf),
				 "%s", ename ? ename : "");

			pos[0] = t->position[0];
			pos[1] = t->position[1];
			pos[2] = t->position[2];
			rot[0] = t->rotation[0];
			rot[1] = t->rotation[1];
			rot[2] = t->rotation[2];
			rot[3] = t->rotation[3];
			scl[0] = t->scale[0];
			scl[1] = t->scale[1];
			scl[2] = t->scale[2];

			bool xform_changed = false;

			ImGui::BeginDisabled(!g_entity_api);

			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputText("##ename", name_buf,
					 sizeof(name_buf));
			/* Commit once, on focus loss — not every keystroke, so
			 * the append-only name blob doesn't churn. */
			if (ImGui::IsItemDeactivatedAfterEdit() && g_entity_api)
				g_entity_api->set_name((int32_t)e, name_buf);

			ImGui::Separator();

			if (ImGui::BeginTable("##xform", 2,
					ImGuiTableFlags_SizingStretchProp)) {
				ImGui::TableSetupColumn("",
					ImGuiTableColumnFlags_WidthFixed,
					64.0f);
				ImGui::TableSetupColumn("",
					ImGuiTableColumnFlags_WidthStretch);

				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted("Position");
				ImGui::TableSetColumnIndex(1);
				ImGui::SetNextItemWidth(-1.0f);
				xform_changed |=
					ImGui::InputFloat3("##pos", pos);

				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted("Rotation");
				ImGui::TableSetColumnIndex(1);
				ImGui::SetNextItemWidth(-1.0f);
				xform_changed |=
					ImGui::InputFloat4("##rot", rot);

				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted("Scale");
				ImGui::TableSetColumnIndex(1);
				ImGui::SetNextItemWidth(-1.0f);
				xform_changed |=
					ImGui::InputFloat3("##scl", scl);

				ImGui::EndTable();
			}

			ImGui::EndDisabled();

			/* Push edited fields back through the mutable API; the
			 * next tick's propagate refreshes world_xform. */
			if (xform_changed && g_entity_api) {
				struct transform nt;

				nt.position[0] = pos[0];
				nt.position[1] = pos[1];
				nt.position[2] = pos[2];
				nt.rotation[0] = rot[0];
				nt.rotation[1] = rot[1];
				nt.rotation[2] = rot[2];
				nt.rotation[3] = rot[3];
				nt.scale[0]    = scl[0];
				nt.scale[1]    = scl[1];
				nt.scale[2]    = scl[2];
				g_entity_api->set_transform((int32_t)e, &nt);
			}

			ImGui::Separator();

			draw_inspector_details(w, e);
			draw_inspector_mesh(w, e);
			draw_inspector_material(w, e);
		}
	}
#endif /* __EMSCRIPTEN__ */
}

/* ------------------------------------------------------------------ */
/* Main board window                                                   */
/* ------------------------------------------------------------------ */

/*
 * Editor-global Undo / Redo buttons for the board header. Greyed via
 * can_undo / can_redo and tooltipped with the next action's label
 * ("Undo Move Entity"). Nothing is drawn when the "edit" service is absent.
 */
static void draw_undo_redo(void)
{
	bool can_undo;
	bool can_redo;

	if (!g_edit_api)
		return;

	can_undo = g_edit_api->can_undo();
	can_redo = g_edit_api->can_redo();

	ImGui::SameLine();
	ImGui::BeginDisabled(!can_undo);
	if (ImGui::SmallButton("Undo"))
		g_edit_api->undo();
	ImGui::EndDisabled();
	if (can_undo && ImGui::IsItemHovered()) {
		const char *label = g_edit_api->undo_label();

		ImGui::SetTooltip("Undo %s", label ? label : "");
	}

	ImGui::SameLine();
	ImGui::BeginDisabled(!can_redo);
	if (ImGui::SmallButton("Redo"))
		g_edit_api->redo();
	ImGui::EndDisabled();
	if (can_redo && ImGui::IsItemHovered()) {
		const char *label = g_edit_api->redo_label();

		ImGui::SetTooltip("Redo %s", label ? label : "");
	}
}

/*
 * Simulation mode toggle for the board header: Playing (default) runs
 * world_tick + entity scripts every frame; Paused freezes the scene while
 * everything else (rendering, gizmo, undo/redo) keeps running. Color-tinted
 * (green while playing, amber while paused) so the state reads at a glance;
 * the label names the action a click takes, not the current state. Nothing
 * is drawn when the "scene" service doesn't support pausing.
 */
static void draw_sim_mode(void)
{
	bool paused;

	if (!g_entity_api || !g_entity_api->get_paused || !g_entity_api->set_paused)
		return;

	paused = g_entity_api->get_paused();

	ImGui::SameLine();
	ImGui::PushStyleColor(ImGuiCol_Button,
			      paused ? IM_COL32(170, 120, 40, 255)
				     : IM_COL32(60, 150, 60, 255));
	if (ImGui::SmallButton(paused ? "> Play" : "|| Pause"))
		g_entity_api->set_paused(!paused);
	ImGui::PopStyleColor();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(paused ? "Resume simulation" : "Pause simulation");
}

/*
 * Spawn a dragged mesh asset as a live entity: identity transform at the
 * origin, render_ref = the asset id (which sets COMPONENT_RENDER), then select
 * it. Placement is fixed for v1; cursor-raycast placement needs camera unproject
 * (#171) and is a noted follow-up. No-op if the scene api or asset is missing.
 */
static void spawn_asset_entity(uint32_t asset_id)
{
	struct transform t;
	int32_t          id;

	if (!g_entity_api || !g_entity_api->create_entity || asset_id == 0)
		return;

	memset(&t, 0, sizeof(t));
	t.rotation[3] = 1.0f;
	t.scale[0] = t.scale[1] = t.scale[2] = 1.0f;
	id = g_entity_api->create_entity(WORLD_NO_PARENT, &t, 0u, asset_id);
	if (id >= 0 && g_entity_api->set_selected)
		g_entity_api->set_selected(id);
}

/*
 * A viewport-sized invisible drop target for drag-to-spawn (#176). The 3D
 * viewport is not an ImGui window, so there is no natural drop target there;
 * this fullscreen window supplies one. It is submitted only while an ASSET_ID
 * drag is in flight (so it never captures the mouse otherwise) and before the
 * kruddboard overlay, so the overlay stays interactable on top.
 */
static void draw_spawn_drop_target(void)
{
	const ImGuiPayload *active = ImGui::GetDragDropPayload();
	ImGuiViewport      *vp;
	ImGuiWindowFlags    flags;

	if (!active || !active->IsDataType("ASSET_ID"))
		return;

	vp    = ImGui::GetMainViewport();
	flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
	      | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
	      | ImGuiWindowFlags_NoBackground
	      | ImGuiWindowFlags_NoBringToFrontOnFocus
	      | ImGuiWindowFlags_NoFocusOnAppearing
	      | ImGuiWindowFlags_NoNavFocus;

	ImGui::SetNextWindowPos(vp->Pos);
	ImGui::SetNextWindowSize(vp->Size);
	if (ImGui::Begin("##viewport_drop", nullptr, flags)) {
		ImGui::InvisibleButton("##viewport_drop_area", vp->Size);
		if (ImGui::BeginDragDropTarget()) {
			const ImGuiPayload *pl =
				ImGui::AcceptDragDropPayload("ASSET_ID");

			if (pl && pl->DataSize == (int)sizeof(uint32_t))
				spawn_asset_entity(
					*(const uint32_t *)pl->Data);
			ImGui::EndDragDropTarget();
		}
	}
	ImGui::End();
}

static void draw_board(void * /*userdata*/)
{
	float            vp_w;
	float            vp_h;
	float            win_w;
	float            hint_x;
	ImGuiWindowFlags flags;

	if (!g_visible)
		return;

	/*
	 * Keep the camera's projection aspect matched to the live canvas, then
	 * draw the selection's transform handles over the viewport.  Both run
	 * before the overlay so the gizmo sits under the editor window.
	 */
	if (g_camera_api && g_camera_api->set_viewport)
		g_camera_api->set_viewport(ImGui::GetIO().DisplaySize.x,
					   ImGui::GetIO().DisplaySize.y);
	pick_update(gizmo_update_and_draw());

	/* Behind the overlay: catches asset drops onto the viewport. */
	draw_spawn_drop_target();

	vp_w  = ImGui::GetMainViewport()->WorkSize.x;
	vp_h  = ImGui::GetMainViewport()->WorkSize.y;
	win_w = vp_w - 16.0f; /* 8 px margin each side */

	ImGui::SetNextWindowSizeConstraints(
		ImVec2(win_w, 0.0f), ImVec2(win_w, vp_h * 0.9f));
	ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.86f);

	flags = ImGuiWindowFlags_NoTitleBar
	      | ImGuiWindowFlags_NoMove
	      | ImGuiWindowFlags_NoScrollbar
	      | ImGuiWindowFlags_NoScrollWithMouse
	      | ImGuiWindowFlags_AlwaysAutoResize
	      | ImGuiWindowFlags_NoBringToFrontOnFocus;

	if (!ImGui::Begin("##kruddboard", nullptr, flags)) {
		ImGui::End();
		return;
	}

	if (ImGui::SmallButton(g_collapsed ? "[+]" : "[-]"))
		g_collapsed = !g_collapsed;
	ImGui::SameLine();
	ImGui::TextDisabled("KRUDD EDITOR");
	draw_undo_redo();
	draw_sim_mode();
#ifdef __EMSCRIPTEN__
	if (g_touch_device) {
		/*
		 * No physical backtick on a touch device, and no reason to
		 * pop the native keyboard on every tap (see imgui_plugin.cpp)
		 * — so replace the hint with a button that shows/hides it.
		 */
		const char *label = g_kbd_shown ? "Hide KB" : "Show KB";

		hint_x = ImGui::GetWindowWidth()
			- ImGui::CalcTextSize(label).x
			- 2.0f * ImGui::GetStyle().FramePadding.x
			- ImGui::GetStyle().WindowPadding.x;
		ImGui::SameLine(hint_x);
		if (ImGui::SmallButton(label)) {
			g_kbd_shown = !g_kbd_shown;
			if (g_kbd_shown)
				krudd_text_input_show();
			else
				krudd_text_input_hide();
		}
	} else
#endif
	{
		hint_x = ImGui::GetWindowWidth()
			- ImGui::CalcTextSize("` to hide").x
			- ImGui::GetStyle().WindowPadding.x;
		ImGui::SameLine(hint_x);
		ImGui::TextDisabled("` to hide");
	}
	ImGui::Separator();

	if (!g_collapsed) {
		if (ImGui::BeginTabBar("##tabs",
				       ImGuiTabBarFlags_FittingPolicyScroll)) {
			if (ImGui::BeginTabItem("Scene")) {
				draw_tab_world();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Assets")) {
				draw_tab_assets();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("KRUDD")) {
				draw_tab_krudd();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}

	ImGui::End();
}

/* ------------------------------------------------------------------ */
/* Plugin lifecycle                                                    */
/* ------------------------------------------------------------------ */

static void kruddboard_init(void)
{
#ifdef __EMSCRIPTEN__
	emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT,
					nullptr, 0, on_keydown);
	g_touch_device = krudd_is_touch_device() != 0;

	/*
	 * Register the Scheme primitives eagerly, not on first panel draw. The
	 * gizmo-mode getter/setter (krudd-gizmo-mode / krudd-set-gizmo-mode)
	 * lives here but is now also read by kruddgui's mode-bar, whose tick can
	 * run before any kruddboard tab has drawn (the board may be collapsed).
	 * Populating the shared s7 environment at init makes the binding exist
	 * regardless of board visibility; the draw-time calls then no-op.
	 */
	ensure_panel_scm();
#endif
	if (g_log)
		g_log->write(LOG_LEVEL_INFO, "kruddboard: init");
}

static void kruddboard_tick(void)
{
#ifdef __EMSCRIPTEN__
	const struct imgui_api *imgui;

	if (g_panels_registered)
		return;

	imgui = (const struct imgui_api *)
		subsystem_manager_get_api(g_mgr, "imgui");

	if (!imgui)
		return;

	imgui->register_panel("##kruddboard", draw_board, nullptr);
	g_panels_registered = 1;

	if (g_log)
		g_log->write(LOG_LEVEL_INFO, "kruddboard: board registered");
#endif
}

static void kruddboard_shutdown(void)
{
	if (g_log)
		g_log->write(LOG_LEVEL_INFO, "kruddboard: shutdown");
}

static const struct subsystem desc = {
	"kruddboard",
	nullptr,
	kruddboard_init,
	kruddboard_tick,
	kruddboard_shutdown,
};

extern "C" void kruddboard_plugin_entry(struct subsystem_manager *mgr)
{
#ifdef __EMSCRIPTEN__
	g_log       = (const struct log_api *)
		subsystem_manager_get_api(mgr, "log");
	g_stats     = (const struct stats_api *)
		subsystem_manager_get_api(mgr, "stats");
	g_asset_api = (const struct asset_api *)
		subsystem_manager_get_api(mgr, "asset");
	g_asset_mut  = (const struct asset_mut_api *)
		subsystem_manager_get_api(mgr, "asset_mut");
	g_backend    = (const struct backend_api *)
		subsystem_manager_get_api(mgr, "backend");
	g_entity_api = (const struct entity_api *)
		subsystem_manager_get_api(mgr, "scene");
	g_edit_api   = (const struct edit_api *)
		subsystem_manager_get_api(mgr, "edit");
	g_camera_api = (const struct camera_api *)
		subsystem_manager_get_api(mgr, "camera");
	g_preview_api = (const struct preview_api *)
		subsystem_manager_get_api(mgr, "mesh_preview");
	g_mem        = (const struct memory_api *)
		subsystem_manager_get_api(mgr, "memory");
	g_mgr        = mgr;
#endif

	subsystem_manager_register(mgr, &desc);
}
