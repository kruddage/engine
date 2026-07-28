/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VIEWPORT_API_H
#define VIEWPORT_API_H

#include <stdint.h>

/*
 * viewport_api — the "viewport" subsystem's vtable: what the drawn surface is,
 * what is under a pixel of it, and how big a thing is (#949).
 *
 * ui/viewport already owned the raycast and the camera-aspect sync, for the
 * kruddgui pointer. This publishes both to anything that resolves the
 * subsystem, and the bridge is the consumer that matters: the editor is a React
 * application beside the canvas, its pointer events never reach kruddgui's
 * router, and it therefore has to ask for a pick rather than have one happen to
 * it.
 *
 * ## Who reports the size
 *
 * Two things can say how big the viewport is and they disagree in the editor.
 * kruddgui measures the canvas element; the editor knows the pixel rect its own
 * layout gave the canvas, including a device-pixel-ratio change kruddgui sees a
 * frame later. set_size lets the owner of the pointer be the owner of the
 * geometry as well — which is the rule #949 settles: in editor mode the editor
 * owns both, in game mode kruddgui owns both. Whoever is not the owner does not
 * write.
 *
 * A pick is answered against the size last reported, whichever of them reported
 * it, so the ray and the drawn pixels come from one number rather than two.
 */
struct viewport_api {
	/*
	 * Report the drawable size in CSS pixels. Non-positive dimensions are
	 * ignored — a canvas measured while its dock is collapsed reports 0,
	 * and adopting that would make the next pick divide by it.
	 */
	void (*set_size)(float width, float height);

	/* The size a pick would be answered against. */
	void (*get_size)(float *width, float *height);

	/*
	 * The live render entity whose mesh is nearest the camera under pixel
	 * (sx, sy) — a top-left origin, in the reported size's pixels — or -1
	 * on a miss.
	 *
	 * The ray is built from the exact view·projection the forward pass
	 * draws with, and the geometry from the same mesh_script_generate call
	 * with the same per-entity parameter override. That is what makes the
	 * thing you click the thing you see (#813); a hit-test generated any
	 * other way drifts, and a wall you can see and walk through is what
	 * drift looks like.
	 */
	int32_t (*pick)(float sx, float sy);

	/*
	 * A world-space bounding sphere for one entity: the centre into
	 * centre[3] and the radius into *radius. Returns 1 on success, 0 when
	 * the entity is dead, has no mesh, or the mesh could not be generated.
	 *
	 * Frame-selection is the consumer. It is here rather than in the camera
	 * because it is a question about the world, and the mesh it measures is
	 * the one the pick and the draw both use.
	 */
	int32_t (*bounds)(int32_t entity, float centre[3], float *radius);
};

#endif /* VIEWPORT_API_H */
