/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "world.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* a, b, out are xyzw quaternions; out must not alias a or b. */
static void quat_mul(const float a[4], const float b[4], float out[4])
{
	out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

/*
 * Rotate v by the unit quaternion q (xyzw):
 *   out = v + 2 q_w (q_xyz × v) + 2 q_xyz × (q_xyz × v)
 * out must not alias v.
 */
static void quat_rotate(const float q[4], const float v[3], float out[3])
{
	float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
	float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
	float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);

	out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
	out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
	out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

/*
 * Compose a parent world transform with a child's local transform. Assumes the
 * v1 scene contract: shear-free per-axis scale. Scale multiplies component-
 * wise, rotations multiply as quaternions, and the child origin is scaled,
 * rotated, then translated into the parent frame.
 */
static struct transform compose(const struct transform *p,
				const struct transform *c)
{
	struct transform out;
	float            scaled[3];
	float            rotated[3];

	out.scale[0] = p->scale[0] * c->scale[0];
	out.scale[1] = p->scale[1] * c->scale[1];
	out.scale[2] = p->scale[2] * c->scale[2];

	quat_mul(p->rotation, c->rotation, out.rotation);

	scaled[0] = p->scale[0] * c->position[0];
	scaled[1] = p->scale[1] * c->position[1];
	scaled[2] = p->scale[2] * c->position[2];
	quat_rotate(p->rotation, scaled, rotated);

	out.position[0] = p->position[0] + rotated[0];
	out.position[1] = p->position[1] + rotated[1];
	out.position[2] = p->position[2] + rotated[2];

	return out;
}

void world_reset(struct world *w)
{
	w->count = 0;
	w->name_bytes = 0;
}

int32_t world_create_entity(struct world *w, int32_t parent,
			    const struct transform *local, uint32_t mask)
{
	uint32_t e;

	if (w->count >= WORLD_MAX_ENTITIES)
		return -1;
	if (parent >= 0 && ((uint32_t)parent >= w->count || !w->alive[parent]))
		return -1;

	e = w->count++;
	w->mask[e]        = mask;
	w->parent[e]      = parent;
	w->alive[e]       = 1;
	w->local[e]       = *local;
	w->world_xform[e] = *local;
	w->name_off[e]    = WORLD_NO_NAME;
	w->render_ref[e]  = 0;
	return (int32_t)e;
}

void world_destroy_entity(struct world *w, int32_t e)
{
	uint32_t i;

	if (e < 0 || (uint32_t)e >= w->count)
		return;

	w->alive[e] = 0;
	/*
	 * Cascade to descendants. Topological order (parent < child) means one
	 * forward sweep suffices: every child is visited after its parent, so
	 * the parent's tombstone is already set when the child tests it.
	 */
	for (i = (uint32_t)e + 1; i < w->count; i++) {
		int32_t p = w->parent[i];

		if (p >= 0 && !w->alive[p])
			w->alive[i] = 0;
	}
}

const char *world_entity_name(const struct world *w, uint32_t e)
{
	if (e >= w->count || !(w->mask[e] & COMPONENT_NAME)
	    || w->name_off[e] == WORLD_NO_NAME)
		return NULL;
	return w->names + w->name_off[e];
}

int32_t world_ingest_scene(struct world *w, const struct scene *s)
{
	uint32_t i;

	if (s->count > WORLD_MAX_ENTITIES)
		return -1;

	world_reset(w);

	for (i = 0; i < s->count; i++) {
		const struct scene_entity *se = &s->entities[i];
		struct transform           t;

		memcpy(t.position, se->position, sizeof(t.position));
		memcpy(t.rotation, se->rotation, sizeof(t.rotation));
		memcpy(t.scale,    se->scale,    sizeof(t.scale));

		w->mask[i]        = se->mask;
		w->parent[i]      = se->parent;
		w->alive[i]       = 1;
		w->local[i]       = t;
		w->world_xform[i] = t;
		w->name_off[i]    = WORLD_NO_NAME;
		w->render_ref[i]  = (se->mask & COMPONENT_RENDER)
				    ? se->render_ref : 0;

		if ((se->mask & COMPONENT_NAME)
		    && se->name_off != SCENE_NO_NAME && s->names) {
			const char *src = s->names + se->name_off;
			size_t      len = strlen(src) + 1;

			if (w->name_bytes + len > WORLD_NAME_BYTES)
				return -1;
			memcpy(w->names + w->name_bytes, src, len);
			w->name_off[i] = w->name_bytes;
			w->name_bytes += (uint32_t)len;
		}
	}

	w->count = s->count;
	world_propagate_transforms(w, 0.0f);
	return 0;
}

void world_propagate_transforms(struct world *w, float dt)
{
	uint32_t i;

	(void)dt;
	for (i = 0; i < w->count; i++) {
		int32_t p;

		if (!w->alive[i])
			continue;
		p = w->parent[i];
		if (p < 0)
			w->world_xform[i] = w->local[i];
		else
			w->world_xform[i] = compose(&w->world_xform[p],
						    &w->local[i]);
	}
}

typedef void (*world_system_fn)(struct world *, float);

/* Ordered per-frame systems. Transform propagation must stay first. */
static const world_system_fn world_systems[] = {
	world_propagate_transforms,
};

void world_tick(struct world *w, float dt)
{
	size_t i;

	for (i = 0; i < sizeof(world_systems) / sizeof(world_systems[0]); i++)
		world_systems[i](w, dt);
}
