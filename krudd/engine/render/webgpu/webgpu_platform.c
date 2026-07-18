/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Platform implementations of the WebGPU backend seam — see webgpu_platform.h
 * for what belongs here and why the list is deliberately short.
 *
 * Both targets live in this one file rather than a pair of per-target sources
 * because kruddmake's `native-only` / `wasm-only` clauses wrap whole libraries
 * and executables, not individual sources: a per-target file would compile to an
 * empty translation unit on the other target, which krudd's `-Wpedantic
 * -Werror` rejects outright. One file, one #ifdef, both halves visible side by
 * side.
 */
#include "webgpu_platform.h"

#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <emscripten/html5.h>

/*
 * Tell the shell WebGPU went live so the header badge flips (kruddSetRenderer
 * from shell.html). Named distinctly from the WebGL backend's reporter — both
 * are compiled into the one WASM module, so a shared symbol would collide.
 */
EM_JS(void, webgpu_js_announce_renderer, (void), {
	if (typeof window.kruddSetRenderer === 'function')
		window.kruddSetRenderer('webgpu');
})

/*
 * Surface progress into the shell's scrolling WebGPU log panel (and the
 * console), so a browser without a working adapter/device reports where it
 * stopped, with full history, instead of just a blank canvas. Diagnostic
 * scaffolding for the port; the kruddgui editor console takes over once WebGPU
 * can drive the UI.
 */
EM_JS(void, webgpu_js_status, (const char *msg), {
	var s = UTF8ToString(msg);
	if (typeof window.kruddWebGPULog === 'function')
		window.kruddWebGPULog(s);
	if (typeof console !== 'undefined')
		console.log('[webgpu] ' + s);
})

WGPUSurface webgpu_platform_create_surface(WGPUInstance instance)
{
	WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas_src;
	WGPUSurfaceDescriptor sd;
	WGPUStringView selector;

	selector.data   = "#canvas";
	selector.length = strlen("#canvas");

	memset(&canvas_src, 0, sizeof(canvas_src));
	canvas_src.chain.sType =
		WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
	canvas_src.selector = selector;

	memset(&sd, 0, sizeof(sd));
	sd.nextInChain = &canvas_src.chain;
	return wgpuInstanceCreateSurface(instance, &sd);
}

void webgpu_platform_backbuffer_size(uint32_t *w, uint32_t *h)
{
	double css_w = 0.0;
	double css_h = 0.0;

	/*
	 * A WebGPU surface draws into the canvas's backing store, whose size is
	 * the canvas.width/height attributes — not its CSS size. Nothing sets
	 * those in WebGPU mode (the WebGL path used to, via the GL context), so
	 * match the backing store to the element's CSS size here; otherwise the
	 * drawing buffer stays 0x0 and nothing is visible however well the clear
	 * runs.
	 */
	emscripten_get_element_css_size("#canvas", &css_w, &css_h);
	*w = (css_w >= 1.0) ? (uint32_t)css_w : 800u;
	*h = (css_h >= 1.0) ? (uint32_t)css_h : 600u;
	emscripten_set_canvas_element_size("#canvas", (int)*w, (int)*h);
}

void webgpu_platform_status(const char *msg)
{
	webgpu_js_status(msg);
}

void webgpu_platform_announce_renderer(void)
{
	webgpu_js_announce_renderer();
}

#else /* native */

#include <stdlib.h>

/*
 * Offscreen by design: a native swapchain is a different Dawn backend from the
 * canvas path, so a window could not reproduce a canvas presentation bug even
 * if one were available. Returning NULL is how the backend is told to configure
 * itself for an offscreen target.
 */
WGPUSurface webgpu_platform_create_surface(WGPUInstance instance)
{
	(void)instance;
	return NULL;
}

/*
 * A fixed offscreen size, overridable so a native capture can be matched to a
 * browser one for pixel comparison against tools/render-diff output.
 */
static uint32_t size_from_env(const char *name, uint32_t fallback)
{
	const char *s = getenv(name);
	long v;

	if (!s || !*s)
		return fallback;
	v = strtol(s, NULL, 10);
	if (v < 1 || v > 16384)
		return fallback;
	return (uint32_t)v;
}

void webgpu_platform_backbuffer_size(uint32_t *w, uint32_t *h)
{
	*w = size_from_env("KRUDD_WEBGPU_WIDTH", 800u);
	*h = size_from_env("KRUDD_WEBGPU_HEIGHT", 600u);
}

void webgpu_platform_status(const char *msg)
{
	fprintf(stderr, "[webgpu] %s\n", msg);
}

void webgpu_platform_announce_renderer(void)
{
}

#endif
