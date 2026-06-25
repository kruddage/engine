/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * kruddboard — engine debug overlay plugin.
 *
 * Registers three always-on ImGui panels via imgui_api:
 *   "Frame Stats"  — FPS, frame time ms, frame count
 *   "Log"          — scrollable live log stream with level filter
 *   "Subsystems"   — loaded plugins, API presence, tick vs. init-only
 *
 * Must be loaded after imgui_plugin.
 */

extern "C" {
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"
#include "log_level.h"
#include "stats_api.h"
#include "imgui_api.h"
}

#include "imgui.h"

static const struct log_api          *g_log;
static const struct stats_api        *g_stats;
static const struct subsystem_manager *g_mgr;

/* ------------------------------------------------------------------ */
/* Frame Stats panel (#114)                                            */
/* ------------------------------------------------------------------ */

static void draw_stats(void * /*userdata*/)
{
	if (!g_stats)
		return;

	ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Once);
	ImGui::SetNextWindowSize(ImVec2(220.0f, 80.0f), ImGuiCond_Once);
	ImGui::Begin("Frame Stats");
	ImGui::Text("FPS (avg): %.1f",  (double)g_stats->fps_avg);
	ImGui::Text("Frame ms:  %.2f",  (double)g_stats->last_frame_ms);
	ImGui::Text("Frame:     %u",    g_stats->frame_count);
	ImGui::End();
}

/* ------------------------------------------------------------------ */
/* Log viewer panel (#115)                                             */
/* ------------------------------------------------------------------ */

static void draw_log(void * /*userdata*/)
{
	static struct log_message msgs[LOG_HISTORY_CAP];
	static int   filter     = LOG_LEVEL_DEBUG;
	static bool  autoscroll = true;
	uint32_t     count;
	uint32_t     i;

	if (!g_log)
		return;

	count = g_log->get_history(msgs, LOG_HISTORY_CAP);

	ImGui::SetNextWindowPos(ImVec2(10.0f, 100.0f), ImGuiCond_Once);
	ImGui::SetNextWindowSize(ImVec2(500.0f, 300.0f), ImGuiCond_Once);
	ImGui::Begin("Log");

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

	ImGui::BeginChild("logscroll", ImVec2(0.0f, 0.0f), false,
			  ImGuiWindowFlags_HorizontalScrollbar);

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
	ImGui::End();
}

/* ------------------------------------------------------------------ */
/* Subsystem list panel (#116)                                         */
/* ------------------------------------------------------------------ */

static void draw_subsystems(void * /*userdata*/)
{
	int i;

	if (!g_mgr)
		return;

	ImGui::SetNextWindowPos(ImVec2(520.0f, 10.0f), ImGuiCond_Once);
	ImGui::SetNextWindowSize(ImVec2(280.0f, 300.0f), ImGuiCond_Once);
	ImGui::Begin("Subsystems");

	if (ImGui::BeginTable("subsys", 3,
			      ImGuiTableFlags_Borders |
			      ImGuiTableFlags_RowBg   |
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
			ImGui::TextUnformatted(g_mgr->static_table[i].api  ? "yes" : "-");
			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(g_mgr->static_table[i].tick ? "yes" : "-");
		}

		for (i = 0; i < g_mgr->dynamic_count; i++) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(g_mgr->dynamic[i].name);
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(g_mgr->dynamic[i].api  ? "yes" : "-");
			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(g_mgr->dynamic[i].tick ? "yes" : "-");
		}

		ImGui::EndTable();
	}

	ImGui::End();
}

/* ------------------------------------------------------------------ */
/* Plugin lifecycle (#113)                                             */
/* ------------------------------------------------------------------ */

static void kruddboard_init(void)
{
	if (g_log)
		g_log->write(LOG_LEVEL_INFO, "kruddboard: init");
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
	nullptr, /* panels draw via imgui_api callbacks, not tick */
	kruddboard_shutdown,
};

#ifdef __EMSCRIPTEN__
extern "C" void plugin_entry(struct subsystem_manager *mgr)
#else
extern "C" void kruddboard_entry(struct subsystem_manager *mgr)
#endif
{
	const struct imgui_api *imgui;

#ifdef __EMSCRIPTEN__
	g_log   = (const struct log_api *)
		subsystem_manager_get_api(mgr, "log");
	g_stats = (const struct stats_api *)
		subsystem_manager_get_api(mgr, "stats");
	g_mgr   = mgr;

	imgui = (const struct imgui_api *)
		subsystem_manager_get_api(mgr, "imgui");

	if (!imgui) {
		if (g_log)
			g_log->write(LOG_LEVEL_WARN,
				     "kruddboard: imgui_api not found, panels disabled");
	} else {
		imgui->register_panel("Frame Stats", draw_stats,     nullptr);
		imgui->register_panel("Log",         draw_log,       nullptr);
		imgui->register_panel("Subsystems",  draw_subsystems, nullptr);
	}
#endif

	subsystem_manager_register(mgr, &desc);
}
