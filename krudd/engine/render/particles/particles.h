/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PARTICLES_H
#define PARTICLES_H

#include "renderer.h"

#include <stdint.h>

/*
 * particles — a CPU-simulated, GPU-rendered cosmetic particle system.
 *
 * This is the WebGL-era particle path, and deliberately the simplest thing that
 * works on every backend the engine has: the CPU integrates a fixed pool each
 * frame, bakes the live particles into camera-facing quads, and draws the whole
 * batch with one gpu_api cmd_draw (alpha-blended). It leans on nothing beyond
 * the primitives every backend already provides — a dynamic vertex buffer plus a
 * non-indexed draw, exactly the seam kruddgui's 2D batch rides on — so it needs
 * no compute, no instancing, and no ABI growth.
 *
 * COMPUTE PATH (future): when the WebGPU backend lands compute (gpu_api already
 * declares GPU_CAP_COMPUTE, cmd_dispatch, storage buffers and a compute stage
 * mask — see renderer.scm), the simulate-on-the-CPU-then-upload step is the ONE
 * thing that changes: a compute kernel writes the per-particle buffer that stays
 * resident on the GPU, and this render path draws from it unchanged. The manager
 * picks the producer by capability (gpu->caps & GPU_CAP_COMPUTE); everything
 * downstream here is shared. Keep that seam — particles_update() feeding the
 * buffer particles_render() draws — intact when the compute path is added.
 *
 * The pool is a fixed, module-global cap: bursts past it are dropped rather than
 * grown, so a runaway emitter costs a bounded amount of memory and upload each
 * frame. Cosmetic only — nothing here feeds back into gameplay, so the two
 * backends are free to diverge in density without desyncing anything.
 */

/*
 * Create the particle pipeline and the dynamic vertex/uniform buffers against
 * DEVICE, off-frame. Safe to call once; a NULL device leaves the system inert
 * (every later call a no-op). Resources are created here, never per-frame, so
 * the render path allocates nothing mid-pass.
 */
void particles_init(const struct gpu_api *device);

/*
 * Emit COUNT particles at world position POS[3] tinted RGB[3] (0..1), fountaining
 * outward with an upward bias so they arc up and fall back under gravity. COUNT
 * is clamped to the free pool space; a burst that would overflow the cap fills
 * what it can and drops the rest. Cheap to call every placement.
 */
void particles_burst(const float pos[3], const float rgb[3], uint32_t count);

/*
 * Advance every live particle by DT seconds (integrate position, apply gravity,
 * age out the expired by swap-remove). This is the producer half of the seam the
 * compute path will one day replace — see the file header.
 */
void particles_update(float dt);

/*
 * Bake the live particles into camera-facing quads (built from CAM_RIGHT[3] /
 * CAM_UP[3], the world-space camera basis) and draw them in one alpha-blended
 * cmd_draw, transformed by VIEW_PROJ[16]. Call inside an open render pass, after
 * the opaque scene, with CMD the pass's command buffer. A no-op when the pool is
 * empty, so an idle frame adds zero GPU calls.
 */
void particles_render(const struct gpu_api *device, gpu_cmd_buf_t cmd,
		      const float view_proj[16], const float cam_right[3],
		      const float cam_up[3]);

/* Live particle count — the pool occupancy, for tests and diagnostics. */
uint32_t particles_live_count(void);

#endif /* PARTICLES_H */
