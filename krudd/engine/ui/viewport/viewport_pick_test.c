/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * viewport_pick — the shared click-to-pick raycast, GPU-free.
 *
 * Boot the real s7 image (so mesh_script_generate can lower a mesh), stand up a
 * one-entity world holding the built-in unit box at the origin, and build a real
 * view·projection with the camera looking down the +Z axis at it. A ray through
 * the centre pixel must strike the box; the guard paths (NULL args, a ray off
 * the box, a tombstoned entity) must all return "no hit". This pins the raycast
 * the native Qt shell and the wasm overlay share, without a window or a GPU.
 */
#include "viewport_pick.h"

#include "world.h"
#include "asset_api.h"
#include "camera.h"
#include "math_types.h"
#include "mesh.h"
#include "builtin_mesh_scripts.h"

#include "script.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const struct memory_api g_mem = {
	mem_alloc, mem_alloc_zero, mem_free,
	mem_pool_create, mem_pool_alloc, mem_pool_free, mem_pool_destroy,
};

/* The box is catalog id 1; every other id has no data. viewport_pick only ever
 * calls get_data, so the rest of the asset_api stays NULL. */
#define BOX_REF 1u
static const void *fake_get_data(uint32_t id, uint32_t *out_size)
{
	if (out_size)
		*out_size = 0;
	if (id == BOX_REF)
		return BOX_MESH_SCRIPT_SRC;
	return NULL;
}
static const struct asset_api g_asset = { .get_data = fake_get_data };

static struct world g_world;

static void set_render_entity(uint32_t e, uint32_t ref,
			      float x, float y, float z)
{
	struct transform *t = &g_world.world_xform[e];

	g_world.alive[e]      = 1;
	g_world.mask[e]       = COMPONENT_RENDER;
	g_world.render_ref[e] = ref;
	t->position[0] = x; t->position[1] = y; t->position[2] = z;
	t->rotation[0] = 0.0f; t->rotation[1] = 0.0f;
	t->rotation[2] = 0.0f; t->rotation[3] = 1.0f;
	t->scale[0] = 1.0f; t->scale[1] = 1.0f; t->scale[2] = 1.0f;
}

/* A view·projection looking down +Z at the origin, framed for a (w,h) viewport. */
static void build_view_proj(struct mat4 *out, float w, float h)
{
	struct camera cam;

	memset(&cam, 0, sizeof(cam));
	cam.eye[0]    = 0.0f; cam.eye[1]    = 0.0f; cam.eye[2]    = 3.0f;
	cam.target[0] = 0.0f; cam.target[1] = 0.0f; cam.target[2] = 0.0f;
	cam.up[0]     = 0.0f; cam.up[1]     = 1.0f; cam.up[2]     = 0.0f;
	cam.fov_y  = 0.8f;
	cam.aspect = w / h;
	cam.near   = 0.1f;
	cam.far    = 100.0f;
	camera_update(&cam);
	*out = cam.view_proj;
}

int main(void)
{
	const float VW = 800.0f, VH = 600.0f;
	struct mat4 vp;
	int32_t     hit;

	mem_init();
	log_init();
	script_init(); /* loads the embedded mesh_script.scm image */

	build_view_proj(&vp, VW, VH);

	/* One box at the origin: a centre-pixel ray strikes it. */
	memset(&g_world, 0, sizeof(g_world));
	g_world.count = 1;
	set_render_entity(0, BOX_REF, 0.0f, 0.0f, 0.0f);
	hit = viewport_pick_entity(&g_world, &vp, VW * 0.5f, VH * 0.5f,
				   VW, VH, &g_asset, &g_mem);
	assert(hit == 0);

	/* NULL arguments never crash and never claim a hit. */
	assert(viewport_pick_entity(NULL, &vp, VW * 0.5f, VH * 0.5f,
				    VW, VH, &g_asset, &g_mem) == -1);
	assert(viewport_pick_entity(&g_world, NULL, VW * 0.5f, VH * 0.5f,
				    VW, VH, &g_asset, &g_mem) == -1);
	assert(viewport_pick_entity(&g_world, &vp, VW * 0.5f, VH * 0.5f,
				    VW, VH, NULL, &g_mem) == -1);
	assert(viewport_pick_entity(&g_world, &vp, VW * 0.5f, VH * 0.5f,
				    VW, VH, &g_asset, NULL) == -1);

	/* A tombstoned entity is not a candidate: the same centre ray misses. */
	g_world.alive[0] = 0;
	assert(viewport_pick_entity(&g_world, &vp, VW * 0.5f, VH * 0.5f,
				    VW, VH, &g_asset, &g_mem) == -1);

	/* A box shoved far off-axis is not under the centre pixel. */
	set_render_entity(0, BOX_REF, 100.0f, 0.0f, 0.0f);
	assert(viewport_pick_entity(&g_world, &vp, VW * 0.5f, VH * 0.5f,
				    VW, VH, &g_asset, &g_mem) == -1);

	printf("viewport_pick tests passed\n");
	return 0;
}
