/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/fetch.h>

#include "canvas_api.h"
#include "fetch_api.h"

#include <stdlib.h>
#include <string.h>

/*
 * Platform bridge: implements canvas_api and fetch_api for the main module.
 *
 * All emscripten_* and EM_JS calls live here.  Plugin side modules obtain
 * these capabilities through the "canvas" and "fetch" subsystem APIs and
 * never call emscripten_* directly — those are JS library functions that
 * are absent from asmLibraryArg in side modules, producing a throwing stub.
 *
 * WebGL context creation is exposed as three plain WASM functions that the
 * renderer plugin imports via extern declarations; EMSCRIPTEN_KEEPALIVE
 * prevents the linker from dead-code eliminating them since the dependency
 * is only visible at dynamic-link time.
 */

/* ------------------------------------------------------------------ */
/* canvas_api — canvas size, DPR, and input events                    */
/* ------------------------------------------------------------------ */

EM_JS(double, get_device_pixel_ratio, (void), {
	return window.devicePixelRatio || 1.0;
})

static void canvas_get_css_size_impl(double *w, double *h)
{
	emscripten_get_element_css_size("#canvas", w, h);
}

static void canvas_set_size_impl(int w, int h)
{
	emscripten_set_canvas_element_size("#canvas", w, h);
}

/* Per-event-type callback slots. One registration per event type. */
static canvas_mousemove_fn   s_mousemove_fn;   static void *s_mousemove_ud;
static canvas_mousebutton_fn s_mousedown_fn;   static void *s_mousedown_ud;
static canvas_mousebutton_fn s_mouseup_fn;     static void *s_mouseup_ud;
static canvas_touch_fn       s_touchstart_fn;  static void *s_touchstart_ud;
static canvas_touch_fn       s_touchmove_fn;   static void *s_touchmove_ud;
static canvas_touch_fn       s_touchend_fn;    static void *s_touchend_ud;
static canvas_touch_fn       s_touchcancel_fn; static void *s_touchcancel_ud;
static canvas_key_fn         s_keydown_fn;     static void *s_keydown_ud;

static EM_BOOL adapt_mousemove(int type, const EmscriptenMouseEvent *e,
				void *ud)
{
	struct canvas_mouse_event ev;

	(void)type;
	(void)ud;
	if (!s_mousemove_fn)
		return EM_FALSE;
	ev.x      = e->canvasX;
	ev.y      = e->canvasY;
	ev.button = 0;
	return (EM_BOOL)s_mousemove_fn(&ev, s_mousemove_ud);
}

static EM_BOOL adapt_mousedown(int type, const EmscriptenMouseEvent *e,
				void *ud)
{
	struct canvas_mouse_event ev;

	(void)type;
	(void)ud;
	if (!s_mousedown_fn)
		return EM_FALSE;
	ev.x      = e->canvasX;
	ev.y      = e->canvasY;
	ev.button = (int)e->button;
	return (EM_BOOL)s_mousedown_fn(1, &ev, s_mousedown_ud);
}

static EM_BOOL adapt_mouseup(int type, const EmscriptenMouseEvent *e,
			      void *ud)
{
	struct canvas_mouse_event ev;

	(void)type;
	(void)ud;
	if (!s_mouseup_fn)
		return EM_FALSE;
	ev.x      = e->canvasX;
	ev.y      = e->canvasY;
	ev.button = (int)e->button;
	return (EM_BOOL)s_mouseup_fn(0, &ev, s_mouseup_ud);
}

static EM_BOOL adapt_touch_event(canvas_touch_fn fn, void *ud, int ev_type,
				  const EmscriptenTouchEvent *e)
{
	struct canvas_touch_event ev;

	if (!fn || e->numTouches < 1)
		return EM_FALSE;
	ev.x    = e->touches[0].targetX;
	ev.y    = e->touches[0].targetY;
	ev.type = ev_type;
	return (EM_BOOL)fn(&ev, ud);
}

static EM_BOOL adapt_touchstart(int t, const EmscriptenTouchEvent *e, void *u)
{
	(void)t; (void)u;
	return adapt_touch_event(s_touchstart_fn, s_touchstart_ud,
				 CANVAS_TOUCH_START, e);
}

static EM_BOOL adapt_touchmove(int t, const EmscriptenTouchEvent *e, void *u)
{
	(void)t; (void)u;
	return adapt_touch_event(s_touchmove_fn, s_touchmove_ud,
				 CANVAS_TOUCH_MOVE, e);
}

static EM_BOOL adapt_touchend(int t, const EmscriptenTouchEvent *e, void *u)
{
	(void)t; (void)u;
	return adapt_touch_event(s_touchend_fn, s_touchend_ud,
				 CANVAS_TOUCH_END, e);
}

static EM_BOOL adapt_touchcancel(int t, const EmscriptenTouchEvent *e, void *u)
{
	(void)t; (void)u;
	return adapt_touch_event(s_touchcancel_fn, s_touchcancel_ud,
				 CANVAS_TOUCH_CANCEL, e);
}

static EM_BOOL adapt_keydown(int type, const EmscriptenKeyboardEvent *e,
			      void *ud)
{
	struct canvas_key_event ev;

	(void)type;
	(void)ud;
	if (!s_keydown_fn)
		return EM_FALSE;
	strncpy(ev.code, e->code, sizeof(ev.code) - 1);
	ev.code[sizeof(ev.code) - 1] = '\0';
	return (EM_BOOL)s_keydown_fn(&ev, s_keydown_ud);
}

static void canvas_set_mousemove(canvas_mousemove_fn cb, void *ud)
{
	s_mousemove_fn = cb;
	s_mousemove_ud = ud;
	emscripten_set_mousemove_callback("#canvas", NULL, 0, adapt_mousemove);
}

static void canvas_set_mousedown(canvas_mousebutton_fn cb, void *ud)
{
	s_mousedown_fn = cb;
	s_mousedown_ud = ud;
	emscripten_set_mousedown_callback("#canvas", NULL, 0, adapt_mousedown);
}

static void canvas_set_mouseup(canvas_mousebutton_fn cb, void *ud)
{
	s_mouseup_fn = cb;
	s_mouseup_ud = ud;
	emscripten_set_mouseup_callback("#canvas", NULL, 0, adapt_mouseup);
}

static void canvas_set_touchstart(canvas_touch_fn cb, void *ud)
{
	s_touchstart_fn = cb;
	s_touchstart_ud = ud;
	emscripten_set_touchstart_callback("#canvas", NULL, 0, adapt_touchstart);
}

static void canvas_set_touchmove(canvas_touch_fn cb, void *ud)
{
	s_touchmove_fn = cb;
	s_touchmove_ud = ud;
	emscripten_set_touchmove_callback("#canvas", NULL, 0, adapt_touchmove);
}

static void canvas_set_touchend(canvas_touch_fn cb, void *ud)
{
	s_touchend_fn = cb;
	s_touchend_ud = ud;
	emscripten_set_touchend_callback("#canvas", NULL, 0, adapt_touchend);
}

static void canvas_set_touchcancel(canvas_touch_fn cb, void *ud)
{
	s_touchcancel_fn = cb;
	s_touchcancel_ud = ud;
	emscripten_set_touchcancel_callback("#canvas", NULL, 0,
					    adapt_touchcancel);
}

static void canvas_set_keydown(canvas_key_fn cb, void *ud)
{
	s_keydown_fn = cb;
	s_keydown_ud = ud;
	emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT,
					NULL, 0, adapt_keydown);
}

const struct canvas_api g_canvas_api = {
	.get_device_pixel_ratio  = get_device_pixel_ratio,
	.get_css_size            = canvas_get_css_size_impl,
	.set_size                = canvas_set_size_impl,
	.set_mousemove_callback  = canvas_set_mousemove,
	.set_mousedown_callback  = canvas_set_mousedown,
	.set_mouseup_callback    = canvas_set_mouseup,
	.set_touchstart_callback = canvas_set_touchstart,
	.set_touchmove_callback  = canvas_set_touchmove,
	.set_touchend_callback   = canvas_set_touchend,
	.set_touchcancel_callback = canvas_set_touchcancel,
	.set_keydown_callback    = canvas_set_keydown,
};

/* ------------------------------------------------------------------ */
/* fetch_api — async HTTP fetch                                        */
/* ------------------------------------------------------------------ */

struct fetch_ctx {
	fetch_success_fn on_success;
	fetch_error_fn   on_error;
	void            *userdata;
};

static void on_fetch_done(emscripten_fetch_t *fetch)
{
	struct fetch_ctx *ctx = (struct fetch_ctx *)fetch->userData;

	ctx->on_success(ctx->userdata, fetch->data, (uint32_t)fetch->numBytes);
	free(ctx);
	emscripten_fetch_close(fetch);
}

static void on_fetch_fail(emscripten_fetch_t *fetch)
{
	struct fetch_ctx *ctx = (struct fetch_ctx *)fetch->userData;

	ctx->on_error(ctx->userdata, (int)fetch->status);
	free(ctx);
	emscripten_fetch_close(fetch);
}

static void fetch_api_fetch(const char *url, void *userdata,
			     fetch_success_fn on_success,
			     fetch_error_fn on_error)
{
	emscripten_fetch_attr_t attr;
	struct fetch_ctx       *ctx;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return;
	ctx->on_success = on_success;
	ctx->on_error   = on_error;
	ctx->userdata   = userdata;

	emscripten_fetch_attr_init(&attr);
	strncpy(attr.requestMethod, "GET", sizeof(attr.requestMethod) - 1);
	attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
	attr.userData   = ctx;
	attr.onsuccess  = on_fetch_done;
	attr.onerror    = on_fetch_fail;
	emscripten_fetch(&attr, url);
}

const struct fetch_api g_fetch_api = {
	.fetch = fetch_api_fetch,
};

/* ------------------------------------------------------------------ */
/* WebGL context wrappers                                             */
/* ------------------------------------------------------------------ */

EMSCRIPTEN_KEEPALIVE int webgl_context_create(void)
{
	EmscriptenWebGLContextAttributes attrs;

	emscripten_webgl_init_context_attributes(&attrs);
	attrs.majorVersion = 2;
	attrs.minorVersion = 0;
	return (int)emscripten_webgl_create_context("#canvas", &attrs);
}

EMSCRIPTEN_KEEPALIVE void webgl_context_make_current(int ctx)
{
	emscripten_webgl_make_context_current(
		(EMSCRIPTEN_WEBGL_CONTEXT_HANDLE)ctx);
}

EMSCRIPTEN_KEEPALIVE void webgl_context_destroy(int ctx)
{
	emscripten_webgl_destroy_context(
		(EMSCRIPTEN_WEBGL_CONTEXT_HANDLE)ctx);
}

#endif /* __EMSCRIPTEN__ */
