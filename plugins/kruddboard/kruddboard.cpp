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
 *   KRUDD    — frame stats, subsystems, log (collapsible sections)
 *   World    — entity list, create/delete, inspector
 *   Assets   — asset browser and markdown editor
 *   Branches — branch/snapshot browser (#213/#217)
 */

extern "C" {
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"
#include "log_level.h"
#include "stats_api.h"
#include "imgui_api.h"
#include "asset_api.h"
#include "asset_codec_api.h"
#include "entity_api.h"
#include "edit_api.h"
#include "vscript_api.h"
#include "shader_graph_api.h"
#include "memory_api.h"
#ifdef __EMSCRIPTEN__
#include "backend_api.h"
#include "branch_api.h"
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
static const struct edit_api          *g_edit_api;  /* NULL = no history */

#ifdef __EMSCRIPTEN__
static const struct asset_mut_api     *g_asset_mut;
static const struct backend_api       *g_backend;
static const struct vscript_api       *g_vscript;
static const struct shader_graph_api  *g_shader_graph;
static const struct asset_codec_api   *g_codec;
static const struct memory_api        *g_mem;
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
	case ASSET_TYPE_VSCRIPT:  return "Visual Script";
	default:                  return "Unknown";
	}
}

#ifdef __EMSCRIPTEN__
/*
 * Shader authoring metadata.  These mirror the decl fields the asset system
 * reports via describe(); index 0 is the default for a freshly created asset.
 */
static const char *const SHADER_STAGES[]   = { "fragment", "vertex" };
static const char *const SHADER_DIALECTS[] = { "glsl_es_300" };

#define SHADER_STAGE_COUNT \
	((int)(sizeof(SHADER_STAGES) / sizeof(SHADER_STAGES[0])))
#define SHADER_DIALECT_COUNT \
	((int)(sizeof(SHADER_DIALECTS) / sizeof(SHADER_DIALECTS[0])))

static int shader_stage_idx(const char *s)
{
	return (s && strcmp(s, "vertex") == 0) ? 1 : 0;
}

/* True if path already ends in a recognized shader extension. */
static int has_shader_ext(const char *s)
{
	const char *dot = strrchr(s, '.');

	return dot && (strcmp(dot, ".vert") == 0 ||
		       strcmp(dot, ".frag") == 0 ||
		       strcmp(dot, ".glsl") == 0);
}
#endif /* __EMSCRIPTEN__ */

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

/*
 * Inspector combo state for the selected authored shader.  Loaded from the
 * asset's declaration when the selection changes, then edited in place by the
 * stage/dialect combos and written back on Save via asset_mut.set_decl.
 */
static uint32_t g_sh_id; /* id whose decl is in the combos; 0 = none */
static int      g_sh_stage;
static int      g_sh_dialect;

static void maybe_reload_shader_decl(uint32_t id, uint32_t idx)
{
	struct asset_decl_field f[16];
	uint32_t                n;
	uint32_t                i;

	if (g_sh_id == id)
		return;
	g_sh_id      = id;
	g_sh_stage   = 0;
	g_sh_dialect = 0;
	if (!g_asset_api || !g_asset_api->describe)
		return;
	n = g_asset_api->describe(idx, f, 16);
	for (i = 0; i < n; i++) {
		if (strcmp(f[i].key, "stage") == 0)
			g_sh_stage = shader_stage_idx(f[i].value);
		/* dialect has a single option today; nothing to map. */
	}
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

	/*
	 * Authored shader assets get a plain source editor plus editable
	 * stage/dialect metadata — no markdown parsing or preview.
	 */
	if (info.origin == ASSET_ORIGIN_AUTHORED &&
	    info.type   == ASSET_TYPE_SHADER) {
		int      can_persist;
		size_t   edit_len;
		uint32_t sh_idx;
		uint32_t sh_n;
		uint32_t k;

		can_persist = g_backend &&
			(g_backend->get_caps() &
			 BACKEND_CAP_PROJECT_PERSIST);

		/* Resolve the index describe() is addressed by. */
		sh_n   = g_asset_api->count();
		sh_idx = sh_n; /* sentinel: not found */
		for (k = 0; k < sh_n; k++) {
			if (g_asset_api->info(k, &tmp) == 0 &&
			    tmp.id == id) {
				sh_idx = k;
				break;
			}
		}

		maybe_reload_shader_decl(id, sh_idx);
		maybe_reload_edit(id);

		ImGui::Separator();

		/* -- Declaration (editable) -- */
		if (ImGui::CollapsingHeader("Declaration",
					    ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::SetNextItemWidth(160.0f);
			ImGui::Combo("stage", &g_sh_stage, SHADER_STAGES,
				     SHADER_STAGE_COUNT);
			ImGui::SetNextItemWidth(160.0f);
			ImGui::Combo("dialect", &g_sh_dialect, SHADER_DIALECTS,
				     SHADER_DIALECT_COUNT);
		}

		ImGui::Separator();

		/* -- Source editor (plain; no markdown preview) -- */
		if (ImGui::CollapsingHeader("Source",
					    ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::InputTextMultiline(
				"##shader", g_edit, (size_t)EDIT_BUF_MAX,
				ImVec2(-1.0f, 260.0f));
		}

		ImGui::Separator();

		/* -- Action buttons -- */
		edit_len = strlen(g_edit);
		if (can_persist) {
			if (ImGui::Button("Save")) {
				struct asset_decl_field decl[2];

				decl[0].key   = "dialect";
				decl[0].value = SHADER_DIALECTS[g_sh_dialect];
				decl[1].key   = "stage";
				decl[1].value = SHADER_STAGES[g_sh_stage];

				if (g_asset_mut) {
					g_asset_mut->set_data(
						id, g_edit,
						(uint32_t)edit_len);
					if (g_asset_mut->set_decl)
						g_asset_mut->set_decl(
							id, decl, 2);
				}
				g_backend->persist_asset(
					id, info.path,
					ASSET_TYPE_SHADER, g_edit,
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
			g_sh_id     = 0;
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
	 * A small form collects the name, the asset type (Text / Shader), and,
	 * for shaders, the stage; the stage drives both the decl and a derived
	 * file extension.
	 */
	if (g_asset_sel == 0 && g_asset_mut) {
		static char new_name[256];
		static int  naming;    /* 1 while the form is visible */
		static int  new_type;  /* 0 = Text, 1 = Shader */
		static int  new_stage; /* index into SHADER_STAGES */

		static const char *const NEW_TYPES[] = { "Text", "Shader" };

		if (!naming) {
			if (ImGui::Button("New Asset")) {
				naming      = 1;
				new_name[0] = '\0';
				new_type    = 0;
				new_stage   = 0;
			}
		} else {
			ImGui::SetNextItemWidth(240.0f);
			bool confirm = ImGui::InputText(
				"name", new_name, sizeof(new_name),
				ImGuiInputTextFlags_EnterReturnsTrue);

			ImGui::SetNextItemWidth(160.0f);
			ImGui::Combo("type", &new_type, NEW_TYPES, 2);

			if (new_type == 1) {
				ImGui::SetNextItemWidth(160.0f);
				ImGui::Combo("stage", &new_stage,
					     SHADER_STAGES, SHADER_STAGE_COUNT);
			}

			confirm |= ImGui::Button("Create");
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				naming = 0;

			if (confirm && new_name[0] != '\0') {
				char     path[256];
				int32_t  atype;
				uint32_t nid;
				int      can_persist;

				atype = (new_type == 1) ? ASSET_TYPE_SHADER
							: ASSET_TYPE_TEXT;

				/* Shaders get a stage-derived extension. */
				if (atype == ASSET_TYPE_SHADER &&
				    !has_shader_ext(new_name))
					snprintf(path, sizeof(path), "%s.%s",
						 new_name,
						 new_stage == 1 ? "vert"
								: "frag");
				else
					snprintf(path, sizeof(path), "%s",
						 new_name);

				nid = g_asset_mut->create(path, atype, "", 0);
				if (nid != 0) {
					if (atype == ASSET_TYPE_SHADER &&
					    g_asset_mut->set_decl) {
						struct asset_decl_field d[2];

						d[0].key   = "dialect";
						d[0].value = "glsl_es_300";
						d[1].key   = "stage";
						d[1].value =
							SHADER_STAGES[new_stage];
						g_asset_mut->set_decl(nid, d, 2);
					}
					can_persist =
						g_backend &&
						(g_backend->get_caps() &
						 BACKEND_CAP_PROJECT_PERSIST);
					if (can_persist)
						g_backend->persist_asset(
							nid, path, atype,
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
/* Tab: Branches — branch/snapshot browser (#213/#217)                */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
/*
 * New-branch name buffer and the selected row in the active branch's
 * snapshot timeline (-1 = none). Selection is keyed to the *active* branch,
 * so a branch_switch() / snapshot_restore() invalidates it — reset whenever
 * branch_active() changes underneath us.
 */
static char    g_branch_new_name[BRANCH_API_NAME_MAX];
static int32_t g_branch_snap_sel    = -1;
static int32_t g_branch_last_active = -2; /* sentinel: force first reset */
#endif

static void draw_tab_branches(void)
{
#ifdef __EMSCRIPTEN__
	const struct branch_api *br;
	uint32_t                 n;
	uint32_t                 i;
	int32_t                  active;
	int                      merge_ok;
	struct branch_api_desc   desc;
	struct branch_api_snapshot snap;

	br = (g_backend && (g_backend->get_caps() & BACKEND_CAP_BRANCHING))
		? g_backend->branching() : NULL;

	if (!br) {
		ImGui::TextDisabled("(branching unavailable)");
		return;
	}

	active = br->branch_active();
	if (active != g_branch_last_active) {
		g_branch_last_active = active;
		g_branch_snap_sel    = -1;
	}

	/* ---- Branch list ---- */
	if (ImGui::CollapsingHeader("Branches",
				    ImGuiTreeNodeFlags_DefaultOpen)) {
		n = br->branch_count();
		if (n == 0) {
			ImGui::TextDisabled("(no branches)");
		} else if (ImGui::BeginTable("##branches", 2,
					     ImGuiTableFlags_Borders |
					     ImGuiTableFlags_RowBg   |
					     ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("Name");
			ImGui::TableSetupColumn("Base");
			ImGui::TableHeadersRow();

			for (i = 0; i < n; i++) {
				char row_id[80];

				if (br->branch_get((int32_t)i, &desc) != 0)
					continue;
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				snprintf(row_id, sizeof(row_id), "%s%s##br%u",
					 desc.name, desc.active ? " *" : "",
					 i);
				if (ImGui::Selectable(
					    row_id, desc.active != 0,
					    ImGuiSelectableFlags_SpanAllColumns) &&
				    !desc.active)
					br->branch_switch(desc.index);
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(
					desc.has_base ? "snapshot" : "-");
			}

			ImGui::EndTable();
		}

		ImGui::Separator();

		/* -- New branch -- */
		ImGui::SetNextItemWidth(200.0f);
		ImGui::InputText("##newbranch", g_branch_new_name,
				 sizeof(g_branch_new_name));

		ImGui::SameLine();
		ImGui::BeginDisabled(g_branch_new_name[0] == '\0');
		if (ImGui::SmallButton("New from head") &&
		    g_branch_new_name[0] != '\0') {
			if (br->branch_fork(g_branch_new_name,
					    BRANCH_FROM_HEAD) >= 0)
				g_branch_new_name[0] = '\0';
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::BeginDisabled(g_branch_new_name[0] == '\0' ||
				     g_branch_snap_sel < 0);
		if (ImGui::SmallButton("New from snapshot") &&
		    g_branch_new_name[0] != '\0' && g_branch_snap_sel >= 0) {
			if (br->branch_fork(g_branch_new_name,
					    g_branch_snap_sel) >= 0)
				g_branch_new_name[0] = '\0';
		}
		ImGui::EndDisabled();
	}

	ImGui::Separator();

	/* ---- Snapshots of the active branch ---- */
	if (ImGui::CollapsingHeader("Snapshots",
				    ImGuiTreeNodeFlags_DefaultOpen)) {
		n = br->snapshot_count();
		if (n == 0) {
			ImGui::TextDisabled("(no snapshots)");
		} else if (ImGui::BeginTable("##snapshots", 4,
					     ImGuiTableFlags_Borders |
					     ImGuiTableFlags_RowBg   |
					     ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("Index");
			ImGui::TableSetupColumn("Seq");
			ImGui::TableSetupColumn("Label");
			ImGui::TableSetupColumn("");
			ImGui::TableHeadersRow();

			for (i = 0; i < n; i++) {
				char row_id[32];
				char restore_id[24];

				if (br->snapshot_get(i, &snap) != 0)
					continue;
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				snprintf(row_id, sizeof(row_id), "%u##sn%u",
					 snap.index, i);
				if (ImGui::Selectable(
					    row_id,
					    g_branch_snap_sel == (int32_t)i,
					    ImGuiSelectableFlags_SpanAllColumns))
					g_branch_snap_sel = (int32_t)i;
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%u", snap.seq);
				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%u", snap.label);
				ImGui::TableSetColumnIndex(3);
				snprintf(restore_id, sizeof(restore_id),
					 "Restore##r%u", i);
				if (ImGui::SmallButton(restore_id))
					br->snapshot_restore(i);
			}

			ImGui::EndTable();
		}
	}

	ImGui::Separator();

	/* ---- Merge (declared, unimplemented in v1) ---- */
	merge_ok = br->merge_supported() != 0;
	ImGui::BeginDisabled(!merge_ok);
	ImGui::SmallButton("Merge... (coming soon)");
	ImGui::EndDisabled();
#else
	ImGui::TextDisabled("(branching unavailable)");
#endif /* __EMSCRIPTEN__ */
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

/* ------------------------------------------------------------------ */
/* Shader Graph tab — hand-rolled node canvas over ImDrawList          */
/* ------------------------------------------------------------------ */

/* Canvas geometry, in graph (world) units before pan/zoom. */
#define SG_NODE_W    148.0f
#define SG_TITLE_H    24.0f
#define SG_PIN_SP     20.0f
#define SG_PIN_R       5.0f
#define SG_POS_MAX   128

struct sg_pos { int32_t id; float x; float y; };

static vscript_graph_t g_sg_graph;           /* live document (NULL = none) */
static uint32_t        g_sg_asset;           /* source .vscript id (0=new)  */
static uint32_t        g_sg_shader;          /* last compiled shader id (0) */
static int32_t         g_sg_sel = -1;        /* selected node id (-1)       */
static int32_t         g_sg_link_from = -1;  /* pending-link source node    */
static uint32_t        g_sg_link_port;       /* pending-link output port    */
static int32_t         g_sg_drag = -1;       /* node being dragged (-1)     */
static int             g_sg_add_sel;         /* palette combo index         */
static float           g_sg_zoom = 1.0f;
static char            g_sg_name[96] = "untitled";
static char            g_sg_status[160];
static ImVec2          g_sg_pan;
static ImVec2          g_sg_origin;          /* canvas top-left this frame  */
static ImVec2          g_sg_canvas_sz;
static struct sg_pos   g_sg_positions[SG_POS_MAX];
static uint32_t        g_sg_npos;

static void sg_status(const char *s)
{
	snprintf(g_sg_status, sizeof(g_sg_status), "%s", s);
}

static void sg_pos_set(int32_t id, float x, float y)
{
	uint32_t i;

	for (i = 0; i < g_sg_npos; i++) {
		if (g_sg_positions[i].id == id) {
			g_sg_positions[i].x = x;
			g_sg_positions[i].y = y;
			return;
		}
	}
	if (g_sg_npos < SG_POS_MAX) {
		g_sg_positions[g_sg_npos].id = id;
		g_sg_positions[g_sg_npos].x  = x;
		g_sg_positions[g_sg_npos].y  = y;
		g_sg_npos++;
	}
}

static void sg_pos_get(int32_t id, float *x, float *y)
{
	uint32_t i;

	for (i = 0; i < g_sg_npos; i++) {
		if (g_sg_positions[i].id == id) {
			*x = g_sg_positions[i].x;
			*y = g_sg_positions[i].y;
			return;
		}
	}
	*x = 40.0f;
	*y = 40.0f;
}

static ImVec2 sg_screen(float wx, float wy)
{
	return ImVec2(g_sg_origin.x + g_sg_pan.x + wx * g_sg_zoom,
		      g_sg_origin.y + g_sg_pan.y + wy * g_sg_zoom);
}

static float sg_node_h(int32_t id)
{
	uint32_t nin  = g_vscript->node_input_count(g_sg_graph, id);
	uint32_t nout = g_vscript->node_output_count(g_sg_graph, id);
	uint32_t rows = nin > nout ? nin : nout;

	if (rows == 0)
		rows = 1;
	return SG_TITLE_H + (float)rows * SG_PIN_SP + 6.0f;
}

static ImVec2 sg_in_pin(int32_t id, uint32_t port)
{
	float x, y;

	sg_pos_get(id, &x, &y);
	return sg_screen(x, y + SG_TITLE_H + SG_PIN_SP * ((float)port + 0.5f));
}

static ImVec2 sg_out_pin(int32_t id, uint32_t port)
{
	float x, y;

	sg_pos_get(id, &x, &y);
	return sg_screen(x + SG_NODE_W,
			 y + SG_TITLE_H + SG_PIN_SP * ((float)port + 0.5f));
}

static bool sg_near(ImVec2 a, ImVec2 b, float r)
{
	float dx = a.x - b.x;
	float dy = a.y - b.y;

	return dx * dx + dy * dy <= r * r;
}

/* Topmost node whose body contains screen point p, or -1. */
static int32_t sg_node_at(ImVec2 p)
{
	uint32_t n = g_vscript->node_count(g_sg_graph);
	int32_t  hit = -1;
	uint32_t i;

	for (i = 0; i < n; i++) {
		int32_t id = g_vscript->node_id_at(g_sg_graph, i);
		float   x, y;
		ImVec2  a, b;

		sg_pos_get(id, &x, &y);
		a = sg_screen(x, y);
		b = sg_screen(x + SG_NODE_W, y + sg_node_h(id));
		if (p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y)
			hit = id;
	}
	return hit;
}

static bool sg_out_pin_at(ImVec2 p, int32_t *node, uint32_t *port)
{
	uint32_t n = g_vscript->node_count(g_sg_graph);
	float    r = SG_PIN_R * g_sg_zoom + 5.0f;
	uint32_t i, j;

	for (i = 0; i < n; i++) {
		int32_t  id = g_vscript->node_id_at(g_sg_graph, i);
		uint32_t no = g_vscript->node_output_count(g_sg_graph, id);

		for (j = 0; j < no; j++) {
			if (sg_near(p, sg_out_pin(id, j), r)) {
				*node = id;
				*port = j;
				return true;
			}
		}
	}
	return false;
}

static bool sg_in_pin_at(ImVec2 p, int32_t *node, uint32_t *port)
{
	uint32_t n = g_vscript->node_count(g_sg_graph);
	float    r = SG_PIN_R * g_sg_zoom + 5.0f;
	uint32_t i, j;

	for (i = 0; i < n; i++) {
		int32_t  id = g_vscript->node_id_at(g_sg_graph, i);
		uint32_t ni = g_vscript->node_input_count(g_sg_graph, id);

		for (j = 0; j < ni; j++) {
			if (sg_near(p, sg_in_pin(id, j), r)) {
				*node = id;
				*port = j;
				return true;
			}
		}
	}
	return false;
}

static void sg_try_connect(int32_t from, uint32_t fport, int32_t to,
			   uint32_t tport)
{
	if (from == to) {
		sg_status("cannot wire a node to itself");
		return;
	}
	if (g_vscript->connect(g_sg_graph, from, fport, to, tport) != 0) {
		sg_status("rejected: type mismatch or input already driven");
		return;
	}
	if (g_vscript->validate(g_sg_graph) != 0) {
		g_vscript->disconnect(g_sg_graph, to, tport);
		sg_status("rejected: would create a cycle");
		return;
	}
	sg_status("connected");
}

static void sg_new(void)
{
	int32_t o;

	if (!g_vscript)
		return;
	if (g_sg_graph)
		g_vscript->destroy(g_sg_graph);
	g_sg_graph     = g_vscript->create(VSCRIPT_TARGET_SHADER);
	g_sg_npos      = 0;
	g_sg_sel       = -1;
	g_sg_asset     = 0;
	g_sg_shader    = 0;
	g_sg_link_from = -1;
	g_sg_drag      = -1;
	snprintf(g_sg_name, sizeof(g_sg_name), "%s", "untitled");
	o = g_vscript->add_node(g_sg_graph, "output", nullptr);
	sg_pos_set(o, 320.0f, 140.0f);
	sg_status("new graph — add nodes, wire into Output, Compile");
}

static void sg_add_selected(void)
{
	const char *type;
	int32_t     id;
	float       wx, wy;

	if (!g_sg_graph)
		return;
	type = g_vscript->type_name_at((uint32_t)g_sg_add_sel);
	if (!type)
		return;
	id = g_vscript->add_node(g_sg_graph, type, "");
	if (id < 1) {
		sg_status("add failed (graph full?)");
		return;
	}
	wx = (g_sg_canvas_sz.x * 0.5f - g_sg_pan.x) / g_sg_zoom;
	wy = (g_sg_canvas_sz.y * 0.5f - g_sg_pan.y) / g_sg_zoom;
	sg_pos_set(id, wx, wy);
	g_sg_sel = id;
}

static void sg_compile(void)
{
	char     path[128];
	uint32_t sid;

	if (!g_sg_graph || !g_shader_graph) {
		sg_status("no shader_graph service");
		return;
	}
	/* Recompiling reuses the derived path, so drop the previous asset. */
	if (g_sg_shader && g_asset_mut) {
		g_asset_mut->destroy(g_sg_shader);
		if (g_backend &&
		    (g_backend->get_caps() & BACKEND_CAP_PROJECT_PERSIST))
			g_backend->delete_asset(g_sg_shader);
		g_sg_shader = 0;
	}
	snprintf(path, sizeof(path), "%s.frag", g_sg_name);
	sid = g_shader_graph->compile(g_sg_graph, path);
	if (!sid) {
		sg_status("compile failed: need one Output, all inputs wired");
		return;
	}
	g_sg_shader = sid;
	if (g_backend &&
	    (g_backend->get_caps() & BACKEND_CAP_PROJECT_PERSIST)) {
		uint32_t    sz  = 0;
		const void *src = g_asset_api->get_data(sid, &sz);

		if (src)
			g_backend->persist_asset(sid, path,
						 ASSET_TYPE_SHADER, src, sz);
	}
	snprintf(g_sg_status, sizeof(g_sg_status), "compiled -> %s", path);
}

static void sg_save(void)
{
	struct asset_decl_field decl[1];
	char     path[128];
	void    *bytes;
	uint32_t sz = 0;

	if (!g_sg_graph || !g_codec || !g_asset_mut) {
		sg_status("save unavailable");
		return;
	}
	bytes = g_codec->encode("vscript", g_sg_graph, &sz);
	if (!bytes) {
		sg_status("encode failed");
		return;
	}
	snprintf(path, sizeof(path), "%s.vscript", g_sg_name);
	decl[0].key   = "target";
	decl[0].value = "shader";

	if (g_sg_asset == 0) {
		uint32_t id = g_asset_mut->create(path, ASSET_TYPE_VSCRIPT,
						  bytes, sz);
		if (id) {
			if (g_asset_mut->set_decl)
				g_asset_mut->set_decl(id, decl, 1);
			g_sg_asset = id;
		}
	} else {
		g_asset_mut->set_data(g_sg_asset, bytes, sz);
		if (g_asset_mut->set_decl)
			g_asset_mut->set_decl(g_sg_asset, decl, 1);
	}
	if (g_sg_asset && g_backend &&
	    (g_backend->get_caps() & BACKEND_CAP_PROJECT_PERSIST))
		g_backend->persist_asset(g_sg_asset, path,
					 ASSET_TYPE_VSCRIPT, bytes, sz);
	if (g_mem)
		g_mem->free(bytes);
	snprintf(g_sg_status, sizeof(g_sg_status), "saved -> %s", path);
}

/* Lay opened graphs out left-to-right by longest-path depth. */
static void sg_auto_layout(void)
{
	int32_t order[SG_POS_MAX];
	int32_t level[SG_POS_MAX];
	int32_t rowcount[SG_POS_MAX];
	int32_t n, i, k;

	if (!g_sg_graph)
		return;
	n = g_vscript->topo_order(g_sg_graph, order, SG_POS_MAX);
	if (n < 0) {
		uint32_t c = g_vscript->node_count(g_sg_graph);
		uint32_t i2;

		for (i2 = 0; i2 < c; i2++)
			sg_pos_set(g_vscript->node_id_at(g_sg_graph, i2),
				   40.0f + (float)(i2 % 4) * 170.0f,
				   40.0f + (float)(i2 / 4) * 110.0f);
		return;
	}
	for (i = 0; i < n; i++) {
		level[i]    = 0;
		rowcount[i] = 0;
	}
	for (i = 0; i < n; i++) {
		int32_t  id = order[i];
		uint32_t ni = g_vscript->node_input_count(g_sg_graph, id);
		uint32_t p;

		for (p = 0; p < ni; p++) {
			int32_t src;

			if (g_vscript->input_source(g_sg_graph, id, p,
						    &src, nullptr) != 0)
				continue;
			for (k = 0; k < n; k++) {
				if (order[k] == src && level[k] + 1 > level[i])
					level[i] = level[k] + 1;
			}
		}
	}
	for (i = 0; i < n; i++) {
		int32_t lv = level[i];

		sg_pos_set(order[i], 40.0f + (float)lv * 180.0f,
			   40.0f + (float)rowcount[lv] * 96.0f);
		rowcount[lv]++;
	}
}

static void sg_open(uint32_t asset_id)
{
	struct asset_info info;
	const void       *bytes;
	uint32_t          sz = 0;
	vscript_graph_t   g;
	const char       *base;
	char             *dot;

	if (!g_codec || !g_asset_api)
		return;
	if (g_asset_api->find(asset_id, &info) != 0)
		return;
	bytes = g_asset_api->get_data(asset_id, &sz);
	if (!bytes) {
		sg_status("open: no data");
		return;
	}
	g = (vscript_graph_t)g_codec->decode_bytes("vscript", bytes, sz);
	if (!g) {
		sg_status("open: decode failed");
		return;
	}
	if (g_vscript->require_target(g, VSCRIPT_TARGET_SHADER) != 0) {
		g_vscript->destroy(g);
		sg_status("open: not a shader graph");
		return;
	}
	if (g_sg_graph)
		g_vscript->destroy(g_sg_graph);
	g_sg_graph  = g;
	g_sg_asset  = asset_id;
	g_sg_shader = 0;
	g_sg_sel    = -1;
	g_sg_npos   = 0;
	base = strrchr(info.path, '/');
	base = base ? base + 1 : info.path;
	snprintf(g_sg_name, sizeof(g_sg_name), "%s", base);
	dot = strrchr(g_sg_name, '.');
	if (dot)
		*dot = '\0';
	sg_auto_layout();
	snprintf(g_sg_status, sizeof(g_sg_status), "opened %s", info.path);
}

/* True if catalog entry idx declares target=shader (cheap decl filter). */
static bool sg_is_shader_target(uint32_t idx)
{
	struct asset_decl_field f[8];
	uint32_t                m, k;

	m = g_asset_api->describe(idx, f, 8);
	for (k = 0; k < m; k++) {
		if (strcmp(f[k].key, "target") == 0)
			return strcmp(f[k].value, "shader") == 0;
	}
	return false;
}

static void draw_sg_open_list(void)
{
	uint32_t n, i;
	int      any = 0;

	if (!g_asset_api)
		return;
	n = g_asset_api->count();
	for (i = 0; i < n; i++) {
		struct asset_info info;
		char              label[192];

		if (g_asset_api->info(i, &info) != 0)
			continue;
		if (info.type != ASSET_TYPE_VSCRIPT || info.read_only)
			continue;
		if (!sg_is_shader_target(i))
			continue;
		any = 1;
		snprintf(label, sizeof(label), "Open  %s", info.path);
		if (ImGui::Button(label))
			sg_open(info.id);
	}
	if (!any)
		ImGui::TextDisabled("(no saved shader graphs)");
}

static void draw_tab_shader_graph(void)
{
	ImDrawList *dl;
	ImVec2      canvas_p1;
	ImGuiIO    &io = ImGui::GetIO();
	uint32_t    n, i, j;
	bool        hovered;

	if (!g_vscript) {
		ImGui::TextDisabled("vscript service unavailable");
		return;
	}

	/* Toolbar. */
	if (ImGui::Button("New"))
		sg_new();
	if (g_sg_graph) {
		ImGui::SameLine();
		if (ImGui::Button("Compile"))
			sg_compile();
		ImGui::SameLine();
		{
			bool can_persist = g_backend &&
				(g_backend->get_caps() &
				 BACKEND_CAP_PROJECT_PERSIST);

			if (can_persist) {
				if (ImGui::Button("Save"))
					sg_save();
			} else {
				ImGui::BeginDisabled();
				ImGui::Button("Save");
				ImGui::EndDisabled();
			}
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(140.0f);
		ImGui::InputText("name", g_sg_name, sizeof(g_sg_name));
	}

	if (g_sg_status[0])
		ImGui::TextDisabled("%s", g_sg_status);

	if (!g_sg_graph) {
		ImGui::TextDisabled("New starts a shader graph. Or open one:");
		draw_sg_open_list();
		return;
	}

	/* Node palette. */
	{
		const char *names[64];
		uint32_t    tc = g_vscript->type_count();

		if (tc > 64)
			tc = 64;
		for (i = 0; i < tc; i++)
			names[i] = g_vscript->type_name_at(i);
		if (g_sg_add_sel >= (int)tc)
			g_sg_add_sel = 0;
		ImGui::SetNextItemWidth(160.0f);
		ImGui::Combo("##addtype", &g_sg_add_sel, names, (int)tc);
		ImGui::SameLine();
		if (ImGui::Button("Add Node"))
			sg_add_selected();
		ImGui::SameLine();
		ImGui::TextDisabled(
			"drag out->in to wire | click in-pin to unwire | "
			"right-drag pan | wheel zoom");
	}

	if (ImGui::CollapsingHeader("Open saved graph"))
		draw_sg_open_list();

	/* Inspector for the selected node. */
	if (g_sg_sel >= 0) {
		const char *tn = g_vscript->node_type_name(g_sg_graph,
							   g_sg_sel);

		if (!tn) {
			g_sg_sel = -1;
		} else {
			const char *pv = g_vscript->node_param(g_sg_graph,
							       g_sg_sel);
			char        pbuf[64];

			snprintf(pbuf, sizeof(pbuf), "%s", pv ? pv : "");
			ImGui::Text("Node %d: %s", g_sg_sel, tn);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(160.0f);
			if (ImGui::InputText("param", pbuf, sizeof(pbuf)))
				g_vscript->set_param(g_sg_graph, g_sg_sel,
						     pbuf);
			ImGui::SameLine();
			if (ImGui::Button("Delete Node")) {
				g_vscript->remove_node(g_sg_graph, g_sg_sel);
				g_sg_sel = -1;
			}
		}
	}

	/* Canvas. */
	g_sg_canvas_sz   = ImGui::GetContentRegionAvail();
	if (g_sg_canvas_sz.x < 320.0f)
		g_sg_canvas_sz.x = 320.0f;
	g_sg_canvas_sz.y = 440.0f;
	g_sg_origin      = ImGui::GetCursorScreenPos();
	canvas_p1 = ImVec2(g_sg_origin.x + g_sg_canvas_sz.x,
			   g_sg_origin.y + g_sg_canvas_sz.y);

	ImGui::InvisibleButton("sg_canvas", g_sg_canvas_sz,
			       ImGuiButtonFlags_MouseButtonLeft |
			       ImGuiButtonFlags_MouseButtonRight);
	hovered = ImGui::IsItemHovered();

	dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(g_sg_origin, canvas_p1, true);
	dl->AddRectFilled(g_sg_origin, canvas_p1, IM_COL32(24, 24, 28, 255));
	dl->AddRect(g_sg_origin, canvas_p1, IM_COL32(60, 60, 68, 255));

	if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
		g_sg_pan.x += io.MouseDelta.x;
		g_sg_pan.y += io.MouseDelta.y;
	}
	if (hovered && io.MouseWheel != 0.0f) {
		g_sg_zoom *= (io.MouseWheel > 0.0f) ? 1.1f : 0.9f;
		if (g_sg_zoom < 0.4f)
			g_sg_zoom = 0.4f;
		if (g_sg_zoom > 2.5f)
			g_sg_zoom = 2.5f;
	}

	n = g_vscript->node_count(g_sg_graph);

	/* Links behind nodes. */
	for (i = 0; i < n; i++) {
		int32_t  id = g_vscript->node_id_at(g_sg_graph, i);
		uint32_t ni = g_vscript->node_input_count(g_sg_graph, id);

		for (j = 0; j < ni; j++) {
			int32_t  src;
			uint32_t sport;
			ImVec2   a, b;
			float    dx;

			if (g_vscript->input_source(g_sg_graph, id, j,
						    &src, &sport) != 0)
				continue;
			a  = sg_out_pin(src, sport);
			b  = sg_in_pin(id, j);
			dx = (b.x - a.x) * 0.5f;
			if (dx < 20.0f)
				dx = 20.0f;
			dl->AddBezierCubic(a, ImVec2(a.x + dx, a.y),
					   ImVec2(b.x - dx, b.y), b,
					   IM_COL32(180, 180, 90, 255), 2.0f);
		}
	}

	if (g_sg_link_from >= 0) {
		ImVec2 a  = sg_out_pin(g_sg_link_from, g_sg_link_port);
		ImVec2 b  = io.MousePos;
		float  dx = (b.x - a.x) * 0.5f;

		if (dx < 20.0f)
			dx = 20.0f;
		dl->AddBezierCubic(a, ImVec2(a.x + dx, a.y),
				   ImVec2(b.x - dx, b.y), b,
				   IM_COL32(240, 240, 140, 255), 2.0f);
	}

	/* Nodes. */
	for (i = 0; i < n; i++) {
		int32_t     id = g_vscript->node_id_at(g_sg_graph, i);
		const char *tn = g_vscript->node_type_name(g_sg_graph, id);
		uint32_t    ni = g_vscript->node_input_count(g_sg_graph, id);
		uint32_t    no = g_vscript->node_output_count(g_sg_graph, id);
		float       x, y;
		ImVec2      a, b, tp;

		sg_pos_get(id, &x, &y);
		a = sg_screen(x, y);
		b = sg_screen(x + SG_NODE_W, y + sg_node_h(id));

		dl->AddRectFilled(a, b, IM_COL32(44, 46, 54, 240), 5.0f);
		dl->AddRectFilled(a, ImVec2(b.x, a.y + SG_TITLE_H * g_sg_zoom),
				  IM_COL32(70, 74, 92, 255), 5.0f);
		dl->AddRect(a, b,
			    id == g_sg_sel ? IM_COL32(250, 220, 120, 255)
					   : IM_COL32(90, 92, 104, 255),
			    5.0f, 0, id == g_sg_sel ? 2.0f : 1.0f);
		tp = ImVec2(a.x + 6.0f, a.y + 4.0f);
		dl->AddText(tp, IM_COL32(235, 235, 240, 255), tn ? tn : "?");

		for (j = 0; j < ni; j++)
			dl->AddCircleFilled(sg_in_pin(id, j), SG_PIN_R,
					    IM_COL32(120, 200, 120, 255));
		for (j = 0; j < no; j++)
			dl->AddCircleFilled(sg_out_pin(id, j), SG_PIN_R,
					    IM_COL32(210, 160, 90, 255));
	}

	dl->PopClipRect();

	/* Interaction. */
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
		int32_t  pn;
		uint32_t pp;

		if (sg_out_pin_at(io.MousePos, &pn, &pp)) {
			g_sg_link_from = pn;
			g_sg_link_port = pp;
		} else if (sg_in_pin_at(io.MousePos, &pn, &pp)) {
			if (g_vscript->disconnect(g_sg_graph, pn, pp) == 0)
				sg_status("unwired");
		} else {
			int32_t nid = sg_node_at(io.MousePos);

			g_sg_sel  = nid;
			g_sg_drag = nid;
		}
	}

	if (g_sg_drag >= 0 && g_sg_link_from < 0 &&
	    ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
		float x, y;

		sg_pos_get(g_sg_drag, &x, &y);
		sg_pos_set(g_sg_drag, x + io.MouseDelta.x / g_sg_zoom,
			   y + io.MouseDelta.y / g_sg_zoom);
	}

	if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
		if (g_sg_link_from >= 0) {
			int32_t  tn2;
			uint32_t tp2;

			if (sg_in_pin_at(io.MousePos, &tn2, &tp2))
				sg_try_connect(g_sg_link_from, g_sg_link_port,
					       tn2, tp2);
			g_sg_link_from = -1;
		}
		g_sg_drag = -1;
	}
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
			if (ImGui::BeginTabItem("Shader Graph")) {
				draw_tab_shader_graph();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Branches")) {
				draw_tab_branches();
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
	g_edit_api   = (const struct edit_api *)
		subsystem_manager_get_api(mgr, "edit");
	g_vscript    = (const struct vscript_api *)
		subsystem_manager_get_api(mgr, "vscript");
	g_shader_graph = (const struct shader_graph_api *)
		subsystem_manager_get_api(mgr, "shader_graph");
	g_codec      = (const struct asset_codec_api *)
		subsystem_manager_get_api(mgr, "asset_codec");
	g_mem        = (const struct memory_api *)
		subsystem_manager_get_api(mgr, "memory");
	g_mgr        = mgr;
#endif

	subsystem_manager_register(mgr, &desc);
}
