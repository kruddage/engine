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
 *   KRUDD      — frame stats, subsystems, log (collapsible sections)
 *   World      — entity list, create/delete, inspector
 *   Assets     — asset browser and markdown editor
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
#ifdef __EMSCRIPTEN__
#include "backend_api.h"
#include "script.h"
#endif
}

#include "imgui.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#include <string.h>
#include <math.h>
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
static const struct camera_api        *g_camera_api; /* NULL = no viewport gizmo */

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

#ifdef __EMSCRIPTEN__
/*
 * Shader authoring metadata.  A krudd shader is a single DSL source that
 * embeds every stage it defines (see shader.scm) — there is no per-asset
 * stage or dialect to pick, so the editor has nothing to ask for beyond a
 * name; the source itself is the only thing to author.
 */
#define SHADER_FORMAT "krudd-shader"

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
#endif /* __EMSCRIPTEN__ */

/*
 * Markdown editor state.  Tracks which asset is loaded into the edit
 * buffer so we only reload on selection change, not every frame.
 */
#ifdef __EMSCRIPTEN__
#define EDIT_BUF_MAX (64 * 1024)
static char     g_edit[EDIT_BUF_MAX];
static uint32_t g_edit_id; /* id whose bytes are in g_edit; 0 = none */
static int      g_shader_compile_ok = -1; /* -1 = untried, 0 = fail, 1 = ok */

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
			ImGui::BeginDisabled();
			ImGui::Button("Save");
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextDisabled("read-only");
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
	 * A small form collects just the name and the asset type (Text /
	 * Shader); a krudd shader's stages live inside its DSL source, not in
	 * a per-asset field, so there is nothing else to ask for here — the
	 * new asset opens straight into the same editor screen used to view
	 * one, ready for source to be typed in and saved.
	 */
	if (g_asset_sel == 0 && g_asset_mut) {
		static char new_name[256];
		static int  naming;   /* 1 while the form is visible */
		static int  new_type; /* 0 = Text, 1 = Shader */

		static const char *const NEW_TYPES[] = { "Text", "Shader" };

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
			ImGui::Combo("type", &new_type, NEW_TYPES, 2);

			confirm |= ImGui::Button("Create");
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				naming = 0;

			if (confirm && new_name[0] != '\0') {
				char        path[256];
				int32_t     atype;
				uint32_t    nid;
				int         can_persist;
				const char *seed;
				uint32_t    seed_len;

				atype = (new_type == 1) ? ASSET_TYPE_SHADER
							: ASSET_TYPE_TEXT;
				seed  = (atype == ASSET_TYPE_SHADER)
						? default_shader_src() : "";
				seed_len = (uint32_t)strlen(seed);

				snprintf(path, sizeof(path), "%s", new_name);

				nid = g_asset_mut->create(path, atype, seed,
							  seed_len);
				if (nid != 0) {
					can_persist =
						g_backend &&
						(g_backend->get_caps() &
						 BACKEND_CAP_PROJECT_PERSIST);
					if (can_persist)
						g_backend->persist_asset(
							nid, path, atype,
							seed, seed_len);
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

/*
 * Read-only identity / hierarchy / component summary for the selected entity.
 * The world exposes more per-entity state than the editable name+transform —
 * the dense id, the parent link, and the component mask — so surface it here.
 */
static void draw_inspector_details(const struct world *w, uint32_t e)
{
	char comps[64];
	char parent_buf[64];

	snprintf(comps, sizeof(comps), "Transform%s%s",
		 (w->mask[e] & COMPONENT_NAME)   ? ", Name"   : "",
		 (w->mask[e] & COMPONENT_RENDER) ? ", Render" : "");

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
 */
static void gizmo_update_and_draw(void)
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
		return;

	sel = g_entity_api->get_selected();
	w   = g_entity_api->get_world();
	if (!w || sel < 0 || (uint32_t)sel >= w->count || !w->alive[(uint32_t)sel])
		return;
	e = (uint32_t)sel;

	/* Anchor at the entity's world origin; write edits to its local xform. */
	origin[0] = w->world_xform[e].position[0];
	origin[1] = w->world_xform[e].position[1];
	origin[2] = w->world_xform[e].position[2];

	g_camera_api->get_view_proj(&vp);
	g_camera_api->get_eye(eye);
	len = gizmo_handle_len(eye, origin);

	if (!gizmo_project(vp.m, origin, io.DisplaySize, &o2d))
		return; /* entity behind the camera */

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
}

/* Move / Rotate / Scale selector — shared state with the viewport handles. */
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
	gizmo_update_and_draw();

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
	g_mgr        = mgr;
#endif

	subsystem_manager_register(mgr, &desc);
}
