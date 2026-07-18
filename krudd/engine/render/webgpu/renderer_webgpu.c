/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * WebGPU renderer backend — first slice.
 *
 * This is the "fuchsia clear" probe: it stands up the emdawnwebgpu toolchain
 * end to end (instance -> canvas surface -> async adapter/device -> per-frame
 * render pass) and clears the canvas to fuchsia, proving the build wiring and
 * the version-sensitive surface API compile and run in the browser before the
 * full gpu_api vtable (pipelines, buffers, textures, draws) lands on top.
 *
 * It is browser-only. Selection lives in engine.c: with ?renderer=webgpu the
 * engine registers this backend alone and skips the GL render cluster, so the
 * probe owns the frame without a vtable the scene renderer would try to call.
 * The device handshake is async (adapter -> device callbacks); the tick no-ops
 * until the device is ready, then clears every frame.
 */
#include "subsystem.h"
#include "subsystem_manager.h"

#ifdef __EMSCRIPTEN__
#include "log_api.h"

#include <emscripten.h>
#include <emscripten/html5.h>
#include <webgpu/webgpu.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const struct log_api *g_log;

static WGPUInstance      g_instance;
static WGPUSurface       g_surface;
static WGPUAdapter       g_adapter;
static WGPUDevice        g_device;
static WGPUQueue         g_queue;
static WGPUTextureFormat g_format;
static int               g_ready;    /* 1 once the device + surface are live */

/*
 * Tell the shell WebGPU went live so the header badge flips (kruddSetRenderer
 * from shell.html, added with the load-time detection). Named distinctly from
 * the WebGL backend's reporter — both are compiled into the one WASM module, so
 * a shared symbol would collide at link.
 */
EM_JS(void, webgpu_announce_renderer, (void), {
	if (typeof window.kruddSetRenderer === 'function')
		window.kruddSetRenderer('webgpu');
})

/*
 * Surface the probe's progress into the shell's scrolling WebGPU log panel (and
 * the console), so a browser without a working adapter/device reports where it
 * stopped, with full history, instead of just a blank canvas or a single
 * overwritten footer line. Diagnostic scaffolding for this first slice; the
 * kruddgui editor console takes over once WebGPU can drive the UI.
 */
EM_JS(void, webgpu_status, (const char *msg), {
	var s = UTF8ToString(msg);
	if (typeof window.kruddWebGPULog === 'function')
		window.kruddWebGPULog(s);
	if (typeof console !== 'undefined')
		console.log('[webgpu] ' + s);
})

/* A WGPUStringView over a NUL-terminated C string (the emdawnwebgpu string ABI). */
static WGPUStringView str_view(const char *s)
{
	WGPUStringView v;

	v.data   = s;
	v.length = strlen(s);
	return v;
}

/*
 * Match the surface to the canvas and the device's preferred format. Called
 * once the device arrives; the probe reconfigures nowhere else (a resize hook
 * comes with the real backend).
 */
static void configure_surface(void)
{
	double css_w = 0.0;
	double css_h = 0.0;
	int w;
	int h;
	WGPUSurfaceCapabilities caps;
	WGPUSurfaceConfiguration cfg;

	/*
	 * A WebGPU surface draws into the canvas's backing store, whose size is
	 * the canvas.width/height attributes — not its CSS size. Nothing sets
	 * those in WebGPU mode (the WebGL path used to, via the GL context), so
	 * match the backing store to the element's CSS size here; otherwise the
	 * drawing buffer stays 0x0 and nothing is visible however well the clear
	 * runs. A resize hook comes with the real backend.
	 */
	emscripten_get_element_css_size("#canvas", &css_w, &css_h);
	w = (int)css_w;
	h = (int)css_h;
	if (w <= 0)
		w = 800;
	if (h <= 0)
		h = 600;
	emscripten_set_canvas_element_size("#canvas", w, h);

	memset(&caps, 0, sizeof(caps));
	wgpuSurfaceGetCapabilities(g_surface, g_adapter, &caps);
	g_format = (caps.formatCount > 0) ? caps.formats[0]
					  : WGPUTextureFormat_BGRA8Unorm;

	memset(&cfg, 0, sizeof(cfg));
	cfg.device      = g_device;
	cfg.format      = g_format;
	cfg.usage       = WGPUTextureUsage_RenderAttachment;
	cfg.width       = (uint32_t)w;
	cfg.height      = (uint32_t)h;
	cfg.alphaMode   = WGPUCompositeAlphaMode_Opaque;
	cfg.presentMode = WGPUPresentMode_Fifo;
	wgpuSurfaceConfigure(g_surface, &cfg);

	wgpuSurfaceCapabilitiesFreeMembers(caps);

	{
		char buf[96];

		snprintf(buf, sizeof(buf), "webgpu: surface %dx%d format=%d",
			 w, h, (int)g_format);
		webgpu_status(buf);
	}
}

static void on_device(WGPURequestDeviceStatus status, WGPUDevice device,
		      WGPUStringView message, void *ud1, void *ud2)
{
	(void)message;
	(void)ud1;
	(void)ud2;
	if (status != WGPURequestDeviceStatus_Success || !device) {
		webgpu_status("webgpu: device request failed");
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgpu: device request failed");
		return;
	}
	g_device = device;
	g_queue  = wgpuDeviceGetQueue(device);
	configure_surface();
	g_ready = 1;
	webgpu_announce_renderer();
	webgpu_status("webgpu: device ready — clearing fuchsia");
	g_log->write(LOG_LEVEL_INFO, "renderer_webgpu: device ready");
}

static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
		       WGPUStringView message, void *ud1, void *ud2)
{
	WGPURequestDeviceCallbackInfo ci;

	(void)message;
	(void)ud1;
	(void)ud2;
	if (status != WGPURequestAdapterStatus_Success || !adapter) {
		webgpu_status("webgpu: no adapter (WebGPU unavailable?)");
		g_log->write(LOG_LEVEL_ERROR,
			     "renderer_webgpu: adapter request failed");
		return;
	}
	g_adapter = adapter;
	webgpu_status("webgpu: adapter ok — requesting device");

	memset(&ci, 0, sizeof(ci));
	ci.mode     = WGPUCallbackMode_AllowSpontaneous;
	ci.callback = on_device;
	wgpuAdapterRequestDevice(adapter, NULL, ci);
}

static void renderer_webgpu_init(void)
{
	WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas_src;
	WGPUSurfaceDescriptor sd;
	WGPURequestAdapterCallbackInfo ci;

	g_instance = wgpuCreateInstance(NULL);
	if (!g_instance) {
		webgpu_status("webgpu: no instance");
		g_log->write(LOG_LEVEL_ERROR, "renderer_webgpu: no instance");
		return;
	}
	webgpu_status("webgpu: requesting adapter");

	memset(&canvas_src, 0, sizeof(canvas_src));
	canvas_src.chain.sType =
		WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
	canvas_src.selector = str_view("#canvas");

	memset(&sd, 0, sizeof(sd));
	sd.nextInChain = &canvas_src.chain;
	g_surface = wgpuInstanceCreateSurface(g_instance, &sd);

	memset(&ci, 0, sizeof(ci));
	ci.mode     = WGPUCallbackMode_AllowSpontaneous;
	ci.callback = on_adapter;
	wgpuInstanceRequestAdapter(g_instance, NULL, ci);

	g_log->write(LOG_LEVEL_INFO,
		     "renderer_webgpu: init (requesting adapter)");
}

/* Clear the current surface texture to fuchsia — the whole point of the probe. */
static void renderer_webgpu_tick(void)
{
	WGPUSurfaceTexture st;
	WGPUTextureView view;
	WGPUCommandEncoder enc;
	WGPURenderPassColorAttachment color;
	WGPURenderPassDescriptor rp;
	WGPURenderPassEncoder pass;
	WGPUCommandBuffer cmd;

	/*
	 * Pump the instance so the adapter/device futures resolve. AllowSpontaneous
	 * callbacks should fire on their own, but processing events each frame is
	 * harmless and covers backends that need the nudge — and it must run before
	 * the not-ready early-out, or the device would never arrive.
	 */
	if (g_instance)
		wgpuInstanceProcessEvents(g_instance);

	if (!g_ready)
		return;

	memset(&st, 0, sizeof(st));
	wgpuSurfaceGetCurrentTexture(g_surface, &st);
	if (!st.texture)
		return;

	view = wgpuTextureCreateView(st.texture, NULL);
	enc  = wgpuDeviceCreateCommandEncoder(g_device, NULL);

	memset(&color, 0, sizeof(color));
	color.view          = view;
	color.depthSlice    = WGPU_DEPTH_SLICE_UNDEFINED;
	color.loadOp        = WGPULoadOp_Clear;
	color.storeOp       = WGPUStoreOp_Store;
	color.clearValue.r  = 1.0;
	color.clearValue.g  = 0.0;
	color.clearValue.b  = 1.0;
	color.clearValue.a  = 1.0;

	memset(&rp, 0, sizeof(rp));
	rp.colorAttachmentCount = 1;
	rp.colorAttachments     = &color;

	pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
	wgpuRenderPassEncoderEnd(pass);

	cmd = wgpuCommandEncoderFinish(enc, NULL);
	wgpuQueueSubmit(g_queue, 1, &cmd);

	wgpuCommandBufferRelease(cmd);
	wgpuRenderPassEncoderRelease(pass);
	wgpuCommandEncoderRelease(enc);
	wgpuTextureViewRelease(view);
	wgpuTextureRelease(st.texture);
}

static void renderer_webgpu_shutdown(void)
{
	g_ready = 0;
	g_log->write(LOG_LEVEL_INFO, "renderer_webgpu: shutdown");
}

static const struct subsystem desc = {
	.name     = "renderer",
	.init     = renderer_webgpu_init,
	.tick     = renderer_webgpu_tick,
	.shutdown = renderer_webgpu_shutdown,
};

void renderer_webgpu_plugin_entry(struct subsystem_manager *mgr)
{
	g_log = subsystem_manager_get_api(mgr, "log");
	subsystem_manager_register(mgr, &desc);
}
#else
/*
 * Native builds never drive WebGPU (it is browser-only), so the entry is a
 * no-op that keeps the module compiling and linking into the native image.
 */
void renderer_webgpu_plugin_entry(struct subsystem_manager *mgr)
{
	(void)mgr;
}
#endif
