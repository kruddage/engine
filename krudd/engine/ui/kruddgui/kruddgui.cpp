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
#include "kgui_input.h"
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
 * Upload and draw the accumulated batch. Sets every piece of GL state it needs
 * (blend, no depth/cull, viewport, program, VAO, atlas texture) so it is
 * independent of whatever ImGui left bound; it runs at the end of the frame and
 * the next frame's renderers set their own state. The batch is one VBO but is
 * issued as one draw per clip command so a scroll body can be scissored: a
 * command's CSS-pixel clip is converted to a physical-pixel, y-up GL scissor.
 */
static void gl_flush(GLuint atlas)
{
	float scale_x = (s_css_w > 0.0f) ? (float)s_phys_w / s_css_w : 1.0f;
	float scale_y = (s_css_h > 0.0f) ? (float)s_phys_h / s_css_h : 1.0f;
	int   i;

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

	for (i = 0; i < s_batch.cmd_count; i++) {
		const struct kgui_clip_cmd *c = &s_batch.cmds[i];

		if (c->count <= 0)
			continue;
		if (c->clipped) {
			int sx = (int)(c->x * scale_x + 0.5f);
			int sw = (int)(c->w * scale_x + 0.5f);
			int sh = (int)(c->h * scale_y + 0.5f);
			int sy = s_phys_h - (int)((c->y + c->h) * scale_y + 0.5f);

			glEnable(GL_SCISSOR_TEST);
			glScissor(sx, sy, sw < 0 ? 0 : sw, sh < 0 ? 0 : sh);
		} else {
			glDisable(GL_SCISSOR_TEST);
		}
		glDrawArrays(GL_TRIANGLES, c->first, c->count);
	}

	glDisable(GL_SCISSOR_TEST);
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
/* Pointer router — the multi-touch hit-test registry (#489)           */
/* ------------------------------------------------------------------ */

/*
 * kruddgui owns the Emscripten pointer callbacks and routes every event
 * through kgui_input: a down that lands on a declared panel region is captured
 * by that region for its whole gesture, and everything else is forwarded to
 * ImGui with the exact translation imgui_plugin used before input moved here.
 * The router is GL- and ImGui-free (host-tested in kgui_input_test.c); this
 * file is only the thin Emscripten/ImGui adapter over it.
 *
 * s_input's region set is (re)declared by the Scheme image each tick
 * (kgui-panel-begin) and committed at the end of the tick; the async callbacks
 * route against the committed set between ticks, safe because they never fire
 * mid-tick (single-threaded).
 */
static struct kgui_input s_input;

/* The region the image is currently drawing into (kgui-panel-begin/end). */
static struct kgui_region_io *s_cur_io;

static bool s_touch_device;

/* ------------------------------------------------------------------ */
/* Input callbacks — route through the registry, forward the rest      */
/* ------------------------------------------------------------------ */

/*
 * The forward path — the exact io translation imgui_plugin did before input
 * ownership moved here. Positions come from targetX/targetY (element-relative
 * CSS pixels), not the deprecated canvasX/canvasY (which read 0 on current
 * Emscripten), so the router's hit-test and ImGui's io share one CSS-pixel
 * space with imgui_tick's DisplaySize.
 */
static void forward_button(float x, float y, bool down)
{
	ImGuiIO &io = ImGui::GetIO();

	io.AddMousePosEvent(x, y);
	io.AddMouseButtonEvent(0, down);
}

static void pointer_down(int32_t id, float x, float y)
{
	if (kgui_input_pointer_down(&s_input, id, x, y) == KGUI_ROUTE_FORWARD)
		forward_button(x, y, true);
}

static void pointer_move(int32_t id, float x, float y)
{
	if (kgui_input_pointer_move(&s_input, id, x, y) == KGUI_ROUTE_FORWARD)
		ImGui::GetIO().AddMousePosEvent(x, y);
}

static void pointer_up(int32_t id, float x, float y, bool is_touch)
{
	if (kgui_input_pointer_up(&s_input, id, x, y) == KGUI_ROUTE_FORWARD) {
		forward_button(x, y, false);
		if (is_touch)
			imgui_soft_keyboard_touch_hint();
	}
}

static EM_BOOL on_mouse_move(int /*type*/, const EmscriptenMouseEvent *e,
			     void * /*ud*/)
{
	pointer_move(KGUI_MOUSE_ID, (float)e->targetX, (float)e->targetY);
	return EM_FALSE;
}

static EM_BOOL on_mouse_button(int type, const EmscriptenMouseEvent *e,
			       void * /*ud*/)
{
	bool  pressed = (type == EMSCRIPTEN_EVENT_MOUSEDOWN);
	float x       = (float)e->targetX;
	float y       = (float)e->targetY;

	/* Only the left button routes to panels; forward the rest verbatim. */
	if ((int)e->button == 0) {
		if (pressed)
			pointer_down(KGUI_MOUSE_ID, x, y);
		else
			pointer_up(KGUI_MOUSE_ID, x, y, false);
	} else if ((int)e->button < 5) {
		ImGuiIO &io = ImGui::GetIO();

		io.AddMousePosEvent(x, y);
		io.AddMouseButtonEvent((int)e->button, pressed);
	}
	return EM_FALSE;
}

/*
 * Every changed touch point is routed by its identifier, so several fingers on
 * different panels each drive their own region and one never steals another.
 */
static EM_BOOL on_touch(int type, const EmscriptenTouchEvent *e, void * /*ud*/)
{
	int i;

	for (i = 0; i < e->numTouches; i++) {
		const EmscriptenTouchPoint *t = &e->touches[i];
		int32_t id = (int32_t)t->identifier;
		float   x  = (float)t->targetX;
		float   y  = (float)t->targetY;

		if (!t->isChanged)
			continue;

		if (type == EMSCRIPTEN_EVENT_TOUCHSTART)
			pointer_down(id, x, y);
		else if (type == EMSCRIPTEN_EVENT_TOUCHEND ||
			 type == EMSCRIPTEN_EVENT_TOUCHCANCEL)
			pointer_up(id, x, y, true);
		else
			pointer_move(id, x, y);
	}
	return EM_TRUE;
}

/*
 * Wheel routes to a region under the pointer as a scroll delta (a scrollable
 * panel reads it via kgui-region-wheel); otherwise it becomes ImGui's wheel.
 * DOM deltaY is positive-down and roughly one notch per 100 px; ImGui's wheel
 * is positive-up, so the forwarded value is negated and scaled to notches.
 */
static EM_BOOL on_wheel(int /*type*/, const EmscriptenWheelEvent *e,
			void * /*ud*/)
{
	float x = (float)e->mouse.targetX;
	float y = (float)e->mouse.targetY;

	if (kgui_input_wheel(&s_input, x, y, (float)e->deltaY) ==
	    KGUI_ROUTE_FORWARD)
		ImGui::GetIO().AddMouseWheelEvent(0.0f,
						  -(float)e->deltaY / 100.0f);
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
 * (kgui-panel-begin name x y w h) -> unspecified. Declare an input region and
 * enter it: the region captures any gesture whose down lands inside its rect,
 * and subsequent kgui-button / kgui-region-* calls read its trapped input.
 * The rect must enclose the panel's buttons and scroll body so their gestures
 * are captured. Panels are declared in draw order; a later one is on top.
 */
static s7_pointer sp_kgui_panel_begin(s7_scheme *sc, s7_pointer args)
{
	s7_pointer  p    = args;
	s7_pointer  nm   = s7_car(p);                     p = s7_cdr(p);
	double      x    = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double      y    = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double      w    = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double      h    = s7_number_to_real(sc, s7_car(p));
	const char *name = s7_is_string(nm) ? s7_string(nm) : "";

	s_cur_io = kgui_input_region(&s_input, kgui_name_hash(name),
				     (float)x, (float)y, (float)w, (float)h);
	return s7_unspecified(sc);
}

/* (kgui-panel-end) -> unspecified. Leave the current input region. */
static s7_pointer sp_kgui_panel_end(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	s_cur_io = NULL;
	return s7_unspecified(sc);
}

/*
 * (kgui-button x y w h) -> #t if a tap landed in this rect this frame. A
 * button is a hit-target inside the current panel region: the region captures
 * the gesture and this reports and consumes its trapped tap when it falls in
 * the rect, so one tap fires exactly one button. The button does not draw —
 * the image draws its own background and label.
 */
static s7_pointer sp_kgui_button(s7_scheme *sc, s7_pointer args)
{
	s7_pointer p = args;
	double x = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double y = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double w = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double h = s7_number_to_real(sc, s7_car(p));
	bool   hit = false;

	if (s_cur_io && s_cur_io->tapped &&
	    s_cur_io->tap_x >= x && s_cur_io->tap_x <= x + w &&
	    s_cur_io->tap_y >= y && s_cur_io->tap_y <= y + h) {
		hit = true;
		s_cur_io->tapped = 0; /* consume: one tap, one button */
	}
	return s7_make_boolean(sc, hit);
}

/*
 * (kgui-region-drag) -> (dx dy): the captured pointer motion in the current
 * panel this frame (CSS px), or (0 0) outside a panel. A scroll body adds -dy
 * to its offset each frame to drag its contents.
 */
static s7_pointer sp_kgui_region_drag(s7_scheme *sc, s7_pointer args)
{
	double dx = s_cur_io ? (double)s_cur_io->drag_dx : 0.0;
	double dy = s_cur_io ? (double)s_cur_io->drag_dy : 0.0;

	(void)args;
	return s7_list(sc, 2, s7_make_real(sc, (s7_double)dx),
		       s7_make_real(sc, (s7_double)dy));
}

/* (kgui-region-wheel) -> wheel delta over the current panel this frame. */
static s7_pointer sp_kgui_region_wheel(s7_scheme *sc, s7_pointer args)
{
	double w = s_cur_io ? (double)s_cur_io->wheel : 0.0;

	(void)args;
	return s7_make_real(sc, (s7_double)w);
}

/* (kgui-region-pressed) -> #t while a pointer is held on the current panel. */
static s7_pointer sp_kgui_region_pressed(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return s7_make_boolean(sc, s_cur_io && s_cur_io->pressed);
}

/*
 * (kgui-clip x y w h) clip subsequent draws to a rect (CSS px);
 * (kgui-clip-none) clears it. Used to scissor a scroll body to its viewport.
 */
static s7_pointer sp_kgui_clip(s7_scheme *sc, s7_pointer args)
{
	s7_pointer p = args;
	double x = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double y = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double w = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double h = s7_number_to_real(sc, s7_car(p));

	kgui_batch_set_clip(&s_batch, (float)x, (float)y, (float)w, (float)h);
	return s7_unspecified(sc);
}

static s7_pointer sp_kgui_clip_none(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	kgui_batch_clear_clip(&s_batch);
	return s7_unspecified(sc);
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
	s7_define_function(sc, "kgui-panel-begin", sp_kgui_panel_begin, 5, 0,
			   false, "(kgui-panel-begin name x y w h) input region");
	s7_define_function(sc, "kgui-panel-end", sp_kgui_panel_end, 0, 0, false,
			   "(kgui-panel-end) leave the current input region");
	s7_define_function(sc, "kgui-region-drag", sp_kgui_region_drag, 0, 0,
			   false, "(kgui-region-drag) -> (dx dy) this frame");
	s7_define_function(sc, "kgui-region-wheel", sp_kgui_region_wheel, 0, 0,
			   false, "(kgui-region-wheel) -> wheel delta");
	s7_define_function(sc, "kgui-region-pressed", sp_kgui_region_pressed, 0,
			   0, false, "(kgui-region-pressed) -> #t while held");
	s7_define_function(sc, "kgui-clip", sp_kgui_clip, 4, 0, false,
			   "(kgui-clip x y w h) clip subsequent draws");
	s7_define_function(sc, "kgui-clip-none", sp_kgui_clip_none, 0, 0, false,
			   "(kgui-clip-none) clear the clip");
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
	kgui_input_init(&s_input);
	gl_init();

	/* Take over the pointer callbacks from imgui_plugin. */
	emscripten_set_mousemove_callback("#canvas", nullptr, 0, on_mouse_move);
	emscripten_set_mousedown_callback("#canvas", nullptr, 0, on_mouse_button);
	emscripten_set_mouseup_callback("#canvas",   nullptr, 0, on_mouse_button);
	emscripten_set_touchstart_callback("#canvas",  nullptr, 0, on_touch);
	emscripten_set_touchmove_callback("#canvas",   nullptr, 0, on_touch);
	emscripten_set_touchend_callback("#canvas",    nullptr, 0, on_touch);
	emscripten_set_touchcancel_callback("#canvas", nullptr, 0, on_touch);
	emscripten_set_wheel_callback("#canvas",       nullptr, 0, on_wheel);

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
	kgui_input_frame_begin(&s_input);
	s_cur_io = NULL;

	call_scm_panel("kruddgui-draw");

	/*
	 * Commit the regions the image just declared as the set the async
	 * callbacks route against, and clear the one-frame results (taps, drag,
	 * wheel) the image consumed. Doing it after the eval — which is what
	 * reads them — stops an unclaimed tap lingering into a later frame.
	 */
	s_cur_io = NULL;
	kgui_input_frame_commit(&s_input);

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
