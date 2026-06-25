/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * imgui_plugin — Dear ImGui proof-of-life subsystem.
 *
 * Initialises Dear ImGui with the OpenGL ES 3 backend and renders a
 * "Hello World" window each frame.  Mouse input is forwarded via
 * Emscripten HTML5 callbacks so the window is interactive.
 *
 * Must be loaded after renderer_webgl so the WebGL 2 context exists.
 */

extern "C" {
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"
#include "log_level.h"
}

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#include <GLES3/gl3.h>
#endif

static const struct log_api *g_log;

#ifdef __EMSCRIPTEN__

static EM_BOOL on_mouse_move(int /*type*/, const EmscriptenMouseEvent *e,
			     void * /*ud*/)
{
	ImGui::GetIO().AddMousePosEvent((float)e->canvasX, (float)e->canvasY);
	return EM_FALSE;
}

static EM_BOOL on_mouse_button(int type, const EmscriptenMouseEvent *e,
			       void * /*ud*/)
{
	bool pressed = (type == EMSCRIPTEN_EVENT_MOUSEDOWN);

	if ((int)e->button < 5)
		ImGui::GetIO().AddMouseButtonEvent((int)e->button, pressed);
	return EM_FALSE;
}

static EM_BOOL on_touch(int type, const EmscriptenTouchEvent *e,
			void * /*ud*/)
{
	if (e->numTouches < 1)
		return EM_FALSE;

	const EmscriptenTouchPoint *t = &e->touches[0];
	ImGuiIO &io = ImGui::GetIO();

	io.AddMousePosEvent((float)t->targetX, (float)t->targetY);

	if (type == EMSCRIPTEN_EVENT_TOUCHSTART)
		io.AddMouseButtonEvent(0, true);
	else if (type == EMSCRIPTEN_EVENT_TOUCHEND ||
		 type == EMSCRIPTEN_EVENT_TOUCHCANCEL)
		io.AddMouseButtonEvent(0, false);

	return EM_TRUE;
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
	emscripten_set_mousemove_callback("#canvas", nullptr, 0, on_mouse_move);
	emscripten_set_mousedown_callback("#canvas", nullptr, 0, on_mouse_button);
	emscripten_set_mouseup_callback("#canvas",   nullptr, 0, on_mouse_button);
	emscripten_set_touchstart_callback("#canvas",  nullptr, 0, on_touch);
	emscripten_set_touchmove_callback("#canvas",   nullptr, 0, on_touch);
	emscripten_set_touchend_callback("#canvas",    nullptr, 0, on_touch);
	emscripten_set_touchcancel_callback("#canvas", nullptr, 0, on_touch);
#endif

	g_log->write(LOG_LEVEL_INFO, "imgui_plugin: init");
}

static void imgui_tick(void)
{
#ifdef __EMSCRIPTEN__
	double css_w, css_h;
	double dpr;
	int phys_w, phys_h;

	emscripten_get_element_css_size("#canvas", &css_w, &css_h);
	dpr    = EM_ASM_DOUBLE({ return window.devicePixelRatio || 1.0; });
	phys_w = (int)(css_w * dpr + 0.5);
	phys_h = (int)(css_h * dpr + 0.5);
	emscripten_set_canvas_element_size("#canvas", phys_w, phys_h);

	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize             = ImVec2((float)css_w, (float)css_h);
	io.DisplayFramebufferScale = ImVec2((float)dpr,   (float)dpr);
	io.DeltaTime               = 1.0f / 60.0f;

	ImGui_ImplOpenGL3_NewFrame();
	ImGui::NewFrame();

	static int  s_counter  = 0;
	static bool s_checkbox = false;
	static float s_slider  = 0.5f;

	ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Once);
	ImGui::Begin("KRUDD");
	ImGui::Text("Hello, World!");
	ImGui::Text("Engine running.");
	ImGui::Separator();
	if (ImGui::Button("Tap me"))
		s_counter++;
	ImGui::SameLine();
	ImGui::Text("count: %d", s_counter);
	ImGui::Checkbox("checkbox", &s_checkbox);
	ImGui::SliderFloat("slider", &s_slider, 0.0f, 1.0f);
	ImGui::End();

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
	nullptr,
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
	g_log = (const struct log_api *)
		subsystem_manager_get_api(mgr, "log");
#endif
	subsystem_manager_register(mgr, &desc);
}
