/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The platform seam for the WebGPU backend.
 *
 * renderer_webgpu.c is the same code on both targets — the WebGPU API it drives
 * is Dawn either way (the web build links Dawn through
 * `--use-port=emdawnwebgpu`, the native build links Dawn's own
 * libwebgpu_dawn.a at the same pinned revision). What genuinely differs is
 * narrow, and it is all here:
 *
 *   - where the backbuffer comes from: a canvas-backed WGPUSurface in the
 *     browser, and nothing at all natively, where rendering is offscreen;
 *   - how big it is: the canvas element's CSS size, or a fixed configured size;
 *   - where progress messages go: the shell's DOM log panel, or stderr.
 *
 * Keeping the seam this small is the point. The native target exists to be a
 * debugger on the code the browser actually runs, and every branch that is not
 * in this file is a branch where the two targets can silently disagree.
 */
#ifndef KRUDD_WEBGPU_PLATFORM_H
#define KRUDD_WEBGPU_PLATFORM_H

#include <stdint.h>
#include <webgpu/webgpu.h>

/*
 * The surface the frame presents into, or NULL when this platform renders
 * offscreen. A NULL return is a supported answer, not a failure: natively there
 * is no window and no swapchain by design (see spec-dawn-native-build), and the
 * backend configures itself for an offscreen target instead.
 */
WGPUSurface webgpu_platform_create_surface(WGPUInstance instance);

#ifndef __EMSCRIPTEN__
/*
 * Native windowing, injected from the outside.
 *
 * The native seam above answers "offscreen" by default — NULL surface, a fixed
 * backbuffer size, a persistent readable texture — because that is what the CI
 * harness (krudd_native) and every headless render-diff need, and it must build
 * with no window library in the picture at all.
 *
 * A windowed binary (krudd_window) supplies the missing half at runtime instead
 * of by a second #ifdef: it owns the window and the swapchain, and registers a
 * host here before the backend boots. With a host set, create_surface returns
 * that window's WGPUSurface, backbuffer_size reports the window's size, and the
 * offscreen backbuffer/readback path stays dark (there is a real swapchain now,
 * and the compositor owns those textures). With no host set — the default, and
 * the only state CI ever sees — the seam is byte-for-byte the offscreen one.
 *
 * Kept out here rather than compiled into the renderer_webgpu library so the
 * window library (SDL, and its X11/Wayland surface types) links only into the
 * one executable that wants it, never into the shared backend or the offscreen
 * harness.
 */
struct webgpu_platform_host {
	/* Create the presentation surface for the host's window. */
	WGPUSurface (*create_surface)(WGPUInstance instance, void *user);
	/* The window's current drawable size, in physical pixels. */
	void        (*drawable_size)(uint32_t *w, uint32_t *h, void *user);
	void        *user;
};

/* Register (or, with NULL, clear) the windowing host. Call before boot. */
void webgpu_platform_set_host(const struct webgpu_platform_host *host);
#endif

/*
 * Backbuffer size in physical (device) pixels, never zero — a platform with
 * nothing sensible to report substitutes a default rather than handing back a
 * 0x0 drawing buffer, which renders as a blank screen with no error anywhere.
 *
 * DEVICE pixels, not CSS pixels: on the web the two differ by devicePixelRatio,
 * and every colour/depth attachment in a pass must agree on one of them or the
 * pass fails validation. Read-only — the canvas backing store belongs to
 * kruddgui_tick.
 */
void webgpu_platform_backbuffer_size(uint32_t *w, uint32_t *h);

/*
 * A view of the frame's colour target on a platform that has no surface, or
 * NULL where one exists (the backend acquires the surface texture instead).
 *
 * Returns a FRESH view each call and the caller owns it, matching the lifetime
 * of a surface texture's view so the backend's existing per-frame release path
 * works unchanged. The underlying texture is persistent and owned here, which
 * is what makes it readable after the frame — see
 * webgpu_platform_read_backbuffer.
 */
WGPUTextureView webgpu_platform_backbuffer_view(WGPUDevice device);

/*
 * Copy the offscreen backbuffer into `rgba` (w*h*4 bytes, top row first, tightly
 * packed). Returns 1 on success, 0 on a platform that presents through a
 * surface — there the backbuffer belongs to the compositor and is not ours to
 * read.
 */
int webgpu_platform_read_backbuffer(WGPUInstance instance, WGPUDevice device,
				    WGPUQueue queue, uint8_t *rgba);

/* Progress reporting, for a target that may have no console attached. */
void webgpu_platform_status(const char *msg);

/* Tell the host WebGPU went live (the shell's renderer badge; a no-op natively). */
void webgpu_platform_announce_renderer(void);

/*
 * Release any persistent resources this seam owns. Natively that is the
 * offscreen backbuffer texture (created lazily in webgpu_platform_backbuffer_view);
 * on the web there is nothing to free — the surface's textures belong to the
 * compositor. Called from renderer_webgpu_shutdown before the device is released.
 */
void webgpu_platform_teardown(void);

#endif /* KRUDD_WEBGPU_PLATFORM_H */
