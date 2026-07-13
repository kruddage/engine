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
 * Text now stands on kruddgui's own baked glyph atlas (kgui_font): the font is
 * rasterised to a GL texture here and every glyph query goes through kgui_font,
 * so ImGui's font atlas is no longer sampled and a non-ASCII code point is
 * skipped rather than drawn as ImGui's '?'. That leaves one ImGui coupling in
 * this file, which falls with the last panel:
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
#include "kgui_text_edit.h"
#include "kgui_font.h"
}

#ifdef __EMSCRIPTEN__
#include "imgui.h"

#include <emscripten/html5.h>
#include <GLES3/gl3.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

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

/*
 * The web text-input bridge (plugin_abi.c, main module). kruddgui drives it
 * directly for its own focused field — the text-input twin of owning the
 * pointer callbacks. krudd_text_input_set_capture tells imgui_plugin to leave
 * the bridge alone while a kruddgui field holds focus (see imgui_tick).
 */
void krudd_text_input_show(void);
void krudd_text_input_hide(void);
int  krudd_text_input_drain_chars(char *buf, int cap);
int  krudd_text_input_pop_key(void);
void krudd_text_input_set_capture(int on);
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
 * it; the sampled texture is kruddgui's own baked glyph atlas (kgui_font),
 * rasterised once into s_font_tex below.
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

/*
 * The owned glyph atlas and its GL texture. s_font is baked once in
 * kruddgui_init and uploaded to s_font_tex; s_white_u/s_white_v (the solid
 * texel a filled rect samples) and s_text_size are copied from it there.
 */
static struct kgui_font s_font;
static GLuint           s_font_tex;
static float            s_white_u, s_white_v;
static float            s_text_size;

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

	/*
	 * Upload the baked glyph atlas as the sampled texture. NEAREST keeps the
	 * bitmap font crisp (it is drawn at whole multiples of its 8px cell), and
	 * clamping stops the edge columns of one glyph bleeding into its
	 * neighbour. s_font must already be baked (kruddgui_init does it first).
	 */
	glGenTextures(1, &s_font_tex);
	glBindTexture(GL_TEXTURE_2D, s_font_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
		     KGUI_FONT_ATLAS_W, KGUI_FONT_ATLAS_H, 0,
		     GL_RGBA, GL_UNSIGNED_BYTE, s_font.pixels);
	glBindTexture(GL_TEXTURE_2D, 0);

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

/* A touch consumed by a field raises the keyboard from the gesture (below). */
static int point_in_field(float x, float y);

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
	} else if (is_touch && point_in_field(x, y)) {
		/*
		 * A touch consumed by a kruddgui field: raise the soft keyboard
		 * from inside this gesture handler, which iOS Safari requires —
		 * field_sync's focus-driven show() runs in the later rAF tick,
		 * outside the gesture, where iOS ignores it.
		 */
		krudd_text_input_show();
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
/* Text field — the first interactive widget, backed by the soft keyboard */
/* ------------------------------------------------------------------ */

/*
 * kruddgui's field is the touch-keyboard model ImGui never had: exactly one
 * field owns focus and a caret at a time, and the hidden-<input> bridge feeds
 * it. The editable buffer is the GL-free kgui_text_edit (host-tested); this
 * layer adds focus, the per-tick char/key drain (field_pump), the show/hide +
 * capture reconcile (field_sync), and the tap-to-focus / commit reported to the
 * Scheme image through (kgui-field).
 *
 * Focus is keyed by the field's name hash, not its position, so a focused field
 * keeps its buffer while the panel scrolls it off-screen. Committing (Enter,
 * Tab, or focus moving to another field) latches the final text for one read;
 * Escape abandons the edit. Only an actually-edited field reports committed?,
 * so tapping in and out without typing records no undo step.
 */
static struct kgui_text_edit s_field_edit;
static uint32_t s_field_id;           /* focused field's name hash, 0 = none */
static int      s_field_numeric;      /* focused field filters to numeric input */
static int      s_field_dirty;        /* buffer edited since focus */
static int      s_field_committed;    /* latched commit awaiting a Scheme read */
static uint32_t s_field_committed_id; /* which field the latch is for */
static char     s_field_commit_buf[KGUI_TEXT_EDIT_CAP]; /* the committed text */
static int      s_field_shown;        /* the soft keyboard is up for our field */

/*
 * Field rects declared this tick, committed for the async touch callbacks (the
 * same "built this tick, read between ticks" discipline as the input regions).
 * A touch-up inside one raises the soft keyboard from within the gesture, which
 * iOS Safari requires — focus() from the later rAF tick would be ignored.
 */
#define KGUI_MAX_FIELD_RECTS 48
struct kgui_field_rect {
	float x0, y0, x1, y1;
};
static struct kgui_field_rect s_fr_build[KGUI_MAX_FIELD_RECTS];
static int                    s_fr_build_n;
static struct kgui_field_rect s_fr_committed[KGUI_MAX_FIELD_RECTS];
static int                    s_fr_committed_n;

static void field_rect_add(float x, float y, float w, float h)
{
	struct kgui_field_rect *r;

	if (s_fr_build_n >= KGUI_MAX_FIELD_RECTS)
		return;
	r     = &s_fr_build[s_fr_build_n++];
	r->x0 = x;
	r->y0 = y;
	r->x1 = x + w;
	r->y1 = y + h;
}

static void field_rects_commit(void)
{
	int i;

	for (i = 0; i < s_fr_build_n; i++)
		s_fr_committed[i] = s_fr_build[i];
	s_fr_committed_n = s_fr_build_n;
}

static int point_in_field(float x, float y)
{
	int i;

	for (i = 0; i < s_fr_committed_n; i++) {
		const struct kgui_field_rect *r = &s_fr_committed[i];

		if (x >= r->x0 && x <= r->x1 && y >= r->y0 && y <= r->y1)
			return 1;
	}
	return 0;
}

/*
 * Copy the focused field's final text into the commit slot and latch it for one
 * Scheme read. Kept separate from the live edit buffer so moving focus to a new
 * field (which re-seeds s_field_edit) does not clobber the text the old field is
 * about to hand back.
 */
static void field_latch_commit(void)
{
	memcpy(s_field_commit_buf, s_field_edit.buf,
	       (size_t)s_field_edit.len + 1);
	s_field_committed    = 1;
	s_field_committed_id = s_field_id;
}

/* Commit the focused field (only an actual edit latches) and drop focus. */
static void field_commit(void)
{
	if (s_field_dirty)
		field_latch_commit();
	s_field_id = 0;
}

/* Keep only the bytes a numeric field admits (digits, sign, dot, exponent). */
static int numeric_filter(char *buf, int n)
{
	int i, w = 0;

	for (i = 0; i < n; i++) {
		char c = buf[i];

		if ((c >= '0' && c <= '9') || c == '.' || c == '-' ||
		    c == '+' || c == 'e' || c == 'E')
			buf[w++] = c;
	}
	buf[w] = '\0';
	return w;
}

/*
 * Drain the keyboard bridge into the focused field. Runs at the top of the tick
 * (before the image reads the field) so a typed character shows the same frame.
 * Only runs while a field is focused, which is exactly when kruddgui holds the
 * capture flag and imgui_plugin has stepped aside.
 */
static void field_pump(void)
{
	char buf[256];
	int  n, k;

	if (!s_field_id)
		return;

	n = krudd_text_input_drain_chars(buf, (int)sizeof(buf));
	if (s_field_numeric)
		n = numeric_filter(buf, n);
	if (n > 0 && kgui_text_edit_insert(&s_field_edit, buf, n) > 0)
		s_field_dirty = 1;

	while ((k = krudd_text_input_pop_key()) != 0) {
		switch (k) {
		case 1: /* Backspace */
			kgui_text_edit_backspace(&s_field_edit);
			s_field_dirty = 1;
			break;
		case 4: /* Delete */
			kgui_text_edit_delete_fwd(&s_field_edit);
			s_field_dirty = 1;
			break;
		case 5: /* LeftArrow */
			kgui_text_edit_left(&s_field_edit);
			break;
		case 6: /* RightArrow */
			kgui_text_edit_right(&s_field_edit);
			break;
		case 9: /* Home */
			kgui_text_edit_home(&s_field_edit);
			break;
		case 10: /* End */
			kgui_text_edit_end(&s_field_edit);
			break;
		case 2: /* Enter */
		case 3: /* Tab */
			field_commit();
			return;
		case 11: /* Escape: abandon the edit */
			s_field_id = 0;
			return;
		default: /* Up / Down: no caret model on a single line */
			break;
		}
	}
}

/* Reconcile the soft keyboard + capture flag with our focus, once per tick. */
static void field_sync(void)
{
	int active = (s_field_id != 0);

	krudd_text_input_set_capture(active);
	if (active && !s_field_shown) {
		krudd_text_input_show();
		s_field_shown = 1;
	} else if (!active && s_field_shown) {
		krudd_text_input_hide();
		s_field_shown = 0;
	}
}

/* Pixel width of the first `caret` bytes of `s`, for placing the caret bar. */
static float field_caret_px(const char *s, int caret)
{
	char tmp[KGUI_TEXT_EDIT_CAP];

	if (caret <= 0)
		return 0.0f;
	if (caret > (int)sizeof(tmp) - 1)
		caret = (int)sizeof(tmp) - 1;
	memcpy(tmp, s, (size_t)caret);
	tmp[caret] = '\0';
	return kgui_text_width(tmp, s_text_size, kgui_font_glyph, &s_font);
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
				(float)a, kgui_font_glyph, &s_font);
	return s7_unspecified(sc);
}

/* (kgui-text-metrics str) -> (w h) in pixels, for centring a label. */
static s7_pointer sp_kgui_text_metrics(s7_scheme *sc, s7_pointer args)
{
	s7_pointer str = s7_car(args);
	float      w   = 0.0f;

	if (s7_is_string(str))
		w = kgui_text_width(s7_string(str), s_text_size,
				    kgui_font_glyph, &s_font);
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
 * (kgui-field id x y w h text mode) -> (display active? committed? caret-px).
 *
 * The soft-keyboard text field. `text` is the caller's current value; `mode` is
 * 0 for free text or 1 for numeric (the drain filters to numeric bytes). A tap
 * in the rect focuses the field, seeding the edit buffer from `text`; while
 * focused it returns the live edit buffer as `display`, `active?` #t, and the
 * caret's pixel offset for drawing a caret bar. On the frame the edit commits
 * (Enter / Tab / focus moving away), `committed?` is #t and `display` is the
 * final text — the caller writes it back through its undo-recording setter. The
 * field draws nothing itself; the image draws the box, text and caret.
 */
static s7_pointer sp_kgui_field(s7_scheme *sc, s7_pointer args)
{
	s7_pointer  p    = args;
	s7_pointer  nm   = s7_car(p);                        p = s7_cdr(p);
	double      x    = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double      y    = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double      w    = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double      h    = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	s7_pointer  txt  = s7_car(p);                        p = s7_cdr(p);
	double      mode = s7_number_to_real(sc, s7_car(p));
	const char *id   = s7_is_string(nm) ? s7_string(nm) : "";
	const char *cur  = s7_is_string(txt) ? s7_string(txt) : "";
	uint32_t    hash = kgui_name_hash(id);
	int         active = 0;
	int         committed = 0;
	const char *disp = cur;
	float       caret_px = 0.0f;

	field_rect_add((float)x, (float)y, (float)w, (float)h);

	if (hash == s_field_id) {
		active = 1;
		disp   = s_field_edit.buf;
	} else if (hash == s_field_committed_id) {
		committed            = s_field_committed;
		disp                 = s_field_commit_buf;
		s_field_committed    = 0;
		s_field_committed_id = 0;
	} else if (s_cur_io && s_cur_io->tapped &&
		   s_cur_io->tap_x >= x && s_cur_io->tap_x <= x + w &&
		   s_cur_io->tap_y >= y && s_cur_io->tap_y <= y + h) {
		s_cur_io->tapped = 0; /* consume: the tap focuses this field */
		/* Moving focus off an edited field commits it (read next frame). */
		if (s_field_id && s_field_dirty)
			field_latch_commit();
		kgui_text_edit_set(&s_field_edit, cur);
		s_field_id      = hash;
		s_field_dirty   = 0;
		s_field_numeric = (mode != 0.0);
		active          = 1;
		disp            = s_field_edit.buf;
	}

	if (active)
		caret_px = field_caret_px(disp, s_field_edit.caret);

	return s7_list(sc, 4, s7_make_string(sc, disp),
		       s7_make_boolean(sc, active),
		       s7_make_boolean(sc, committed),
		       s7_make_real(sc, (s7_double)caret_px));
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
 * (kgui-region name x y w h) -> (pressed press-x press-y): declare a per-widget
 * drag-capture region and read its live captured-pointer state. Unlike
 * kgui-panel-begin this does not change the current panel (s_cur_io) — it just
 * appends one more region to this frame's z-stack and hands back that region's
 * own result slot, so a widget (a slider track, a colour-picker square) can sit
 * ON TOP of the enclosing scroll body: a down inside it is captured here (read
 * press-x/press-y to map to a value) while a down anywhere else still reaches
 * the body region underneath and scrolls. Declare it after the body draw so it
 * wins the overlap, and only for on-screen widgets — regions are a fixed pool
 * (KGUI_MAX_REGIONS). `press-x`/`press-y` are the live pointer position while
 * `pressed`, updated on down and every captured move; they hold the down point
 * until the first move.
 */
static s7_pointer sp_kgui_region(s7_scheme *sc, s7_pointer args)
{
	s7_pointer  p    = args;
	s7_pointer  nm   = s7_car(p);                        p = s7_cdr(p);
	double      x    = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double      y    = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double      w    = s7_number_to_real(sc, s7_car(p)); p = s7_cdr(p);
	double      h    = s7_number_to_real(sc, s7_car(p));
	const char *name = s7_is_string(nm) ? s7_string(nm) : "";
	struct kgui_region_io *io =
		kgui_input_region(&s_input, kgui_name_hash(name),
				  (float)x, (float)y, (float)w, (float)h);

	return s7_list(sc, 3, s7_make_boolean(sc, io->pressed),
		       s7_make_real(sc, (s7_double)io->press_x),
		       s7_make_real(sc, (s7_double)io->press_y));
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
	s7_define_function(sc, "kgui-field", sp_kgui_field, 7, 0, false,
			   "(kgui-field id x y w h text mode) -> "
			   "(display active? committed? caret-px)");
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
	s7_define_function(sc, "kgui-region", sp_kgui_region, 5, 0, false,
			   "(kgui-region name x y w h) -> "
			   "(pressed press-x press-y) drag-capture region");
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

	/* Bake the owned glyph atlas before gl_init uploads it to a texture. */
	kgui_font_init(&s_font);
	s_white_u   = s_font.white_u;
	s_white_v   = s_font.white_v;
	s_text_size = s_font.size;
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

	/*
	 * Apply this tick's typed input to the focused field before the image
	 * reads it, so a keystroke shows the same frame. No-op unless a field
	 * holds focus (and thus kruddgui holds the keyboard-capture flag).
	 */
	field_pump();

	kgui_batch_reset(&s_batch);
	kgui_input_frame_begin(&s_input);
	s_cur_io     = NULL;
	s_fr_build_n = 0;

	call_scm_panel("kruddgui-draw");

	/*
	 * Commit the regions the image just declared as the set the async
	 * callbacks route against, and clear the one-frame results (taps, drag,
	 * wheel) the image consumed. Doing it after the eval — which is what
	 * reads them — stops an unclaimed tap lingering into a later frame.
	 */
	s_cur_io = NULL;
	kgui_input_frame_commit(&s_input);
	field_rects_commit();

	/* Reconcile the soft keyboard + capture flag with the field focus. */
	field_sync();

	gl_flush(s_font_tex);
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
