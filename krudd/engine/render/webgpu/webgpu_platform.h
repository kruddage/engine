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

/*
 * Backbuffer size in physical pixels, never zero — a platform with nothing
 * sensible to report substitutes a default rather than handing back a 0x0
 * drawing buffer, which renders as a blank screen with no error anywhere.
 */
void webgpu_platform_backbuffer_size(uint32_t *w, uint32_t *h);

/* Progress reporting, for a target that may have no console attached. */
void webgpu_platform_status(const char *msg);

/* Tell the host WebGPU went live (the shell's renderer badge; a no-op natively). */
void webgpu_platform_announce_renderer(void);

#endif /* KRUDD_WEBGPU_PLATFORM_H */
