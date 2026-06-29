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
 *   KRUDD  — frame stats, subsystems, log (collapsible sections)
 *   World  — entity list, create/delete, inspector
 *   Assets — asset browser and markdown editor
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
#ifdef __EMSCRIPTEN__
#include "backend_api.h"
#endif
}

#include "imgui.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#include <string.h>
#include "md_parse.h"
#include "md_draw.h"
#endif

#include <cstdio>

static const struct log_api           *g_log;
static const struct stats_api         *g_stats;
static const struct asset_api         *g_asset_api;
static const struct subsystem_manager *g_mgr;
static int                             g_visible = 1;
static int                             g_collapsed;
static int                             g_panels_registered;
static uint32_t                        g_asset_sel; /* 0 = none */
static const struct entity_api        *g_entity_api;
static int32_t                         g_entity_sel = -1; /* -1 = none */

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
	if (strcmp(e->code, "Backquote") == 0) {
		g_visible = !g_visible;
		return EM_TRUE;
	}
	return EM_FALSE;
}
#endif

/* ------------------------------------------------------------------ */
/* Tab: Frame Stats                                                    */
/* ------------------------------------------------------------------ */

static void draw_tab_stats(void)
{
	if (!g_stats) {
		ImGui::TextDisabled("(stats unavailable)");
		return;
	}
	ImGui::Text("FPS (avg): %.1f", (double)g_stats->fps_avg);
	ImGui::Text("Frame ms:  %.2f", (double)g_stats->last_frame_ms);
	ImGui::Text("Frame:     %u",   g_stats->frame_count);
}

/* ------------------------------------------------------------------ */
/* Tab: Log                                                            */
/* ------------------------------------------------------------------ */

static void draw_tab_log(void)
{
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
}

/* ------------------------------------------------------------------ */
/* Tab: Subsystems                                                     */
/* ------------------------------------------------------------------ */

static void draw_tab_subsystems(void)
{
	int i;
	char size_buf[32];

	if (!g_mgr) {
		ImGui::TextDisabled("(subsystem manager unavailable)");
		return;
	}

	if (ImGui::BeginTable("##subsys", 4,
			      ImGuiTableFlags_Borders        |
			      ImGuiTableFlags_RowBg          |
			      ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Name");
		ImGui::TableSetupColumn("API");
		ImGui::TableSetupColumn("Tick");
		ImGui::TableSetupColumn("WASM Size");
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
			ImGui::TableSetColumnIndex(3);
			if (g_mgr->static_table[i].wasm_size) {
				snprintf(size_buf, sizeof(size_buf), "%u",
					g_mgr->static_table[i].wasm_size);
				ImGui::TextUnformatted(size_buf);
			} else {
				ImGui::TextDisabled("-");
			}
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
			ImGui::TableSetColumnIndex(3);
			if (g_mgr->dynamic[i].wasm_size) {
				snprintf(size_buf, sizeof(size_buf), "%u",
					g_mgr->dynamic[i].wasm_size);
				ImGui::TextUnformatted(size_buf);
			} else {
				ImGui::TextDisabled("-");
			}
		}

		ImGui::EndTable();
	}
}

/* ------------------------------------------------------------------ */
/* Tab: Assets                                                         */
/* ------------------------------------------------------------------ */

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
#ifdef __EMSCRIPTEN__
#define EDIT_BUF_MAX (64 * 1024)
static char     g_edit[EDIT_BUF_MAX];
static uint32_t g_edit_id; /* id whose bytes are in g_edit; 0 = none */

static struct md_block g_blocks[MD_BLOCKS_MAX];
static int32_t         g_nblocks;

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
#endif /* __EMSCRIPTEN__ */

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
		if (can_persist) {
			if (ImGui::Button("Save")) {
				if (g_asset_mut)
					g_asset_mut->set_data(
						id, g_edit,
						(uint32_t)edit_len);
				g_backend->persist_asset(
					id, info.path,
					ASSET_TYPE_TEXT, g_edit,
					(uint32_t)edit_len);
			}
		} else {
			ImGui::BeginDisabled();
			ImGui::Button("Save");
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextDisabled(
				"in-memory only (persistence unavailable)");
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

static void draw_tab_assets(void)
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
	 * Uses an inline name-entry row to collect the asset path.
	 */
	if (g_asset_sel == 0 && g_asset_mut) {
		static char new_name[256];
		static int  naming; /* 1 while the input row is visible */

		if (!naming) {
			if (ImGui::Button("New Asset")) {
				naming      = 1;
				new_name[0] = '\0';
			}
		} else {
			ImGui::SetNextItemWidth(240.0f);
			bool confirm = ImGui::InputText(
				"##newname", new_name, sizeof(new_name),
				ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::SameLine();
			confirm |= ImGui::Button("Create");
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				naming = 0;

			if (confirm && new_name[0] != '\0') {
				uint32_t nid;
				int      can_persist;

				nid = g_asset_mut->create(
					new_name, ASSET_TYPE_TEXT,
					"", 0);
				if (nid != 0) {
					can_persist =
						g_backend &&
						(g_backend->get_caps() &
						 BACKEND_CAP_PROJECT_PERSIST);
					if (can_persist)
						g_backend->persist_asset(
							nid, new_name,
							ASSET_TYPE_TEXT,
							"", 0);
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

/* ------------------------------------------------------------------ */
/* Tab: KRUDD — frame stats, subsystems, log                          */
/* ------------------------------------------------------------------ */

static void draw_tab_krudd(void)
{
	if (ImGui::CollapsingHeader("Frame Stats",
				    ImGuiTreeNodeFlags_DefaultOpen))
		draw_tab_stats();

	if (ImGui::CollapsingHeader("Subsystems",
				    ImGuiTreeNodeFlags_DefaultOpen))
		draw_tab_subsystems();

	if (ImGui::CollapsingHeader("Log",
				    ImGuiTreeNodeFlags_DefaultOpen))
		draw_tab_log();
}

/* ------------------------------------------------------------------ */
/* Tab: World — entity list, create/delete, inspector                 */
/* ------------------------------------------------------------------ */

static void draw_tab_world(void)
{
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
			int32_t          id;

			seed.rotation[3] = 1.0f;
			seed.scale[0] = seed.scale[1] = seed.scale[2] = 1.0f;
			id = g_entity_api->create_entity(WORLD_NO_PARENT, &seed,
							 0u, 0u);
			if (id >= 0) {
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
		}
	}
}

/* ------------------------------------------------------------------ */
/* Main board window                                                   */
/* ------------------------------------------------------------------ */

static void draw_board(void * /*userdata*/)
{
	float            vp_w;
	float            vp_h;
	float            win_w;
	float            hint_x;
	ImGuiWindowFlags flags;

	if (!g_visible)
		return;

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
	hint_x = ImGui::GetWindowWidth()
		- ImGui::CalcTextSize("` to hide").x
		- ImGui::GetStyle().WindowPadding.x;
	ImGui::SameLine(hint_x);
	ImGui::TextDisabled("` to hide");
	ImGui::Separator();

	if (!g_collapsed) {
		if (ImGui::BeginTabBar("##tabs",
				       ImGuiTabBarFlags_FittingPolicyScroll)) {
			if (ImGui::BeginTabItem("KRUDD")) {
				draw_tab_krudd();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("World")) {
				draw_tab_world();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Assets")) {
				draw_tab_assets();
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

#ifdef __EMSCRIPTEN__
extern "C" void plugin_entry(struct subsystem_manager *mgr)
#else
extern "C" void kruddboard_entry(struct subsystem_manager *mgr)
#endif
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
	g_mgr        = mgr;
#endif

	subsystem_manager_register(mgr, &desc);
}
