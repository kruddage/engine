/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * viewport_pick — the shared click-to-pick raycast (#697).
 *
 * Lifted out of ui/viewport/viewport.c's pick_entity_at, which was fenced under
 * __EMSCRIPTEN__ behind the kruddgui pointer. The raycast itself is plain CPU
 * math over the entity world — nothing web-specific — so both the wasm viewport
 * overlay (which feeds it the kruddgui viewport + camera) and the native Qt
 * shell (which feeds it the window size + camera) call this one copy rather than
 * each hand-rolling — and drifting from — the same ray/triangle sweep.
 *
 * The math it picks with (ray_from_screen / ray_tri_intersect / mat4_*) and
 * mesh_script_generate resolve against the single copies the renderer and the
 * mesh_script library already provide — for wasm at the final module link, for
 * native at the executable link (krudd_qt already links scene_renderer, which
 * carries the math).
 */
#include "viewport_pick.h"

#include "world.h"
#include "asset_api.h"
#include "memory_api.h"
#include "mesh.h"
#include "mesh_script.h"
#include "math_types.h"

#include <float.h>

int32_t viewport_pick_entity(const struct world *w,
			     const struct mat4 *view_proj,
			     float sx, float sy, float vw, float vh,
			     const struct asset_api *asset,
			     const struct memory_api *mem)
{
	float    origin[3];
	float    dir[3];
	int32_t  best   = -1;
	float    best_t = FLT_MAX;
	uint32_t e;

	if (!w || !view_proj || !asset || !mem)
		return -1;
	if (ray_from_screen(view_proj, sx, sy, vw, vh, origin, dir) != 0)
		return -1;

	for (e = 0; e < w->count; e++) {
		struct mesh_blob         *blob;
		const struct mesh_vertex *vtx;
		const uint16_t           *idx;
		const char               *src;
		const uint8_t            *mp;
		uint32_t                  mplen = 0;
		struct mat4               model;
		uint32_t                  i;

		if (!w->alive[e] || !(w->mask[e] & COMPONENT_RENDER))
			continue;
		src = (const char *)asset->get_data(w->render_ref[e], NULL);
		if (!src)
			continue;
		mp   = world_mesh_params(w, e, &mplen);
		blob = mesh_script_generate(src, mp, mplen, mem, NULL);
		if (!blob)
			continue;

		mat4_from_transform(&model, &w->world_xform[e]);
		vtx = mesh_blob_vertices(blob);
		idx = mesh_blob_indices(blob);

		for (i = 0; i + 3 <= blob->index_count; i += 3) {
			float a[3], b[3], c[3];
			float t;

			mat4_transform_point(a, &model, vtx[idx[i]].position);
			mat4_transform_point(b, &model, vtx[idx[i + 1]].position);
			mat4_transform_point(c, &model, vtx[idx[i + 2]].position);
			if (ray_tri_intersect(origin, dir, a, b, c, &t) &&
			    t < best_t) {
				best_t = t;
				best   = (int32_t)e;
			}
		}
		mem->free(blob);
	}
	return best;
}
