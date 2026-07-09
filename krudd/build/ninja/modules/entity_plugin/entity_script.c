/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * entity_script — host side of the entity scripting layer.
 *
 * Registers the entity-* primitives the (script ...) clauses call, and drives
 * one tick per bound entity by handing (id, source, clock) to the image's
 * entity-script-tick. See entity_script.h for the read-rest / write-animated
 * contract that keeps stateless scripts drift-free.
 */
#include "entity_script.h"

#include "world.h"
#include "asset_api.h"
#include "script.h"

#include "s7.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define ENTITY_SCRIPT_PI 3.14159265358979323846

/*
 * The world the primitives act on, valid only for the span of one
 * entity_script_tick (set before the per-entity s7_call loop, cleared after).
 * A primitive can only run synchronously inside that loop, so no primitive ever
 * observes a stale pointer.
 */
static struct world *g_w;

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

/* True when id names a live entity in the bound world. */
static int id_ok(int32_t id)
{
	return g_w && id >= 0 && (uint32_t)id < g_w->count && g_w->alive[id];
}

static s7_pointer vec3_list(s7_scheme *sc, const float v[3])
{
	return s7_list(sc, 3, s7_make_real(sc, v[0]),
		       s7_make_real(sc, v[1]), s7_make_real(sc, v[2]));
}

/* (entity-base-position id) -> (x y z): the authored rest position. */
static s7_pointer sp_entity_base_position(s7_scheme *sc, s7_pointer args)
{
	int32_t id = arg_id(args);

	if (!id_ok(id))
		return s7_nil(sc);
	return vec3_list(sc, g_w->local[id].position);
}

/* (entity-base-scale id) -> (sx sy sz): the authored rest scale. */
static s7_pointer sp_entity_base_scale(s7_scheme *sc, s7_pointer args)
{
	int32_t id = arg_id(args);

	if (!id_ok(id))
		return s7_nil(sc);
	return vec3_list(sc, g_w->local[id].scale);
}

/* (entity-set-position! id x y z): set this frame's animated render position. */
static s7_pointer sp_entity_set_position(s7_scheme *sc, s7_pointer args)
{
	int32_t id = arg_id(args);

	if (id_ok(id)) {
		struct transform *x = &g_w->world_xform[id];

		x->position[0] = (float)arg_real(sc, args, 1);
		x->position[1] = (float)arg_real(sc, args, 2);
		x->position[2] = (float)arg_real(sc, args, 3);
	}
	return s7_unspecified(sc);
}

/* (entity-set-scale! id sx sy sz): set this frame's animated render scale. */
static s7_pointer sp_entity_set_scale(s7_scheme *sc, s7_pointer args)
{
	int32_t id = arg_id(args);

	if (id_ok(id)) {
		struct transform *x = &g_w->world_xform[id];

		x->scale[0] = (float)arg_real(sc, args, 1);
		x->scale[1] = (float)arg_real(sc, args, 2);
		x->scale[2] = (float)arg_real(sc, args, 3);
	}
	return s7_unspecified(sc);
}

/*
 * (entity-set-euler! id deg-x deg-y deg-z): set this frame's animated render
 * rotation from intrinsic X-Y-Z Euler angles in degrees, stored as the xyzw
 * quaternion the world transform expects.
 */
static s7_pointer sp_entity_set_euler(s7_scheme *sc, s7_pointer args)
{
	int32_t id = arg_id(args);

	if (id_ok(id)) {
		double  rx = arg_real(sc, args, 1) * (ENTITY_SCRIPT_PI / 180.0) * 0.5;
		double  ry = arg_real(sc, args, 2) * (ENTITY_SCRIPT_PI / 180.0) * 0.5;
		double  rz = arg_real(sc, args, 3) * (ENTITY_SCRIPT_PI / 180.0) * 0.5;
		double  cx = cos(rx), sx = sin(rx);
		double  cy = cos(ry), sy = sin(ry);
		double  cz = cos(rz), sz = sin(rz);
		float  *q  = g_w->world_xform[id].rotation;

		q[0] = (float)(sx * cy * cz - cx * sy * sz);
		q[1] = (float)(cx * sy * cz + sx * cy * sz);
		q[2] = (float)(cx * cy * sz - sx * sy * cz);
		q[3] = (float)(cx * cy * cz + sx * sy * sz);
	}
	return s7_unspecified(sc);
}

void entity_script_init(void)
{
	static int registered;
	s7_scheme *sc;

	if (registered)
		return;
	sc = script_s7();
	if (!sc)
		return;
	s7_define_function(sc, "entity-base-position", sp_entity_base_position,
			   1, 0, false, "(entity-base-position id) authored position");
	s7_define_function(sc, "entity-base-scale", sp_entity_base_scale,
			   1, 0, false, "(entity-base-scale id) authored scale");
	s7_define_function(sc, "entity-set-position!", sp_entity_set_position,
			   4, 0, false, "(entity-set-position! id x y z)");
	s7_define_function(sc, "entity-set-scale!", sp_entity_set_scale,
			   4, 0, false, "(entity-set-scale! id sx sy sz)");
	s7_define_function(sc, "entity-set-euler!", sp_entity_set_euler,
			   4, 0, false, "(entity-set-euler! id dx dy dz) degrees");
	registered = 1;
}

void entity_script_tick(struct world *w, const struct asset_api *asset, float t)
{
	s7_scheme *sc;
	s7_pointer fn;
	uint32_t   i;

	if (!w || !asset)
		return;
	sc = script_s7();
	if (!sc)
		return;
	fn = s7_name_to_value(sc, "entity-script-tick");
	if (!s7_is_procedure(fn))
		return;

	g_w = w;
	for (i = 0; i < w->count; i++) {
		const char *src;

		if (!w->alive[i] || !(w->mask[i] & COMPONENT_SCRIPT)
		    || !w->script_ref[i])
			continue;
		src = (const char *)asset->get_data(w->script_ref[i], NULL);
		if (!src)
			continue;
		s7_call(sc, fn, s7_list(sc, 3, s7_make_integer(sc, (s7_int)i),
					s7_make_string(sc, src),
					s7_make_real(sc, (double)t)));
	}
	g_w = NULL;
}
