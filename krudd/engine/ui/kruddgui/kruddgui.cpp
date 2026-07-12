/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * kruddgui — a touch-first UI layer drawn OVER Dear ImGui.
 *
 * krudd is a mobile-first editor, but its debug UI (kruddboard) is Dear ImGui,
 * which is mouse-first. kruddgui is the seam through which we grow our own UI:
 * it draws with its own batched 2D quad primitives straight to WebGL, layered
 * on top of ImGui, and its panels are authored in Scheme (kruddgui.scm) against
 * a handful of C primitives registered here. This is #490's proof of life — a
 * finger-sized MOVE / ROTATE / SCALE mode-bar wired to the shared gizmo tool.
 *
 * Loaded after renderer_webgl (the GL 2 context) and after imgui_plugin: its
 * tick runs after imgui's, so ImGui_ImplOpenGL3_RenderDrawData has already
 * drawn and kruddgui composites over it.
 *
 * Two deliberate, isolated ImGui couplings for v0, both confined to this file:
 *   - text reuses ImGui's baked font atlas (glyph_source below) rather than
 *     standing up a new glyph pipeline;
 *   - non-bar input is forwarded verbatim to ImGui's io, using the exact
 *     translation imgui_plugin used before input ownership moved here.
 */

extern "C" {
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"
#include "log_level.h"
#include "kgui_batch.h"
}

#ifdef __EMSCRIPTEN__
#include "imgui.h"

#include <emscripten/html5.h>
#include <GLES3/gl3.h>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "s7.h"			/* self-guards for C++ linkage */
#include "script.h"
#include "kruddgui_scm.h"	/* KRUDDGUI_SCM — the panel image */

double get_device_pixel_ratio(void);	/* plugin_abi.c (main module) */
int    krudd_is_touch_device(void);

/*
 * The soft-keyboard speculative raise on a forwarded touch-end still lives in
 * imgui_plugin (which owns the grace countdown and the WantTextInput reconcile
 * in its tick); kruddgui pokes it when it forwards a touch to ImGui.
 */
void imgui_soft_keyboard_touch_hint(void);
}
#endif

static const struct log_api *g_log;

#ifdef __EMSCRIPTEN__

/* ------------------------------------------------------------------ */
/* GL batch renderer                                                   */
/* ------------------------------------------------------------------ */

/*
 * One VBO, one program, one draw call per flush. The batch is built in
 * CSS-pixel space (kgui_batch, GL-free) and this file only uploads and draws
 * it, plus supplies the ImGui font atlas as the sampled texture.
 */
#define KGUI_MAX_VERTS 16384

static GLuint s_prog;
static GLuint s_vao;
static GLuint s_vbo;
static GLint  s_u_viewport;
static GLint  s_u_tex;
static bool   s_gl_ready;

static struct kgui_vertex s_verts[KGUI_MAX_VERTS];
static struct kgui_batch  s_batch;

/* Viewport for this tick, refreshed at the top of kruddgui_tick. */
static float s_css_w, s_css_h;
static int   s_phys_w, s_phys_h;

/* Font atlas coupling, refreshed each tick from ImGui's io. */
static float  s_white_u, s_white_v;
static float  s_text_size;

/*
 * Glyph text scale over the atlas's baked size. The atlas is baked once at a
 * desktop-ish size; a modest upscale keeps mode-bar labels legible at arm's
 * distance without a second glyph pipeline (that is #488's follow-on, not v0).
 */
#define KGUI_TEXT_SCALE 1.5f

static const char *k_vert_src =
	"#version 300 es\n"
	"layout(location=0) in vec2 a_pos;\n"
	"layout(location=1) in vec2 a_uv;\n"
	"layout(location=2) in vec4 a_col;\n"
	"uniform vec2 u_viewport;\n"
	"out vec2 v_uv;\n"
	"out vec4 v_col;\n"
	"void main() {\n"
	"    vec2 p = a_pos / u_viewport;\n"      /* 0..1 in CSS space */
	"    p = p * 2.0 - 1.0;\n"                /* -1..1             */
	"    gl_Position = vec4(p.x, -p.y, 0.0, 1.0);\n" /* y down     */
	"    v_uv = a_uv;\n"
	"    v_col = a_col;\n"
	"}\n";

static const char *k_frag_src =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec2 v_uv;\n"
	"in vec4 v_col;\n"
	"uniform sampler2D u_tex;\n"
	"out vec4 frag;\n"
	"void main() {\n"
	"    frag = v_col * texture(u_tex, v_uv);\n"
	"}\n";

static GLuint compile_shader(GLenum type, const char *src)
{
	GLuint sh = glCreateShader(type);
	GLint  ok = 0;

	glShaderSource(sh, 1, &src, nullptr);
	glCompileShader(sh);
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char info[512];

		glGetShaderInfoLog(sh, (GLsizei)sizeof(info), nullptr, info);
		if (g_log)
			g_log->write(LOG_LEVEL_ERROR,
				     "kruddgui: shader compile: %s", info);
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

static void gl_init(void)
{
	GLuint vs, fs;

	vs = compile_shader(GL_VERTEX_SHADER, k_vert_src);
	if (!vs)
		return;
	fs = compile_shader(GL_FRAGMENT_SHADER, k_frag_src);
	if (!fs) {
		glDeleteShader(vs);
		return;
	}

	s_prog = glCreateProgram();
	glAttachShader(s_prog, vs);
	glAttachShader(s_prog, fs);
	glLinkProgram(s_prog);
	glDeleteShader(vs);
	glDeleteShader(fs);

	s_u_viewport = glGetUniformLocation(s_prog, "u_viewport");
	s_u_tex      = glGetUniformLocation(s_prog, "u_tex");

	glGenVertexArrays(1, &s_vao);
	glGenBuffers(1, &s_vbo);
	glBindVertexArray(s_vao);
	glBindBuffer(GL_ARRAY_BUFFER, s_vbo);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
			      sizeof(struct kgui_vertex),
			      (void *)offsetof(struct kgui_vertex, x));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
			      sizeof(struct kgui_vertex),
			      (void *)offsetof(struct kgui_vertex, u));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE,
			      sizeof(struct kgui_vertex),
			      (void *)offsetof(struct kgui_vertex, r));

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	s_gl_ready = true;
}

/*
 * Upload and draw the accumulated batch as one triangle list. Sets every piece
 * of GL state it needs (blend, no depth/cull/scissor, viewport, program, VAO,
 * atlas texture) so it is independent of whatever ImGui left bound; it runs at
 * the end of the frame and the next frame's renderers set their own state.
 */
static void gl_flush(GLuint atlas)
{
	if (!s_gl_ready || s_batch.count <= 0 || !atlas)
		return;

	glViewport(0, 0, s_phys_w, s_phys_h);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glUseProgram(s_prog);
	glUniform2f(s_u_viewport, s_css_w, s_css_h);
	glUniform1i(s_u_tex, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, atlas);

	glBindVertexArray(s_vao);
	glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
	glBufferData(GL_ARRAY_BUFFER,
		     (GLsizeiptr)(s_batch.count * sizeof(struct kgui_vertex)),
		     s_verts, GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, s_batch.count);

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/* ------------------------------------------------------------------ */
/* ImGui font atlas — the one intentional, isolated text coupling      */
/* ------------------------------------------------------------------ */

static ImFont *atlas_font(void)
{
	ImGuiIO &io = ImGui::GetIO();

	if (!io.Fonts || io.Fonts->Fonts.Size == 0)
		return nullptr;
	return io.Fonts->Fonts[0];
}

static GLuint atlas_texture(void)
{
	return (GLuint)(intptr_t)ImGui::GetIO().Fonts->TexID;
}

/*
 * kgui_glyph_fn over an ImFont: map a code point to its baked atlas quad,
 * scaled to the requested pixel size. This function — plus atlas_texture and
 * the TexUvWhitePixel read in refresh_atlas — is the whole of kruddgui's ImGui
 * text dependency; everything else draws through kgui_batch.
 */
static int glyph_source(void *ud, uint32_t cp, float size,
			struct kgui_glyph *out)
{
	ImFont *f = (ImFont *)ud;
	const ImFontGlyph *g;
	float   s;

	if (!f)
		return 0;
	g = f->FindGlyph((ImWchar)cp);
	if (!g)
		return 0;

	s = (f->FontSize > 0.0f) ? size / f->FontSize : 1.0f;
	out->x0      = g->X0 * s;
	out->y0      = g->Y0 * s;
	out->x1      = g->X1 * s;
	out->y1      = g->Y1 * s;
	out->u0      = g->U0;
	out->v0      = g->V0;
	out->u1      = g->U1;
	out->v1      = g->V1;
	out->advance = g->AdvanceX * s;
	out->visible = (g->X1 > g->X0 && g->Y1 > g->Y0);
	return 1;
}

static void refresh_atlas(void)
{
	ImGuiIO &io = ImGui::GetIO();
	ImFont  *f  = atlas_font();

	if (io.Fonts) {
		s_white_u = io.Fonts->TexUvWhitePixel.x;
		s_white_v = io.Fonts->TexUvWhitePixel.y;
	}
	s_text_size = (f ? f->FontSize : 13.0f) * KGUI_TEXT_SCALE;
}

/* ------------------------------------------------------------------ */
/* Pointer state + mode-bar hit region                                 */
/* ------------------------------------------------------------------ */

/*
 * Input ownership for v0 is scoped (the general panel hit-test + multi-touch
 * model is #489): kruddgui owns the Emscripten pointer callbacks, consumes the
 * gesture only when it starts on the mode-bar, and forwards everything else to
 * ImGui verbatim. The bar's footprint is the union of the button rects the
 * Scheme image lays out; it is rebuilt every tick and read by the async
 * callbacks between ticks, which is safe because the callbacks never fire
 * mid-tick (single-threaded).
 */
static float s_bar_x0, s_bar_y0, s_bar_x1, s_bar_y1;
static bool  s_bar_valid;

static float s_ptr_x, s_ptr_y;
static bool  s_gesture_active;   /* a button / touch is currently down     */
static bool  s_gesture_owned;    /* that gesture started inside the bar    */

static bool  s_tap_pending;      /* a completed bar tap awaiting a consumer */
static float s_tap_x, s_tap_y;

static bool s_touch_device;

static void bar_reset(void)
{
	s_bar_valid = false;
	s_bar_x0 = s_bar_y0 = s_bar_x1 = s_bar_y1 = 0.0f;
}

static void bar_add_rect(float x, float y, float w, float h)
{
	float x1 = x + w;
	float y1 = y + h;

	if (!s_bar_valid) {
		s_bar_x0 = x;  s_bar_y0 = y;
		s_bar_x1 = x1; s_bar_y1 = y1;
		s_bar_valid = true;
		return;
	}
	if (x  < s_bar_x0) s_bar_x0 = x;
	if (y  < s_bar_y0) s_bar_y0 = y;
	if (x1 > s_bar_x1) s_bar_x1 = x1;
	if (y1 > s_bar_y1) s_bar_y1 = y1;
}

static bool in_bar(float x, float y)
{
	return s_bar_valid &&
	       x >= s_bar_x0 && x <= s_bar_x1 &&
	       y >= s_bar_y0 && y <= s_bar_y1;
}

/* ------------------------------------------------------------------ */
/* Input callbacks — take over from imgui_plugin, forward the rest     */
/* ------------------------------------------------------------------ */

static void pointer_move(float x, float y)
{
	s_ptr_x = x;
	s_ptr_y = y;
	/* A bar-owned drag is consumed whole; otherwise the move is ImGui's. */
	if (s_gesture_active && s_gesture_owned)
		return;
	ImGui::GetIO().AddMousePosEvent(x, y);
}

static void pointer_down(float x, float y)
{
	s_ptr_x = x;
	s_ptr_y = y;
	s_gesture_active = true;

	if (in_bar(x, y)) {
		s_gesture_owned = true;	/* consume: not forwarded to ImGui */
		return;
	}
	s_gesture_owned = false;
	ImGui::GetIO().AddMousePosEvent(x, y);
	ImGui::GetIO().AddMouseButtonEvent(0, true);
}

static void pointer_up(float x, float y, bool is_touch)
{
	bool owned = s_gesture_owned;

	s_ptr_x = x;
	s_ptr_y = y;

	if (owned) {
		/* A tap is a press and release both on the bar. */
		if (in_bar(x, y)) {
			s_tap_pending = true;
			s_tap_x = x;
			s_tap_y = y;
		}
		/* consume: no button event reaches ImGui */
	} else {
		ImGuiIO &io = ImGui::GetIO();

		io.AddMousePosEvent(x, y);
		io.AddMouseButtonEvent(0, false);
		if (is_touch)
			imgui_soft_keyboard_touch_hint();
	}
	s_gesture_active = false;
	s_gesture_owned  = false;
}

/*
 * Pointer coordinates come from targetX/targetY (element-relative CSS pixels),
 * not the deprecated canvasX/canvasY — the latter read 0 on current Emscripten.
 * This is the same field the touch path and ImGui's own backend use, so the
 * bar hit-test and the forwarded io positions share one CSS-pixel space with
 * imgui_tick's DisplaySize.
 */
static EM_BOOL on_mouse_move(int /*type*/, const EmscriptenMouseEvent *e,
			     void * /*ud*/)
{
	pointer_move((float)e->targetX, (float)e->targetY);
	return EM_FALSE;
}

static EM_BOOL on_mouse_button(int type, const EmscriptenMouseEvent *e,
			       void * /*ud*/)
{
	bool pressed = (type == EMSCRIPTEN_EVENT_MOUSEDOWN);

	/* Only the left button participates in bar taps; forward the rest. */
	if ((int)e->button == 0) {
		if (pressed)
			pointer_down((float)e->targetX, (float)e->targetY);
		else
			pointer_up((float)e->targetX, (float)e->targetY, false);
	} else if ((int)e->button < 5) {
		ImGuiIO &io = ImGui::GetIO();

		io.AddMousePosEvent((float)e->targetX, (float)e->targetY);
		io.AddMouseButtonEvent((int)e->button, pressed);
	}
	return EM_FALSE;
}

static EM_BOOL on_touch(int type, const EmscriptenTouchEvent *e, void * /*ud*/)
{
	const EmscriptenTouchPoint *t;

	if (e->numTouches < 1)
		return EM_FALSE;

	t = &e->touches[0];

	if (type == EMSCRIPTEN_EVENT_TOUCHSTART)
		pointer_down((float)t->targetX, (float)t->targetY);
	else if (type == EMSCRIPTEN_EVENT_TOUCHEND ||
		 type == EMSCRIPTEN_EVENT_TOUCHCANCEL)
		pointer_up((float)t->targetX, (float)t->targetY, true);
	else
		pointer_move((float)t->targetX, (float)t->targetY);

	return EM_TRUE;
}

/* ------------------------------------------------------------------ */
/* Scheme primitives — the panel-authoring seam                        */
/* ------------------------------------------------------------------ */

extern "C" {

/* (kgui-rect x y w h r g b a) -> unspecified. Filled rectangle. */
static s7_pointer sp_kgui_rect(s7_scheme *sc, s7_pointer args)
{
	s7_pointer p = args;
	double x = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double y = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double w = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double h = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double r = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double g = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double b = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double a = s7_number_to_real(sc, s7_car(p));

	kgui_batch_quad(&s_batch, (float)x, (float)y, (float)w, (float)h,
			s_white_u, s_white_v, s_white_u, s_white_v,
			(float)r, (float)g, (float)b, (float)a);
	return s7_unspecified(sc);
}

/* (kgui-text x y str r g b a) -> unspecified. (x, y) is the text's top-left. */
static s7_pointer sp_kgui_text(s7_scheme *sc, s7_pointer args)
{
	s7_pointer p = args;
	double x = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double y = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	s7_pointer str = s7_car(p); p = s7_cdr(p);
	double r = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double g = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double b = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double a = s7_number_to_real(sc, s7_car(p));

	if (s7_is_string(str))
		kgui_batch_text(&s_batch, (float)x, (float)y, s7_string(str),
				s_text_size, (float)r, (float)g, (float)b,
				(float)a, glyph_source, atlas_font());
	return s7_unspecified(sc);
}

/* (kgui-text-metrics str) -> (w h) in pixels, for centring a label. */
static s7_pointer sp_kgui_text_metrics(s7_scheme *sc, s7_pointer args)
{
	s7_pointer str = s7_car(args);
	float      w   = 0.0f;

	if (s7_is_string(str))
		w = kgui_text_width(s7_string(str), s_text_size,
				    glyph_source, atlas_font());
	return s7_list(sc, 2, s7_make_real(sc, (s7_double)w),
		       s7_make_real(sc, (s7_double)s_text_size));
}

/*
 * (kgui-button x y w h) -> #t if a bar tap landed in this rect this frame.
 * The button does not draw — the image draws its own background and label — it
 * only registers the rect as part of the bar's hit footprint and reports the
 * trapped tap, consuming it so a single tap fires exactly one button.
 */
static s7_pointer sp_kgui_button(s7_scheme *sc, s7_pointer args)
{
	s7_pointer p = args;
	double x = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double y = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double w = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double h = s7_number_to_real(sc, s7_car(p));
	bool   hit = false;

	bar_add_rect((float)x, (float)y, (float)w, (float)h);

	if (s_tap_pending &&
	    s_tap_x >= x && s_tap_x <= x + w &&
	    s_tap_y >= y && s_tap_y <= y + h) {
		hit = true;
		s_tap_pending = false;
	}
	return s7_make_boolean(sc, hit);
}

/* (kgui-viewport-size) -> (w h) in CSS pixels, for responsive layout. */
static s7_pointer sp_kgui_viewport_size(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return s7_list(sc, 2, s7_make_real(sc, (s7_double)s_css_w),
		       s7_make_real(sc, (s7_double)s_css_h));
}

} /* extern "C" */

static void register_primitives(s7_scheme *sc)
{
	s7_define_function(sc, "kgui-rect", sp_kgui_rect, 8, 0, false,
			   "(kgui-rect x y w h r g b a) filled rectangle");
	s7_define_function(sc, "kgui-text", sp_kgui_text, 7, 0, false,
			   "(kgui-text x y str r g b a) atlas text run");
	s7_define_function(sc, "kgui-text-metrics", sp_kgui_text_metrics, 1, 0,
			   false, "(kgui-text-metrics str) -> (w h) px");
	s7_define_function(sc, "kgui-button", sp_kgui_button, 4, 0, false,
			   "(kgui-button x y w h) -> #t on tap this frame");
	s7_define_function(sc, "kgui-viewport-size", sp_kgui_viewport_size, 0, 0,
			   false, "(kgui-viewport-size) -> (w h) CSS px");
}

/*
 * Register the primitives and load the panel image once, lazily, when the s7
 * runtime is up. Mirrors kruddboard's ensure_panel_scm.
 */
static s7_scheme *ensure_panel_scm(void)
{
	static bool ready;
	s7_scheme  *sc = script_s7();

	if (ready || !sc)
		return sc;
	register_primitives(sc);
	script_eval(KRUDDGUI_SCM);
	ready = true;
	return sc;
}

static void call_scm_panel(const char *proc)
{
	s7_scheme *sc = ensure_panel_scm();
	s7_pointer fn;

	if (!sc)
		return;
	fn = s7_name_to_value(sc, proc);
	if (s7_is_procedure(fn))
		s7_call(sc, fn, s7_nil(sc));
}

#endif /* __EMSCRIPTEN__ */

/* ------------------------------------------------------------------ */
/* Subsystem lifecycle                                                 */
/* ------------------------------------------------------------------ */

static void kruddgui_init(void)
{
#ifdef __EMSCRIPTEN__
	kgui_batch_init(&s_batch, s_verts, KGUI_MAX_VERTS);
	gl_init();

	/* Take over the pointer callbacks from imgui_plugin. */
	emscripten_set_mousemove_callback("#canvas", nullptr, 0, on_mouse_move);
	emscripten_set_mousedown_callback("#canvas", nullptr, 0, on_mouse_button);
	emscripten_set_mouseup_callback("#canvas",   nullptr, 0, on_mouse_button);
	emscripten_set_touchstart_callback("#canvas",  nullptr, 0, on_touch);
	emscripten_set_touchmove_callback("#canvas",   nullptr, 0, on_touch);
	emscripten_set_touchend_callback("#canvas",    nullptr, 0, on_touch);
	emscripten_set_touchcancel_callback("#canvas", nullptr, 0, on_touch);

	s_touch_device = krudd_is_touch_device() != 0;
#endif
	if (g_log)
		g_log->write(LOG_LEVEL_INFO, "kruddgui: init");
}

static void kruddgui_tick(void)
{
#ifdef __EMSCRIPTEN__
	double css_w, css_h, dpr;

	emscripten_get_element_css_size("#canvas", &css_w, &css_h);
	dpr      = get_device_pixel_ratio();
	s_css_w  = (float)css_w;
	s_css_h  = (float)css_h;
	s_phys_w = (int)(css_w * dpr + 0.5);
	s_phys_h = (int)(css_h * dpr + 0.5);

	refresh_atlas();

	kgui_batch_reset(&s_batch);
	bar_reset();

	call_scm_panel("kruddgui-draw");

	/*
	 * A tap gets exactly one frame to be claimed by a button. Clearing it
	 * here (never before the eval, which is what consumes it) stops a tap
	 * that missed every chip from lingering into a later frame.
	 */
	s_tap_pending = false;

	gl_flush(atlas_texture());
#endif
}

static void kruddgui_shutdown(void)
{
	if (g_log)
		g_log->write(LOG_LEVEL_INFO, "kruddgui: shutdown");
}

static const struct subsystem desc = {
	"kruddgui",
	nullptr,
	kruddgui_init,
	kruddgui_tick,
	kruddgui_shutdown,
};

extern "C" void kruddgui_plugin_entry(struct subsystem_manager *mgr)
{
#ifdef __EMSCRIPTEN__
	g_log = (const struct log_api *)
		subsystem_manager_get_api(mgr, "log");
#endif
	subsystem_manager_register(mgr, &desc);
}
