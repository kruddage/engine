/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "world.h"
#include "memory_api.h"

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
	w->selected = -1;
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
	w->material_ref[e] = 0;
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

	/* The cascade may have killed the selected entity; drop a stale ref. */
	if (w->selected >= 0 && !w->alive[w->selected])
		w->selected = -1;
}

void world_set_transform(struct world *w, int32_t e,
			 const struct transform *local)
{
	if (e < 0 || (uint32_t)e >= w->count || !w->alive[e])
		return;
	w->local[e] = *local;
}

int32_t world_set_name(struct world *w, int32_t e, const char *name)
{
	size_t len;

	if (e < 0 || (uint32_t)e >= w->count || !w->alive[e])
		return -1;

	/* No name clears the component rather than storing an empty string. */
	if (!name || name[0] == '\0') {
		w->mask[e]     &= ~(uint32_t)COMPONENT_NAME;
		w->name_off[e]  = WORLD_NO_NAME;
		return 0;
	}

	len = strlen(name) + 1;
	if (w->name_bytes + len > WORLD_NAME_BYTES)
		return -1;

	memcpy(w->names + w->name_bytes, name, len);
	w->name_off[e]  = w->name_bytes;
	w->name_bytes  += (uint32_t)len;
	w->mask[e]     |= COMPONENT_NAME;
	return 0;
}

void world_set_render_ref(struct world *w, int32_t e, uint32_t render_ref)
{
	if (e < 0 || (uint32_t)e >= w->count || !w->alive[e])
		return;

	/* A zero ref unbinds: clear the component rather than leave a dangling
	 * render_ref that resolves to no mesh (mirrors world_set_name). */
	if (render_ref == 0) {
		w->render_ref[e]  = 0;
		w->mask[e]       &= ~(uint32_t)COMPONENT_RENDER;
		return;
	}
	w->render_ref[e]  = render_ref;
	w->mask[e]       |= COMPONENT_RENDER;
}

void world_set_material_ref(struct world *w, int32_t e, uint32_t material_ref)
{
	if (e < 0 || (uint32_t)e >= w->count || !w->alive[e])
		return;

	/* A zero ref unbinds: clear the component rather than leave a dangling
	 * material_ref that resolves to no material (mirrors world_set_render_ref). */
	if (material_ref == 0) {
		w->material_ref[e]  = 0;
		w->mask[e]         &= ~(uint32_t)COMPONENT_MATERIAL;
		return;
	}
	w->material_ref[e]  = material_ref;
	w->mask[e]         |= COMPONENT_MATERIAL;
}

void world_set_selected(struct world *w, int32_t e)
{
	if (e < 0) {
		w->selected = -1;
		return;
	}
	if ((uint32_t)e < w->count && w->alive[e])
		w->selected = e;
}

int32_t world_get_selected(const struct world *w)
{
	return w->selected;
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

		memcpy(t.position, &se->position, sizeof(t.position));
		memcpy(t.rotation, &se->rotation, sizeof(t.rotation));
		memcpy(t.scale,    &se->scale,    sizeof(t.scale));

		w->mask[i]        = se->mask;
		w->parent[i]      = se->parent;
		w->alive[i]       = 1;
		w->local[i]       = t;
		w->world_xform[i] = t;
		w->name_off[i]    = WORLD_NO_NAME;
		w->render_ref[i]  = (se->mask & COMPONENT_RENDER)
				    ? se->render_ref : 0;
		w->material_ref[i] = (se->mask & COMPONENT_MATERIAL)
				    ? se->material_ref : 0;

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

struct scene *world_export_scene(const struct world *w,
				 const struct memory_api *mem)
{
	struct scene *s;
	int32_t      *remap;
	uint32_t      i, live = 0, name_bytes = 0, no = 0;

	if (!mem)
		return NULL;
	s = mem->alloc_zero(sizeof(*s));
	if (!s)
		return NULL;
	if (w->count == 0)
		return s;   /* empty world -> valid empty scene */

	remap = mem->alloc(w->count * sizeof(*remap));
	if (!remap) {
		mem->free(s);
		return NULL;
	}

	/* Pass 1: assign compacted indices to live entities and tally names. */
	for (i = 0; i < w->count; i++) {
		if (!w->alive[i]) {
			remap[i] = -1;
			continue;
		}
		remap[i] = (int32_t)live++;
		if ((w->mask[i] & COMPONENT_NAME)
		    && w->name_off[i] != WORLD_NO_NAME)
			name_bytes += (uint32_t)strlen(w->names
						       + w->name_off[i]) + 1u;
	}

	s->count = live;
	if (live == 0) {
		mem->free(remap);
		return s;   /* everything tombstoned */
	}

	s->entities = mem->alloc(live * sizeof(*s->entities));
	if (!s->entities)
		goto oom;
	if (name_bytes) {
		s->names = mem->alloc(name_bytes);
		if (!s->names)
			goto oom;
	}

	/* Pass 2: emit compacted records with remapped parents and packed names. */
	for (i = 0; i < w->count; i++) {
		struct scene_entity *se;

		if (!w->alive[i])
			continue;
		se         = &s->entities[remap[i]];
		se->mask   = w->mask[i];
		se->parent = (w->parent[i] < 0) ? -1 : remap[w->parent[i]];
		memcpy(&se->position, w->local[i].position, sizeof(se->position));
		memcpy(&se->rotation, w->local[i].rotation, sizeof(se->rotation));
		memcpy(&se->scale,    w->local[i].scale,    sizeof(se->scale));
		se->render_ref = (w->mask[i] & COMPONENT_RENDER)
				 ? w->render_ref[i] : 0u;
		se->material_ref = (w->mask[i] & COMPONENT_MATERIAL)
				 ? w->material_ref[i] : 0u;

		if ((w->mask[i] & COMPONENT_NAME)
		    && w->name_off[i] != WORLD_NO_NAME) {
			const char *nm  = w->names + w->name_off[i];
			uint32_t    len = (uint32_t)strlen(nm) + 1u;

			memcpy(s->names + no, nm, len);
			se->name_off = no;
			no += len;
		} else {
			se->name_off = SCENE_NO_NAME;
		}
	}

	mem->free(remap);
	return s;

oom:
	mem->free(remap);
	mem->free(s->entities);
	mem->free(s->names);
	mem->free(s);
	return NULL;
}

/*
 * Full-fidelity snapshot of the world's used prefix. Column arrays are sized
 * to `count` (the high-water mark) and the name blob to `name_bytes`, so a
 * snapshot costs O(scene) rather than O(WORLD_MAX_ENTITIES). world_xform is
 * derived and left out — restore re-propagates it.
 */
struct world_snapshot {
	uint32_t          count;
	uint32_t          name_bytes;
	int32_t           selected;
	uint8_t          *alive;
	uint32_t         *mask;
	int32_t          *parent;
	struct transform *local;
	uint32_t         *name_off;
	uint32_t         *render_ref;
	uint32_t         *material_ref;
	char             *names;
};

void world_snapshot_free(struct world_snapshot *s, const struct memory_api *mem)
{
	if (!s)
		return;
	mem->free(s->alive);
	mem->free(s->mask);
	mem->free(s->parent);
	mem->free(s->local);
	mem->free(s->name_off);
	mem->free(s->render_ref);
	mem->free(s->material_ref);
	mem->free(s->names);
	mem->free(s);
}

/* dup n bytes from src, or return NULL for a zero-length column (not a leak). */
static void *dup_bytes(const struct memory_api *mem, const void *src, size_t n)
{
	void *p;

	if (n == 0)
		return NULL;
	p = mem->alloc(n);
	if (p)
		memcpy(p, src, n);
	return p;
}

struct world_snapshot *world_snapshot_capture(const struct world *w,
					      const struct memory_api *mem)
{
	struct world_snapshot *s;
	uint32_t               n = w->count;

	if (!mem)
		return NULL;
	s = mem->alloc_zero(sizeof(*s));
	if (!s)
		return NULL;

	s->count      = n;
	s->name_bytes = w->name_bytes;
	s->selected   = w->selected;

	s->alive      = (uint8_t *)dup_bytes(mem, w->alive, n);
	s->mask       = (uint32_t *)dup_bytes(mem, w->mask, n * sizeof(*w->mask));
	s->parent     = (int32_t *)
			dup_bytes(mem, w->parent, n * sizeof(*w->parent));
	s->local      = (struct transform *)
			dup_bytes(mem, w->local, n * sizeof(*w->local));
	s->name_off   = (uint32_t *)
			dup_bytes(mem, w->name_off, n * sizeof(*w->name_off));
	s->render_ref = (uint32_t *)
			dup_bytes(mem, w->render_ref, n * sizeof(*w->render_ref));
	s->material_ref = (uint32_t *)
			dup_bytes(mem, w->material_ref,
				  n * sizeof(*w->material_ref));
	s->names      = (char *)dup_bytes(mem, w->names, w->name_bytes);

	/* A zero-length column dups to NULL; only a real short alloc is OOM. */
	if ((n && (!s->alive || !s->mask || !s->parent || !s->local ||
		   !s->name_off || !s->render_ref || !s->material_ref)) ||
	    (w->name_bytes && !s->names)) {
		world_snapshot_free(s, mem);
		return NULL;
	}
	return s;
}

void world_snapshot_restore(struct world *w, const struct world_snapshot *s)
{
	uint32_t n;

	if (!s)
		return;

	n = s->count;
	w->count      = n;
	w->name_bytes = s->name_bytes;
	w->selected   = s->selected;

	if (n) {
		memcpy(w->alive,      s->alive,      n);
		memcpy(w->mask,       s->mask,       n * sizeof(*w->mask));
		memcpy(w->parent,     s->parent,     n * sizeof(*w->parent));
		memcpy(w->local,      s->local,      n * sizeof(*w->local));
		memcpy(w->name_off,   s->name_off,   n * sizeof(*w->name_off));
		memcpy(w->render_ref, s->render_ref, n * sizeof(*w->render_ref));
		memcpy(w->material_ref, s->material_ref,
		       n * sizeof(*w->material_ref));
	}
	if (s->name_bytes)
		memcpy(w->names, s->names, s->name_bytes);

	world_propagate_transforms(w, 0.0f);
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
