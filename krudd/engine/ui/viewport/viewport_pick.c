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
 * Split in two at the ray (#996). viewport_pick_ray is the whole of the
 * raycast and takes the ray as a ray; viewport_pick_entity is that call with an
 * unprojection in front of it. The split is not a refactor for its own sake: a
 * screen pointer HAS no ray until a camera makes one, and an XR controller IS
 * one already — pose plus forward axis — so the second caller wants in one step
 * later. Nothing about ray_tri_intersect or the mesh walk changed with the
 * split; the screen-space path runs the identical loop over the identical ray
 * it built before, which is what viewport_pick_test's equivalence case pins.
 *
 * The math it picks with (ray_from_screen / ray_tri_intersect / mat4_*) and
 * mesh_script_generate resolve against the single copies the renderer and the
 * mesh_script library already provide, at the final wasm module link.
 */
#include <viewport/viewport_pick.h>

#include <entity/world.h>
#include <abi/asset_api.h>
#include <abi/memory_api.h>
#include <asset/mesh.h>
#include <asset/mesh_script.h>
#include <math/math_types.h>

#include <float.h>

/* Whether e is one of the ids the caller took out of the running. */
static int32_t is_ignored(int32_t e, const int32_t *ignore, int32_t count)
{
	int32_t i;

	for (i = 0; i < count; i++) {
		if (ignore[i] == e)
			return 1;
	}
	return 0;
}

int32_t viewport_pick_ray(const struct world *w,
			  const float origin[3], const float dir[3],
			  const int32_t *ignore, int32_t ignore_count,
			  const struct asset_api *asset,
			  const struct memory_api *mem)
{
	int32_t  best   = -1;
	float    best_t = FLT_MAX;
	uint32_t e;

	if (!w || !origin || !dir || !asset || !mem)
		return -1;
	if (!ignore)
		ignore_count = 0;
	/*
	 * A zero direction is not a ray. ray_tri_intersect would divide the
	 * barycentric terms by a zero determinant on every triangle, so this is
	 * refused here rather than left to produce whichever NaN comparison
	 * happens to pass.
	 */
	if (dir[0] == 0.0f && dir[1] == 0.0f && dir[2] == 0.0f)
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
		if (is_ignored((int32_t)e, ignore, ignore_count))
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

int32_t viewport_pick_entity(const struct world *w,
			     const struct mat4 *view_proj,
			     float sx, float sy, float vw, float vh,
			     const struct asset_api *asset,
			     const struct memory_api *mem)
{
	float origin[3];
	float dir[3];

	if (!view_proj)
		return -1;
	if (ray_from_screen(view_proj, sx, sy, vw, vh, origin, dir) != 0)
		return -1;
	return viewport_pick_ray(w, origin, dir, NULL, 0, asset, mem);
}
