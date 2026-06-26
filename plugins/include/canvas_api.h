/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CANVAS_API_H
#define CANVAS_API_H

/*
 * Canvas and input-event API for plugins.
 *
 * Registered by the main module as the "canvas" subsystem; retrieve via
 *   subsystem_manager_get_api(mgr, "canvas")
 *
 * Wraps Emscripten canvas and HTML5 input APIs so side-module plugins do
 * not need to call emscripten_* functions directly.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Touch event types passed to touch callbacks. */
#define CANVAS_TOUCH_START   0
#define CANVAS_TOUCH_MOVE    1
#define CANVAS_TOUCH_END     2
#define CANVAS_TOUCH_CANCEL  3

struct canvas_mouse_event {
	double x, y;
	int    button;
};

struct canvas_touch_event {
	double x, y;
	int    type; /* CANVAS_TOUCH_* */
};

struct canvas_key_event {
	char code[32]; /* DOM KeyboardEvent.code string */
};

typedef int (*canvas_mousemove_fn)(const struct canvas_mouse_event *ev,
				   void *ud);
typedef int (*canvas_mousebutton_fn)(int pressed,
				     const struct canvas_mouse_event *ev,
				     void *ud);
typedef int (*canvas_touch_fn)(const struct canvas_touch_event *ev, void *ud);
typedef int (*canvas_key_fn)(const struct canvas_key_event *ev, void *ud);

struct canvas_api {
	double (*get_device_pixel_ratio)(void);
	void   (*get_css_size)(double *w, double *h);
	void   (*set_size)(int w, int h);
	void   (*set_mousemove_callback)(canvas_mousemove_fn cb, void *ud);
	void   (*set_mousedown_callback)(canvas_mousebutton_fn cb, void *ud);
	void   (*set_mouseup_callback)(canvas_mousebutton_fn cb, void *ud);
	void   (*set_touchstart_callback)(canvas_touch_fn cb, void *ud);
	void   (*set_touchmove_callback)(canvas_touch_fn cb, void *ud);
	void   (*set_touchend_callback)(canvas_touch_fn cb, void *ud);
	void   (*set_touchcancel_callback)(canvas_touch_fn cb, void *ud);
	void   (*set_keydown_callback)(canvas_key_fn cb, void *ud);
};

#ifdef __cplusplus
}
#endif

#endif /* CANVAS_API_H */
