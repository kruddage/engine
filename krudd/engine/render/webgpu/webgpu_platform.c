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
	int cw = 0;
	int ch = 0;

	/*
	 * Report the canvas's backing store — canvas.width/height — because that
	 * is what the surface texture is sized from, whatever width/height
	 * wgpuSurfaceConfigure was handed.
	 *
	 * Read it, do not set it. kruddgui_tick owns the backing store and sizes
	 * it in DEVICE pixels (css * devicePixelRatio, kruddgui.cpp). This used to
	 * report the CSS size and write it back, which is identical on a dpr == 1
	 * display and wrong everywhere else: at dpr 3 the colour attachment came
	 * back 1236x2535 while the depth target was built 412x845, so every render
	 * pass failed attachment-size validation and the canvas stayed black on
	 * every phone and every HiDPI screen. Desktop dpr == 1 hid it completely.
	 *
	 * Boot order is self-healing: before kruddgui's first tick this reads the
	 * canvas default, and create_depth_target rebuilds when the size changes.
	 */
	emscripten_get_canvas_element_size("#canvas", &cw, &ch);
	*w = (cw >= 1) ? (uint32_t)cw : 800u;
	*h = (ch >= 1) ? (uint32_t)ch : 600u;
}

WGPUTextureView webgpu_platform_backbuffer_view(WGPUDevice device)
{
	(void)device;
	return NULL; /* the canvas surface is the backbuffer here */
}

int webgpu_platform_read_backbuffer(WGPUInstance instance, WGPUDevice device,
				    WGPUQueue queue, uint8_t *rgba)
{
	(void)instance;
	(void)device;
	(void)queue;
	(void)rgba;
	return 0; /* the compositor owns the canvas backbuffer */
}

void webgpu_platform_status(const char *msg)
{
	webgpu_js_status(msg);
}

void webgpu_platform_announce_renderer(void)
{
	webgpu_js_announce_renderer();
}

void webgpu_platform_teardown(void)
{
	/* The web surface's textures belong to the compositor — nothing ours. */
}

#else /* native */

#include <stdlib.h>

/*
 * The windowing host, or NULL. NULL is the default and the only state the
 * offscreen harness and CI ever see; a windowed binary sets it before boot (see
 * webgpu_platform.h). Every branch below that changes behaviour keys off this
 * one pointer, so "no host" is provably the original offscreen seam.
 */
static const struct webgpu_platform_host *g_host;

void webgpu_platform_set_host(const struct webgpu_platform_host *host)
{
	g_host = host;
}

/*
 * Offscreen unless a window is hosting us. Without a host a native swapchain is
 * a different Dawn backend from the canvas path — so a window could not
 * reproduce a canvas presentation bug even were one available, and returning
 * NULL is how the backend is told to configure an offscreen target. With a host
 * (krudd_window), its window's real WGPUSurface is what the frame presents into.
 */
WGPUSurface webgpu_platform_create_surface(WGPUInstance instance)
{
	if (g_host && g_host->create_surface)
		return g_host->create_surface(instance, g_host->user);
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
	if (g_host && g_host->drawable_size) {
		g_host->drawable_size(w, h, g_host->user);
		if (*w < 1u)
			*w = 1u;
		if (*h < 1u)
			*h = 1u;
		return;
	}
	*w = size_from_env("KRUDD_WEBGPU_WIDTH", 800u);
	*h = size_from_env("KRUDD_WEBGPU_HEIGHT", 600u);
}

/*
 * The offscreen colour target. Persistent and owned here, unlike a surface
 * texture which belongs to the frame — that is precisely what makes it readable
 * once the frame has been submitted.
 */
static WGPUTexture g_backbuffer;
static uint32_t    g_backbuffer_w;
static uint32_t    g_backbuffer_h;

WGPUTextureView webgpu_platform_backbuffer_view(WGPUDevice device)
{
	if (!device)
		return NULL;

	/*
	 * Hosted (windowed): there is a real surface, so the backend acquires the
	 * swapchain texture itself — same as the browser. Returning NULL here is
	 * exactly how the web seam answers, and it keeps the offscreen backbuffer
	 * below from ever being created when a window owns the frame.
	 */
	if (g_host)
		return NULL;

	if (!g_backbuffer) {
		WGPUTextureDescriptor td;

		webgpu_platform_backbuffer_size(&g_backbuffer_w, &g_backbuffer_h);

		memset(&td, 0, sizeof(td));
		/* CopySrc is what readback needs; RenderAttachment is what the
		 * frame needs. Format matches configure_surface's offscreen
		 * choice, which the scene renderer's pipelines are built for. */
		td.usage         = WGPUTextureUsage_RenderAttachment |
				   WGPUTextureUsage_CopySrc;
		td.dimension     = WGPUTextureDimension_2D;
		td.size.width    = g_backbuffer_w;
		td.size.height   = g_backbuffer_h;
		td.size.depthOrArrayLayers = 1;
		td.format        = WGPUTextureFormat_RGBA8Unorm;
		td.mipLevelCount = 1;
		td.sampleCount   = 1;

		g_backbuffer = wgpuDeviceCreateTexture(device, &td);
		if (!g_backbuffer)
			return NULL;
	}

	/* Fresh view per call: the backend releases the view it is handed at the
	 * end of every frame, the same as it does a surface texture's view. */
	return wgpuTextureCreateView(g_backbuffer, NULL);
}

struct map_wait {
	int done;
	int ok;
};

static void on_map(WGPUMapAsyncStatus status, WGPUStringView message,
		   void *ud1, void *ud2)
{
	struct map_wait *w = ud1;

	(void)message;
	(void)ud2;
	w->done = 1;
	w->ok   = (status == WGPUMapAsyncStatus_Success);
}

int webgpu_platform_read_backbuffer(WGPUInstance instance, WGPUDevice device,
				    WGPUQueue queue, uint8_t *rgba)
{
	WGPUCommandEncoder enc;
	WGPUCommandBuffer cmd;
	WGPUBuffer readback;
	WGPUBufferDescriptor bd;
	WGPUTexelCopyTextureInfo src;
	WGPUTexelCopyBufferInfo dst;
	WGPUExtent3D extent;
	WGPUBufferMapCallbackInfo cb;
	struct map_wait wait;
	const uint8_t *mapped;
	uint32_t padded_bpr;
	uint32_t tight_bpr;
	uint64_t bytes;
	uint32_t y;
	int spins;

	/* Hosted (windowed): the swapchain's textures belong to the compositor,
	 * not us — the same answer the web seam gives. Only the offscreen path,
	 * which owns g_backbuffer, can read a frame back. */
	if (g_host || !g_backbuffer || !device || !queue || !rgba)
		return 0;

	/*
	 * WebGPU requires a buffer copy's bytesPerRow to be a multiple of 256, so
	 * for most widths the readback buffer is wider than the image and every
	 * row has to be un-padded on the way out. Getting this wrong shears the
	 * image diagonally rather than failing, which is exactly the kind of
	 * plausible-looking wrong picture this instrument exists to avoid.
	 */
	tight_bpr  = g_backbuffer_w * 4u;
	padded_bpr = (tight_bpr + 255u) & ~255u;
	bytes      = (uint64_t)padded_bpr * g_backbuffer_h;

	memset(&bd, 0, sizeof(bd));
	bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
	bd.size  = bytes;
	readback = wgpuDeviceCreateBuffer(device, &bd);
	if (!readback)
		return 0;

	enc = wgpuDeviceCreateCommandEncoder(device, NULL);

	memset(&src, 0, sizeof(src));
	src.texture  = g_backbuffer;
	src.mipLevel = 0;
	src.aspect   = WGPUTextureAspect_All;

	memset(&dst, 0, sizeof(dst));
	dst.buffer              = readback;
	dst.layout.offset       = 0;
	dst.layout.bytesPerRow  = padded_bpr;
	dst.layout.rowsPerImage = g_backbuffer_h;

	extent.width              = g_backbuffer_w;
	extent.height             = g_backbuffer_h;
	extent.depthOrArrayLayers = 1;

	wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &extent);
	cmd = wgpuCommandEncoderFinish(enc, NULL);
	wgpuQueueSubmit(queue, 1, &cmd);
	wgpuCommandBufferRelease(cmd);
	wgpuCommandEncoderRelease(enc);

	wait.done = 0;
	wait.ok   = 0;
	memset(&cb, 0, sizeof(cb));
	cb.mode      = WGPUCallbackMode_AllowSpontaneous;
	cb.callback  = on_map;
	cb.userdata1 = &wait;
	wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, (size_t)bytes, cb);

	/* Pump rather than wgpuInstanceWaitAny, which would need the
	 * TimedWaitAny instance feature requested at instance creation — and the
	 * instance is created in the shared backend, not here. */
	for (spins = 0; !wait.done && spins < 10000; spins++)
		wgpuInstanceProcessEvents(instance);

	if (!wait.done || !wait.ok) {
		wgpuBufferRelease(readback);
		return 0;
	}

	mapped = wgpuBufferGetConstMappedRange(readback, 0, (size_t)bytes);
	if (!mapped) {
		wgpuBufferRelease(readback);
		return 0;
	}

	for (y = 0; y < g_backbuffer_h; y++)
		memcpy(rgba + (size_t)y * tight_bpr,
		       mapped + (size_t)y * padded_bpr,
		       tight_bpr);

	wgpuBufferUnmap(readback);
	wgpuBufferRelease(readback);
	return 1;
}

void webgpu_platform_status(const char *msg)
{
	fprintf(stderr, "[webgpu] %s\n", msg);
}

void webgpu_platform_announce_renderer(void)
{
}

void webgpu_platform_teardown(void)
{
	if (g_backbuffer) {
		wgpuTextureRelease(g_backbuffer);
		g_backbuffer   = NULL;
		g_backbuffer_w = 0;
		g_backbuffer_h = 0;
	}
}

#endif
