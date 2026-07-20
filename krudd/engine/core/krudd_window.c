/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * krudd_window — the WebGPU backend, presenting into a real native window.
 *
 * A proof of life for a native editor on SteamOS / the Steam Deck: clone the
 * repo, `./krudd.sh editor`, and the engine's WebGPU backend boots against
 * native Dawn (Vulkan on the Deck's RDNA2) and draws into an SDL3 window on
 * Wayland — no browser, no Emscripten, nothing web in the path.
 *
 * It is the windowed sibling of engine_native.c. That harness proves the backend
 * reaches a colour target *offscreen* (readback → PNG, so CI can trust it with no
 * GPU). This one takes the same backend, the same pinned Dawn, and swaps the
 * offscreen target for a live swapchain: it registers a webgpu_platform host that
 * hands the backend the window's WGPUSurface, then presents every frame. The
 * whole "does the native window → surface → Dawn → present chain actually light
 * up" question — the one thing neither the browser nor the offscreen harness can
 * answer — is what this binary exists to answer, on real hardware.
 *
 * Scope is deliberately small. It does NOT run engine.c's full boot: that boot
 * is Emscripten-only and drags in the whole plugin table (the IndexedDB-backed
 * asset store, the canvas UI, fetch). Porting that off the browser is the editor
 * work this is a proof of life *for*. So the loop here drives the backend
 * directly through the gpu_api vtable — an animated clear, redrawn every frame —
 * which exercises surface configuration, per-frame acquire, a render pass, submit
 * and present: the entire native presentation path, and nothing that still
 * assumes a browser.
 */
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log.h"
#include "log_api.h"
#include "memory.h"
#include "memory_api.h"
#include "script.h"
#include "renderer.h"        /* struct gpu_api — the backend's vtable */
#include "webgpu_platform.h" /* the native windowing host seam */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

void renderer_webgpu_plugin_entry(struct subsystem_manager *mgr);
int  renderer_webgpu_device_ready(void);

/* Mirrors engine.c's core service table (static to an Emscripten-only TU, so it
 * cannot be shared) — the same pair engine_native.c stands up. */
static const struct log_api g_log_api = {
	.write       = log_write,
	.get_history = log_get_history,
};

static const struct memory_api g_mem_api = {
	.alloc        = mem_alloc,
	.alloc_zero   = mem_alloc_zero,
	.free         = mem_free,
	.pool_create  = mem_pool_create,
	.pool_alloc   = mem_pool_alloc,
	.pool_free    = mem_pool_free,
	.pool_destroy = mem_pool_destroy,
};

static const struct subsystem subsystems[] = {
	{ .name = "log",    .api = &g_log_api, .init = log_init, .shutdown = log_shutdown },
	{ .name = "memory", .api = &g_mem_api, .init = mem_init, .shutdown = mem_shutdown },
	{ NULL }
};

static struct subsystem_manager manager;

/* ------------------------------------------------------- the window host */

/*
 * Build the WGPUSurface for our SDL window. Called by the backend through the
 * webgpu_platform host, once, at device bring-up.
 *
 * SDL owns the platform window; Dawn needs the raw platform handles behind it.
 * SDL3 exposes them as window properties, and Dawn takes them through a
 * platform-specific chained descriptor. Wayland is the SteamOS path (Desktop
 * Mode is KDE on Wayland, Game Mode is gamescope, also Wayland); X11 is kept as
 * an XWayland fallback so a plain X session still works.
 *
 * NOTE ON DAWN TYPE NAMES: the WGPUSurfaceSource* structs and their WGPUSType_*
 * tags track Dawn's webgpu.h at the pinned revision (tools/dawn-smoke/README.md).
 * If a Dawn roll renames them, this one function is the only thing to adjust —
 * see docs/steamos-window.md.
 */
static WGPUSurface window_create_surface(WGPUInstance instance, void *user)
{
	SDL_Window *win = (SDL_Window *)user;
	SDL_PropertiesID props = SDL_GetWindowProperties(win);
	const char *driver = SDL_GetCurrentVideoDriver();
	WGPUSurfaceDescriptor sd;

	memset(&sd, 0, sizeof(sd));

	if (driver && strcmp(driver, "wayland") == 0) {
		struct wl_display *display = SDL_GetPointerProperty(
			props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
		struct wl_surface *surface = SDL_GetPointerProperty(
			props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
		WGPUSurfaceSourceWaylandSurface src;

		if (!display || !surface) {
			fprintf(stderr, "krudd_window: no Wayland handles\n");
			return NULL;
		}
		memset(&src, 0, sizeof(src));
		src.chain.sType = WGPUSType_SurfaceSourceWaylandSurface;
		src.display     = display;
		src.surface     = surface;
		sd.nextInChain  = &src.chain;
		return wgpuInstanceCreateSurface(instance, &sd);
	}

	if (driver && strcmp(driver, "x11") == 0) {
		void *display = SDL_GetPointerProperty(
			props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
		Sint64 window = SDL_GetNumberProperty(
			props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
		WGPUSurfaceSourceXlibWindow src;

		if (!display || !window) {
			fprintf(stderr, "krudd_window: no X11 handles\n");
			return NULL;
		}
		memset(&src, 0, sizeof(src));
		src.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
		src.display     = display;
		src.window      = (uint64_t)window;
		sd.nextInChain  = &src.chain;
		return wgpuInstanceCreateSurface(instance, &sd);
	}

	fprintf(stderr, "krudd_window: unsupported SDL video driver '%s' "
			"(want wayland or x11)\n", driver ? driver : "(none)");
	return NULL;
}

static void window_drawable_size(uint32_t *w, uint32_t *h, void *user)
{
	SDL_Window *win = (SDL_Window *)user;
	int pw = 0;
	int ph = 0;

	SDL_GetWindowSizeInPixels(win, &pw, &ph);
	*w = (pw > 0) ? (uint32_t)pw : 1u;
	*h = (ph > 0) ? (uint32_t)ph : 1u;
}

/* ------------------------------------------------------------- the frame */

/*
 * One frame: clear the backbuffer to an animated colour and present it. A
 * colour attachment with a NULL texture is the backbuffer by convention (the
 * same convention fg_import_backbuffer and every backend follow); a clear load
 * op with no draws needs no pipeline and no depth, so this is the whole of the
 * GPU work. The animation is what makes it a proof of *life*: a moving picture
 * is the swapchain actually cycling, not one lucky frame frozen on screen.
 */
static void draw_frame(const struct gpu_api *gpu, uint32_t frame)
{
	struct gpu_render_pass_desc rp;
	gpu_cmd_buf_t cmd;
	float t = (float)frame * 0.02f;

	memset(&rp, 0, sizeof(rp));
	rp.color_count        = 1;
	rp.color[0].texture   = NULL; /* the backbuffer */
	rp.color[0].load_op   = GPU_LOAD_OP_CLEAR;
	rp.color[0].store_op  = GPU_STORE_OP_STORE;
	rp.color[0].clear[0]  = 0.5f + 0.5f * sinf(t);
	rp.color[0].clear[1]  = 0.5f + 0.5f * sinf(t + 2.094f);
	rp.color[0].clear[2]  = 0.5f + 0.5f * sinf(t + 4.188f);
	rp.color[0].clear[3]  = 1.0f;

	cmd = gpu->cmd_buf_begin();
	gpu->cmd_begin_render_pass(cmd, &rp);
	gpu->cmd_end_render_pass(cmd);
	gpu->cmd_buf_submit(cmd);
	gpu->frame_end(); /* releases the frame's surface texture and presents */
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
	SDL_Window *win;
	struct webgpu_platform_host host;
	const struct gpu_api *gpu;
	uint32_t frame = 0;
	int ready = 0;
	int i;

	(void)argc;
	(void)argv;

	/* Prefer Wayland (SteamOS), fall back to X11/XWayland. */
	SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland,x11");
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "krudd_window: SDL_Init failed: %s\n",
			SDL_GetError());
		return 1;
	}

	/* 1280x800 is the Deck's native panel; not resizable, so the surface
	 * configured once at boot stays valid for the life of the window. */
	win = SDL_CreateWindow("krudd", 1280, 800, 0);
	if (!win) {
		fprintf(stderr, "krudd_window: SDL_CreateWindow failed: %s\n",
			SDL_GetError());
		SDL_Quit();
		return 1;
	}
	printf("krudd_window: SDL video driver = %s\n",
	       SDL_GetCurrentVideoDriver());

	/*
	 * Register the window host BEFORE the backend boots: renderer_webgpu_init
	 * asks the platform seam for a surface during bring-up, and the host is
	 * what turns that from "offscreen" into "this window".
	 */
	memset(&host, 0, sizeof(host));
	host.create_surface = window_create_surface;
	host.drawable_size  = window_drawable_size;
	host.user           = win;
	webgpu_platform_set_host(&host);

	subsystem_manager_init(&manager, subsystems);

	/* The shader transpiler lives in the Scheme image; the backend lowers
	 * through it while building pipelines. Same as engine_native. */
	script_init();

	renderer_webgpu_plugin_entry(&manager);

	/* The adapter/device handshake is async even natively; the backend's tick
	 * pumps the instance, so ticking is how the device arrives. */
	for (i = 0; i < 1000 && !ready; i++) {
		subsystem_manager_tick(&manager);
		ready = renderer_webgpu_device_ready();
	}
	if (!ready) {
		fprintf(stderr, "krudd_window: device never became ready\n");
		webgpu_platform_set_host(NULL);
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}
	printf("krudd_window: device ready after %d tick(s)\n", i);

	gpu = subsystem_manager_get_api(&manager, "renderer");
	if (!gpu) {
		fprintf(stderr, "krudd_window: no renderer api\n");
		webgpu_platform_set_host(NULL);
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}

	printf("krudd_window: presenting — close the window or press Esc "
	       "to quit\n");
	for (;;) {
		SDL_Event e;
		int quit = 0;

		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_EVENT_QUIT)
				quit = 1;
			else if (e.type == SDL_EVENT_KEY_DOWN &&
				 e.key.key == SDLK_ESCAPE)
				quit = 1;
		}
		if (quit)
			break;

		subsystem_manager_tick(&manager); /* pump instance events */
		draw_frame(gpu, frame++);
	}

	/* Clear the host before the backend releases the surface it created. */
	webgpu_platform_set_host(NULL);
	subsystem_manager_shutdown(&manager);
	SDL_DestroyWindow(win);
	SDL_Quit();
	printf("krudd_window: bye (%u frame(s))\n", frame);
	return 0;
}
