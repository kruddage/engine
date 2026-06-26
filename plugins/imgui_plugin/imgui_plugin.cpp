/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * imgui_plugin — Dear ImGui subsystem.
 *
 * Manages the ImGui frame lifecycle (NewFrame → registered panels → Render)
 * and Emscripten input handling.  ImGui is compiled into this side module;
 * kruddboard and any future panel plugins import ImGui symbols from here via
 * Emscripten's dynamic linker global symbol table (loaded before them).
 *
 * Other plugins register draw callbacks via imgui_api.register_panel;
 * they are called between ImGui::NewFrame() and ImGui::Render() each tick.
 *
 * Must be loaded after renderer_webgl so the WebGL 2 context exists.
 */

extern "C" {
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"
#include "log_level.h"
#include "imgui_api.h"
#include "stats_api.h"
}

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#include "canvas_api.h"
#endif

static const struct log_api    *g_log;
static const struct stats_api  *g_stats;
#ifdef __EMSCRIPTEN__
static const struct canvas_api *g_canvas;
#endif

struct panel_entry {
	const char *name;
	void      (*draw_fn)(void *userdata);
	void       *userdata;
};

static struct panel_entry s_panels[IMGUI_MAX_PANELS];
static int                s_panel_count;

static void imgui_register_panel(const char *name,
				 void (*draw_fn)(void *userdata),
				 void *userdata)
{
	if (s_panel_count >= IMGUI_MAX_PANELS)
		return;
	s_panels[s_panel_count].name     = name;
	s_panels[s_panel_count].draw_fn  = draw_fn;
	s_panels[s_panel_count].userdata = userdata;
	s_panel_count++;
}

static const imgui_api g_imgui_api = { imgui_register_panel };

#ifdef __EMSCRIPTEN__

static int on_mouse_move(const struct canvas_mouse_event *ev, void * /*ud*/)
{
	ImGui::GetIO().AddMousePosEvent((float)ev->x, (float)ev->y);
	return 0;
}

static int on_mouse_button(int pressed, const struct canvas_mouse_event *ev,
			    void * /*ud*/)
{
	if ((int)ev->button < 5)
		ImGui::GetIO().AddMouseButtonEvent((int)ev->button,
						   (bool)pressed);
	return 0;
}

static int on_touch(const struct canvas_touch_event *ev, void * /*ud*/)
{
	ImGuiIO &io = ImGui::GetIO();

	io.AddMousePosEvent((float)ev->x, (float)ev->y);

	if (ev->type == CANVAS_TOUCH_START)
		io.AddMouseButtonEvent(0, true);
	else if (ev->type == CANVAS_TOUCH_END || ev->type == CANVAS_TOUCH_CANCEL)
		io.AddMouseButtonEvent(0, false);

	return 1;
}

#endif /* __EMSCRIPTEN__ */

static void imgui_init(void)
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;	/* no writable filesystem in WASM */

	ImGui::StyleColorsDark();
	ImGui_ImplOpenGL3_Init("#version 300 es");

#ifdef __EMSCRIPTEN__
	g_canvas->set_mousemove_callback(on_mouse_move,   nullptr);
	g_canvas->set_mousedown_callback(on_mouse_button, nullptr);
	g_canvas->set_mouseup_callback(on_mouse_button,   nullptr);
	g_canvas->set_touchstart_callback(on_touch,  nullptr);
	g_canvas->set_touchmove_callback(on_touch,   nullptr);
	g_canvas->set_touchend_callback(on_touch,    nullptr);
	g_canvas->set_touchcancel_callback(on_touch, nullptr);
#endif

	g_log->write(LOG_LEVEL_INFO, "imgui_plugin: init");
}

static void imgui_tick(void)
{
#ifdef __EMSCRIPTEN__
	double css_w, css_h;
	double dpr;
	int    phys_w, phys_h;
	int    i;

	g_canvas->get_css_size(&css_w, &css_h);
	dpr    = g_canvas->get_device_pixel_ratio();
	phys_w = (int)(css_w * dpr + 0.5);
	phys_h = (int)(css_h * dpr + 0.5);
	g_canvas->set_size(phys_w, phys_h);

	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize             = ImVec2((float)css_w, (float)css_h);
	io.DisplayFramebufferScale = ImVec2((float)dpr,   (float)dpr);
	io.DeltaTime = (g_stats && g_stats->last_frame_ms > 0.0f)
		? g_stats->last_frame_ms / 1000.0f
		: 1.0f / 60.0f;

	ImGui_ImplOpenGL3_NewFrame();
	ImGui::NewFrame();

	for (i = 0; i < s_panel_count; i++) {
		if (s_panels[i].draw_fn)
			s_panels[i].draw_fn(s_panels[i].userdata);
	}

	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#endif
}

static void imgui_shutdown(void)
{
	g_log->write(LOG_LEVEL_INFO, "imgui_plugin: shutdown");
	ImGui_ImplOpenGL3_Shutdown();
	ImGui::DestroyContext();
}

static const struct subsystem desc = {
	"imgui",
	&g_imgui_api,
	imgui_init,
	imgui_tick,
	imgui_shutdown,
};

#ifdef __EMSCRIPTEN__
extern "C" void plugin_entry(struct subsystem_manager *mgr)
#else
extern "C" void imgui_plugin_entry(struct subsystem_manager *mgr)
#endif
{
#ifdef __EMSCRIPTEN__
	g_log    = (const struct log_api *)
		subsystem_manager_get_api(mgr, "log");
	g_stats  = (const struct stats_api *)
		subsystem_manager_get_api(mgr, "stats");
	g_canvas = (const struct canvas_api *)
		subsystem_manager_get_api(mgr, "canvas");
#endif
	subsystem_manager_register(mgr, &desc);
}
