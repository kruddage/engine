/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PREVIEW_API_H
#define PREVIEW_API_H

#include <stdint.h>

/*
 * The "mesh_preview" subsystem api — render one mesh asset, shaded, into an
 * offscreen target and hand back a texture an editor can display. Published by
 * scene_renderer (which owns the pipelines, the material UBO and the mesh upload
 * path), so a preview goes through the exact same shader/material pipeline a live
 * entity draws with rather than a separate approximation.
 *
 * The mesh inspector (kruddgui's Assets console) is the first consumer: it calls
 * render_mesh each frame the Preview fold is open and draws the result with
 * kgui-image, giving a lit, spinnable thumbnail of the authored geometry.
 */
struct preview_api {
	/*
	 * Render mesh_ref, shaded with material_ref, into a res x res offscreen
	 * RGBA target and return the backend-native texture handle (the GL
	 * texture name on the WebGL backend) for the caller to composite, or 0 on
	 * failure (no device, unknown mesh, target allocation failed, or a build
	 * with no live GL context). material_ref 0 selects the built-in default
	 * material, so a pure-geometry preview needs no material chosen. yaw is a
	 * model-space rotation about +Y in radians, so a caller can spin the mesh
	 * by advancing it each frame. res is clamped to a preview-sized edge.
	 *
	 * The returned handle names a texture scene_renderer owns and reuses
	 * across calls; it is valid until the next render_mesh call or renderer
	 * shutdown. The caller displays it the same frame and does not free it.
	 */
	uint32_t (*render_mesh)(uint32_t mesh_ref, uint32_t material_ref,
				uint32_t res, float yaw);
};

#endif /* PREVIEW_API_H */
