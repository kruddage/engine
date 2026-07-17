/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * scene_script — host side of the scene-building layer.
 *
 * Registers the scene-* primitives the (scene ...) form calls, and drives one
 * build by handing a scene's source text to the image's scene-build. The
 * primitives spawn and bind entities in the world bound for the span of one
 * scene_script_build (set before the s7_call, cleared after), mirroring the
 * g_w discipline entity_script.c uses for its per-tick primitives.
 */
#include "scene_script.h"

#include "world.h"
#include "asset_api.h"
#include "script.h"

#include "s7.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SCENE_SCRIPT_PI 3.14159265358979323846

/*
 * The world and catalog a build acts on, valid only for the span of one
 * scene_script_build. A primitive can only run synchronously inside that call,
 * so no primitive ever observes a stale pointer.
 */
static struct world           *g_w;
static const struct asset_api *g_asset;

/* First list arg as an entity id, or -1 when it is not an integer. */
static int32_t arg_id(s7_pointer args)
{
	s7_pointer a = s7_car(args);

	return s7_is_integer(a) ? (int32_t)s7_integer(a) : -1;
}

/* The n-th list arg coerced to double, or 0.0 when it is not a number. */
static double arg_real(s7_scheme *sc, s7_pointer args, int32_t n)
{
	s7_pointer a = s7_list_ref(sc, args, n);

	return s7_is_number(a) ? s7_number_to_real(sc, a) : 0.0;
}

/* The n-th list arg as a C string, or NULL when it is not a string. */
static const char *arg_str(s7_scheme *sc, s7_pointer args, int32_t n)
{
	s7_pointer a = s7_list_ref(sc, args, n);

	return s7_is_string(a) ? s7_string(a) : NULL;
}

/* True when id names a live entity in the bound world. */
static int id_ok(int32_t id)
{
	return g_w && id >= 0 && (uint32_t)id < g_w->count && g_w->alive[id];
}

/*
 * Resolve a catalog PATH (e.g. "builtin://mesh/box") to its stable asset id, or
 * 0 when unknown. A linear catalog scan — a scene binds a handful of assets at
 * build time, not per frame, so the cost never matters.
 */
static uint32_t resolve_asset(const char *path)
{
	struct asset_info info;
	uint32_t          n, i;

	if (!g_asset || !path)
		return 0;
	n = g_asset->count();
	for (i = 0; i < n; i++) {
		if (g_asset->info(i, &info) == 0 && info.path
		    && strcmp(info.path, path) == 0)
			return info.id;
	}
	return 0;
}

/*
 * (scene-spawn [parent]) -> id: a new entity with an identity transform and an
 * empty component mask. PARENT is the entity id to nest under, or -1 / omitted
 * for a root entity. A child's transform clauses are read as local to its parent
 * (world_tick composes the two each frame), so a composite piece — an X built
 * from two crossed bars — moves and scales as one when its parent does.
 */
static s7_pointer sp_scene_spawn(s7_scheme *sc, s7_pointer args)
{
	struct transform t;
	int32_t          parent = WORLD_NO_PARENT;

	if (!g_w)
		return s7_make_integer(sc, -1);
	if (s7_is_pair(args) && s7_is_integer(s7_car(args)))
		parent = (int32_t)s7_integer(s7_car(args));
	memset(&t, 0, sizeof(t));
	t.rotation[3] = 1.0f;                       /* identity quaternion */
	t.scale[0] = t.scale[1] = t.scale[2] = 1.0f;
	return s7_make_integer(sc, world_create_entity(g_w, parent, &t, 0u));
}

/*
 * (scene-xform! id px py pz rx ry rz sx sy sz): set id's authored local
 * transform — position, intrinsic X-Y-Z Euler rotation in degrees, and scale.
 * The Euler-to-quaternion conversion mirrors entity-set-euler!, but writes the
 * authored pose (local) rather than a frame's animated pose (world_xform).
 */
static s7_pointer sp_scene_xform(s7_scheme *sc, s7_pointer args)
{
	int32_t id = arg_id(args);

	if (id_ok(id)) {
		struct transform t;
		double rx = arg_real(sc, args, 4) * (SCENE_SCRIPT_PI / 180.0) * 0.5;
		double ry = arg_real(sc, args, 5) * (SCENE_SCRIPT_PI / 180.0) * 0.5;
		double rz = arg_real(sc, args, 6) * (SCENE_SCRIPT_PI / 180.0) * 0.5;
		double cx = cos(rx), sx = sin(rx);
		double cy = cos(ry), sy = sin(ry);
		double cz = cos(rz), sz = sin(rz);

		t.position[0] = (float)arg_real(sc, args, 1);
		t.position[1] = (float)arg_real(sc, args, 2);
		t.position[2] = (float)arg_real(sc, args, 3);
		t.rotation[0] = (float)(sx * cy * cz - cx * sy * sz);
		t.rotation[1] = (float)(cx * sy * cz + sx * cy * sz);
		t.rotation[2] = (float)(cx * cy * sz - sx * sy * cz);
		t.rotation[3] = (float)(cx * cy * cz + sx * sy * sz);
		t.scale[0] = (float)arg_real(sc, args, 7);
		t.scale[1] = (float)arg_real(sc, args, 8);
		t.scale[2] = (float)arg_real(sc, args, 9);
		world_set_transform(g_w, id, &t);
	}
	return s7_unspecified(sc);
}

/* (scene-mesh! id "path"): bind id's mesh by catalog path (sets COMPONENT_RENDER). */
static s7_pointer sp_scene_mesh(s7_scheme *sc, s7_pointer args)
{
	int32_t id = arg_id(args);

	if (id_ok(id))
		world_set_render_ref(g_w, id, resolve_asset(arg_str(sc, args, 1)));
	return s7_unspecified(sc);
}

/* (scene-material! id "path"): bind id's material by catalog path. */
static s7_pointer sp_scene_material(s7_scheme *sc, s7_pointer args)
{
	int32_t id = arg_id(args);

	if (id_ok(id))
		world_set_material_ref(g_w, id,
				       resolve_asset(arg_str(sc, args, 1)));
	return s7_unspecified(sc);
}

/* (scene-script! id "path"): bind id's behavior script by catalog path. */
static s7_pointer sp_scene_script(s7_scheme *sc, s7_pointer args)
{
	int32_t id = arg_id(args);

	if (id_ok(id))
		world_set_script_ref(g_w, id,
				     resolve_asset(arg_str(sc, args, 1)));
	return s7_unspecified(sc);
}

/* (scene-name! id "name"): set id's human-readable label. */
static s7_pointer sp_scene_name(s7_scheme *sc, s7_pointer args)
{
	int32_t id = arg_id(args);

	if (id_ok(id))
		world_set_name(g_w, id, arg_str(sc, args, 1));
	return s7_unspecified(sc);
}

void scene_script_init(void)
{
	static int registered;
	s7_scheme *sc;

	if (registered)
		return;
	sc = script_s7();
	if (!sc)
		return;
	s7_define_function(sc, "scene-spawn", sp_scene_spawn, 0, 1, false,
			   "(scene-spawn [parent]) -> new entity id under parent");
	s7_define_function(sc, "scene-xform!", sp_scene_xform, 10, 0, false,
			   "(scene-xform! id px py pz rx ry rz sx sy sz)");
	s7_define_function(sc, "scene-mesh!", sp_scene_mesh, 2, 0, false,
			   "(scene-mesh! id path) bind mesh by catalog path");
	s7_define_function(sc, "scene-material!", sp_scene_material, 2, 0, false,
			   "(scene-material! id path) bind material by path");
	s7_define_function(sc, "scene-script!", sp_scene_script, 2, 0, false,
			   "(scene-script! id path) bind script by path");
	s7_define_function(sc, "scene-name!", sp_scene_name, 2, 0, false,
			   "(scene-name! id name) set entity name");
	registered = 1;
}

int32_t scene_script_build(struct world *w, const struct asset_api *asset,
			   const char *src)
{
	s7_scheme *sc;
	s7_pointer fn, res;
	int32_t    count;

	if (!w || !src)
		return -1;
	sc = script_s7();
	if (!sc)
		return -1;
	fn = s7_name_to_value(sc, "scene-build");
	if (!s7_is_procedure(fn))
		return -1;

	g_w     = w;
	g_asset = asset;
	res = s7_call(sc, fn, s7_list(sc, 1, s7_make_string(sc, src)));
	count = s7_is_integer(res) ? (int32_t)s7_integer(res) : -1;
	g_w     = NULL;
	g_asset = NULL;
	return count;
}
