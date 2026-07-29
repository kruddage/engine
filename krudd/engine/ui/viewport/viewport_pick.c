/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * viewport_pick — the shared click-to-pick raycast (#697).
 *
 * Lifted out of ui/viewport/viewport.c's pick_entity_at, which was fenced under
 * __EMSCRIPTEN__ behind the kruddgui pointer. The raycast itself is plain CPU
 * math over the entity world — nothing web-specific — so it stays a file of its
 * own: the wasm viewport overlay feeds it the kruddgui viewport + camera, and
 * viewport_pick_test drives the same entry point with no window and no GPU,
 * against one copy rather than two that drift.
 *
 * The math it picks with (ray_from_screen / ray_tri_intersect / mat4_*) and
 * mesh_script_generate resolve against the single copies the renderer and the
 * mesh_script library already provide, at the final wasm module link.
 */
#include "viewport_pick.h"

#include "world.h"
#include "asset_api.h"
#include "memory_api.h"
#include "mesh.h"
#include "mesh_script.h"
#include "math_types.h"

#include <float.h>
#include <math.h>

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
		/*
		 * Hidden and locked are both unpickable (#950), and for the
		 * same reason from the reader's side: a click should land on
		 * the thing they can see and are allowed to move. Hidden is
		 * not drawn at all, so picking it would select something
		 * invisible; locked is drawn precisely so it can be a backdrop
		 * to work in front of.
		 */
		if (w->flags[e] & (WORLD_ENTITY_HIDDEN | WORLD_ENTITY_LOCKED))
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

int32_t viewport_entity_bounds(const struct world *w, int32_t entity,
			       float centre[3], float *radius,
			       const struct asset_api *asset,
			       const struct memory_api *mem)
{
	struct mesh_blob         *blob;
	const struct mesh_vertex *vtx;
	const char               *src;
	const uint8_t            *mp;
	uint32_t                  mplen = 0;
	struct mat4               model;
	float                     lo[3], hi[3];
	uint32_t                  i;
	int                       c;

	if (!w || !centre || !radius || !asset || !mem)
		return 0;
	if (entity < 0 || (uint32_t)entity >= w->count || !w->alive[entity])
		return 0;
	if (!(w->mask[entity] & COMPONENT_RENDER))
		return 0;

	src = (const char *)asset->get_data(w->render_ref[entity], NULL);
	if (!src)
		return 0;
	mp   = world_mesh_params(w, (uint32_t)entity, &mplen);
	blob = mesh_script_generate(src, mp, mplen, mem, NULL);
	if (!blob)
		return 0;

	vtx = mesh_blob_vertices(blob);
	if (blob->vertex_count == 0) {
		mem->free(blob);
		return 0;
	}

	/*
	 * The world-space axis-aligned box of the transformed vertices, then
	 * the sphere around it. Every vertex is transformed rather than the box
	 * corners being transformed afterwards: a rotated mesh's world box is
	 * not the rotation of its local box, and the cheap version of this is
	 * how a framed selection ends up slightly too small on exactly the
	 * entities a reader rotated.
	 */
	mat4_from_transform(&model, &w->world_xform[entity]);
	for (i = 0; i < blob->vertex_count; i++) {
		float p[3];

		mat4_transform_point(p, &model, vtx[i].position);
		for (c = 0; c < 3; c++) {
			if (i == 0 || p[c] < lo[c])
				lo[c] = p[c];
			if (i == 0 || p[c] > hi[c])
				hi[c] = p[c];
		}
	}
	mem->free(blob);

	*radius = 0.0f;
	for (c = 0; c < 3; c++) {
		float half = (hi[c] - lo[c]) * 0.5f;

		centre[c] = (lo[c] + hi[c]) * 0.5f;
		*radius  += half * half;
	}
	*radius = sqrtf(*radius);
	/* A degenerate mesh still deserves a framing that shows where it is. */
	if (*radius < 1e-3f)
		*radius = 1e-3f;
	return 1;
}
