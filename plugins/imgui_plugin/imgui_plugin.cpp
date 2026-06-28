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
#include <emscripten/html5.h>
#include <GLES3/gl3.h>

/*
 * Defined in plugin_abi.c (main module) via EM_JS; side modules must not
 * use EM_ASM_* — see plugin_abi.c for the explanation.
 */
extern "C" double get_device_pixel_ratio(void);

/*
 * Web text-input bridge (plugin_abi.c, main module).
 * krudd_text_input_init  — create hidden <input>, wire listeners (call once).
 * krudd_text_input_show  — focus() the element (raises soft keyboard on mobile).
 * krudd_text_input_hide  — blur() the element (dismisses soft keyboard).
 * krudd_text_input_drain_chars — copy pending UTF-8 into buf; return byte count.
 * krudd_text_input_pop_key     — dequeue one key code (1-11) or 0 if empty.
 */
extern "C" void krudd_text_input_init(void);
extern "C" void krudd_text_input_show(void);
extern "C" void krudd_text_input_hide(void);
extern "C" int  krudd_text_input_drain_chars(char *buf, int cap);
extern "C" int  krudd_text_input_pop_key(void);
#endif

static const struct log_api   *g_log;
static const struct stats_api *g_stats;

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

	/*
	 * iOS soft-keyboard workaround: iOS only raises the keyboard when
	 * focus() is called inside a user-gesture handler.  ImGui activates
	 * InputText the frame AFTER the tap, so the edge-based show() in
	 * imgui_tick runs outside the gesture window and the keyboard never
	 * appears.  Calling show() here — inside the TOUCHEND gesture — lets
	 * iOS raise the keyboard; if WantTextInput turns out to be false this
	 * frame, the edge logic in imgui_tick will call hide() immediately.
	 * This is best-effort and may need per-device tuning.
	 */
	if (type == EMSCRIPTEN_EVENT_TOUCHEND)
		krudd_text_input_show();

	return EM_TRUE;
}

#endif /* __EMSCRIPTEN__ */

static void imgui_init(void)
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename  = nullptr;	/* no writable filesystem in WASM */
	io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;

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

	/* Create the hidden <input> element for keyboard / soft-keyboard capture. */
	krudd_text_input_init();
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
	/* Text-input state persisted across ticks for edge detection. */
	static bool s_prev_want_text;

	emscripten_get_element_css_size("#canvas", &css_w, &css_h);
	dpr    = get_device_pixel_ratio();
	phys_w = (int)(css_w * dpr + 0.5);
	phys_h = (int)(css_h * dpr + 0.5);
	emscripten_set_canvas_element_size("#canvas", phys_w, phys_h);

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

	/*
	 * Text-input bridge — runs after Render() so io.WantTextInput
	 * reflects this frame's active widgets.
	 *
	 * Focus management: on the rising edge of WantTextInput, focus the
	 * hidden <input> element (desktop: begin capturing keys; mobile:
	 * raise soft keyboard).  On the falling edge, blur it.
	 *
	 * Char drain: pull any UTF-8 text typed this frame and feed it to
	 * ImGui via AddInputCharactersUTF8.
	 *
	 * Key drain: pull queued navigation / editing key codes and inject
	 * them as ImGui key-down + key-up pairs.  Key-code mapping matches
	 * the table in krudd_text_input_init (plugin_abi.c):
	 *   1=Backspace  2=Enter       3=Tab        4=Delete
	 *   5=LeftArrow  6=RightArrow  7=UpArrow    8=DownArrow
	 *   9=Home       10=End        11=Escape
	 */
	{
		bool want = io.WantTextInput;

		if (want && !s_prev_want_text)
			krudd_text_input_show();
		else if (!want && s_prev_want_text)
			krudd_text_input_hide();
		s_prev_want_text = want;

		/* Drain printable / IME characters. */
		{
			char buf[256];

			if (krudd_text_input_drain_chars(buf, (int)sizeof(buf)) > 0)
				io.AddInputCharactersUTF8(buf);
		}

		/* Drain navigation / editing key codes. */
		{
			static const ImGuiKey key_map[] = {
				ImGuiKey_None,       /* 0 — sentinel / empty */
				ImGuiKey_Backspace,  /* 1 */
				ImGuiKey_Enter,      /* 2 */
				ImGuiKey_Tab,        /* 3 */
				ImGuiKey_Delete,     /* 4 */
				ImGuiKey_LeftArrow,  /* 5 */
				ImGuiKey_RightArrow, /* 6 */
				ImGuiKey_UpArrow,    /* 7 */
				ImGuiKey_DownArrow,  /* 8 */
				ImGuiKey_Home,       /* 9 */
				ImGuiKey_End,        /* 10 */
				ImGuiKey_Escape,     /* 11 */
			};
			int k;

			while ((k = krudd_text_input_pop_key()) != 0) {
				if (k > 0 && k < (int)(sizeof(key_map) /
						       sizeof(key_map[0]))) {
					io.AddKeyEvent(key_map[k], true);
					io.AddKeyEvent(key_map[k], false);
				}
			}
		}
	}
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
	g_log   = (const struct log_api *)
		subsystem_manager_get_api(mgr, "log");
	g_stats = (const struct stats_api *)
		subsystem_manager_get_api(mgr, "stats");
#endif
	subsystem_manager_register(mgr, &desc);
}
