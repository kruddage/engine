/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * kruddboard — engine debug overlay plugin.
 *
 * Single full-width ImGui window anchored to the top of the viewport.
 * Height auto-sizes to the active tab's content; the Log tab caps at
 * ~88 % of the viewport height and scrolls internally.  The tab bar
 * uses FittingPolicyScroll to handle overflow on narrow / phone screens.
 * Toggle visibility with backtick (`).
 */

extern "C" {
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"
#include "log_level.h"
#include "stats_api.h"
#include "imgui_api.h"
#include "asset_api.h"
}

#include "imgui.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#include <string.h>
#endif

#include <cstdio>

static const struct log_api           *g_log;
static const struct stats_api         *g_stats;
static const struct asset_api         *g_asset_api;
static const struct subsystem_manager *g_mgr;
static int                             g_visible = 1;
static int                             g_panels_registered;
static int                             g_asset_sel = -1;

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
	default:                  return "Unknown";
	}
}

static void draw_asset_inspector(int sel)
{
	struct asset_info       info;
	struct asset_decl_field fields[16];
	uint32_t                nf;
	uint32_t                i;
	char                    buf[32];

	if (g_asset_api->info((uint32_t)sel, &info) != 0)
		return;

	if (ImGui::SmallButton("<- Back"))
		g_asset_sel = -1;
	ImGui::SameLine();
	ImGui::TextUnformatted(info.path);
	ImGui::SameLine();
	ImGui::TextDisabled("[%s | %s | %s]",
		asset_type_str(info.type),
		asset_state_str(info.state),
		info.read_only ? "read-only" : "mutable");

	ImGui::Separator();
	ImGui::TextUnformatted("Declaration");

	nf = g_asset_api->describe((uint32_t)sel, fields, 16);
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

	n = g_asset_api->count();
	if (n == 0) {
		ImGui::TextDisabled("(no assets)");
		return;
	}

	/* Guard stale selection after a catalog change. */
	if (g_asset_sel >= (int)n)
		g_asset_sel = -1;

	if (g_asset_sel >= 0) {
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
				g_asset_sel = (int)i;
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
				g_asset_sel = (int)i;
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

	ImGui::TextDisabled("KRUDD EDITOR");
	hint_x = ImGui::GetWindowWidth()
		- ImGui::CalcTextSize("` to hide").x
		- ImGui::GetStyle().WindowPadding.x;
	ImGui::SameLine(hint_x);
	ImGui::TextDisabled("` to hide");
	ImGui::Separator();

	if (ImGui::BeginTabBar("##tabs",
			       ImGuiTabBarFlags_FittingPolicyScroll)) {
		if (ImGui::BeginTabItem("Frame Stats")) {
			draw_tab_stats();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Log")) {
			draw_tab_log();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Subsystems")) {
			draw_tab_subsystems();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Assets")) {
			draw_tab_assets();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
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
	g_mgr       = mgr;
#endif

	subsystem_manager_register(mgr, &desc);
}
