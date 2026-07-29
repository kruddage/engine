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

/*
 * Scratch for the hierarchy walks below.
 *
 * File scope rather than automatic, and that is a wasm constraint rather than a
 * style choice: these are WORLD_MAX_ENTITIES wide, the module's stack is
 * measured in tens of kilobytes, and a 4096-entry int32 stack inside a function
 * the frame loop calls is how a deep scene becomes a silent stack overflow.
 * The engine is single-threaded and there is one world; every user below fills
 * a buffer before reading it, and none of them nest.
 */
static uint8_t g_mark[WORLD_MAX_ENTITIES];
static int32_t g_chain[WORLD_MAX_ENTITIES];

void world_reset(struct world *w)
{
	w->count = 0;
	w->name_bytes = 0;
	w->selected = -1;
	w->outline = -1;
}

int32_t world_is_ancestor(const struct world *w, int32_t a, int32_t e)
{
	uint32_t steps;

	if (a < 0 || e < 0 || (uint32_t)a >= w->count ||
	    (uint32_t)e >= w->count || a == e)
		return 0;

	/*
	 * Bounded by the entity count rather than trusted to terminate. A cycle
	 * is unrepresentable while world_set_parent is the only way to move a
	 * parent — and this is the function it asks, so it cannot also be the
	 * thing that guarantees the walk ends.
	 */
	for (steps = 0; steps < w->count; steps++) {
		e = w->parent[e];
		if (e < 0 || (uint32_t)e >= w->count)
			return 0;
		if (e == a)
			return 1;
	}
	return 0;
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
	/*
	 * world_reset leaves the columns alone, so a slot arrives carrying
	 * whatever the previous world left in it — including a selection flag.
	 * Clearing it here is the same discipline every other column follows
	 * below, and skipping it would spawn an entity into the selection.
	 */
	w->selected_set[e] = 0;
	w->flags[e]       = 0;
	w->local[e]       = *local;
	w->world_xform[e] = *local;
	w->name_off[e]    = WORLD_NO_NAME;
	w->render_ref[e]  = 0;
	w->material_ref[e] = 0;
	w->script_ref[e]  = 0;
	w->script_param_len[e] = 0;
	w->material_param_len[e] = 0;
	w->mesh_param_len[e] = 0;
	w->texture_param_len[e] = 0;
	return (int32_t)e;
}

void world_destroy_entity(struct world *w, int32_t e)
{
	uint32_t i;
	int32_t  spread;

	if (e < 0 || (uint32_t)e >= w->count)
		return;

	w->alive[e] = 0;
	/*
	 * Cascade to descendants. One forward sweep used to suffice, on the
	 * strength of every child sitting above its parent; #950's reparent
	 * ended that, so the sweep repeats until it tombstones nothing new.
	 * Each pass kills at least one more level of the subtree, so it runs as
	 * many times as that subtree is deep — and this is a cold path, reached
	 * when a reader presses Delete.
	 */
	do {
		spread = 0;
		for (i = 0; i < w->count; i++) {
			int32_t p = w->parent[i];

			if (!w->alive[i] || p < 0 || (uint32_t)p >= w->count)
				continue;
			if (!w->alive[p]) {
				w->alive[i] = 0;
				spread = 1;
			}
		}
	} while (spread);

	/* The cascade may have killed selected entities; drop the stale refs. */
	for (i = 0; i < w->count; i++) {
		if (!w->alive[i])
			w->selected_set[i] = 0;
	}
	if (w->selected >= 0 && !w->alive[w->selected])
		w->selected = -1;
	/*
	 * The primary went with its entity but the rest of the set may have
	 * survived, and a selection with members and no primary is a state no
	 * reader below expects. Promote the lowest survivor, exactly as
	 * world_select_remove does.
	 */
	if (w->selected < 0) {
		for (i = 0; i < w->count; i++) {
			if (w->alive[i] && w->selected_set[i]) {
				w->selected = (int32_t)i;
				break;
			}
		}
	}
	/* Likewise the game's outline target (a captured piece is destroyed). */
	if (w->outline >= 0 && !w->alive[w->outline])
		w->outline = -1;
}

/*
 * Recompute world_xform for `root` and every live descendant, composing off
 * each ancestor's CURRENT world_xform. Everything outside this subtree is
 * left untouched — unlike world_propagate_transforms, which re-derives the
 * whole world from `local` and so discards any script-authored world_xform
 * (e.g. an orbiting camera) that has drifted from its rest pose. That's fine
 * once a frame, right before scripts re-run and correct it, but a live edit
 * outside the tick — a gizmo drag while Paused holds scripts still — has no
 * following tick to fix it back up, so a full re-derive would snap every
 * other entity to its rest pose instead of leaving it frozen in place.
 */
static void propagate_subtree(struct world *w, int32_t root)
{
	uint32_t i;
	int32_t  p, spread;

	memset(g_mark, 0, w->count);

	p = w->parent[root];
	w->world_xform[root] = (p < 0 || (uint32_t)p >= w->count)
			       ? w->local[root]
			       : compose(&w->world_xform[p], &w->local[root]);
	g_mark[root] = 1;

	/*
	 * Sweep until nothing new is reached. A single forward pass was enough
	 * while every child sat above its parent; a reparented child can sit
	 * below one, so the pass repeats. Each one reaches a further level of
	 * the subtree, so the cost is O(depth · live) — and depth here is scene
	 * nesting, which is single digits in every scene this engine has.
	 */
	do {
		spread = 0;
		for (i = 0; i < w->count; i++) {
			if (!w->alive[i] || g_mark[i])
				continue;
			p = w->parent[i];
			if (p < 0 || (uint32_t)p >= w->count || !g_mark[p])
				continue;
			w->world_xform[i] = compose(&w->world_xform[p],
						    &w->local[i]);
			g_mark[i] = 1;
			spread = 1;
		}
	} while (spread);
}

void world_set_transform(struct world *w, int32_t e,
			 const struct transform *local)
{
	if (e < 0 || (uint32_t)e >= w->count || !w->alive[e])
		return;
	w->local[e] = *local;
	propagate_subtree(w, e);
}

int32_t world_set_parent(struct world *w, int32_t e, int32_t parent)
{
	if (e < 0 || (uint32_t)e >= w->count || !w->alive[e])
		return -1;
	if (parent >= 0 &&
	    ((uint32_t)parent >= w->count || !w->alive[parent]))
		return -1;
	/*
	 * The two cycles, and they are refused rather than clamped. Dropping a
	 * node onto itself is the easy one; dropping it onto its own descendant
	 * is the one a designer actually does, by dragging a folder into a
	 * child of that folder, and it is the drop that would build a subtree
	 * unreachable from any root — invisible in the outliner, undeletable,
	 * and still in the file.
	 */
	if (parent == e || world_is_ancestor(w, e, parent))
		return -1;

	w->parent[e] = parent;
	/*
	 * Recompose off the new ancestry, and only this subtree. The rest of
	 * the world keeps whatever world_xform it had, for the reason
	 * propagate_subtree's comment gives: an edit outside the tick has no
	 * following tick to undo a full re-derive's damage.
	 */
	propagate_subtree(w, e);
	return 0;
}

/*
 * Collect e and every live descendant into g_chain, parents before children,
 * and return how many. Uses g_mark, so it must not run inside a walk that is
 * using it — none of duplicate's callees are.
 */
static uint32_t collect_subtree(struct world *w, int32_t e)
{
	uint32_t n = 0, i;
	int32_t  spread;

	memset(g_mark, 0, w->count);
	g_mark[e] = 1;
	g_chain[n++] = e;

	/*
	 * The same fixed-point sweep the other walks use, and here the sweep
	 * order is load-bearing rather than incidental: a pass only takes an
	 * entity whose parent it has already taken, so g_chain comes out
	 * parent-before-child and the copy loop can map each parent to its copy
	 * without a second pass.
	 */
	do {
		spread = 0;
		for (i = 0; i < w->count; i++) {
			int32_t p = w->parent[i];

			if (!w->alive[i] || g_mark[i])
				continue;
			if (p < 0 || (uint32_t)p >= w->count || !g_mark[p])
				continue;
			g_mark[i] = 1;
			g_chain[n++] = (int32_t)i;
			spread = 1;
		}
	} while (spread);
	return n;
}

int32_t world_duplicate_entity(struct world *w, int32_t e)
{
	/*
	 * The old-id -> new-id map. Static for the reason the scratch above is:
	 * it is WORLD_MAX_ENTITIES wide and this runs on the wasm stack.
	 */
	static int32_t copy_of[WORLD_MAX_ENTITIES];
	uint32_t       n, i, names = 0;
	int32_t        root = -1;

	if (e < 0 || (uint32_t)e >= w->count || !w->alive[e])
		return -1;

	n = collect_subtree(w, e);
	if (w->count + n > WORLD_MAX_ENTITIES)
		return -1;

	/*
	 * Both budgets are checked before a single entity is appended. A
	 * duplicate that ran out of name blob halfway would leave the reader
	 * with a partial copy they did not ask for and have to unpick by hand,
	 * and it would do it after the point where returning -1 still means
	 * "nothing happened".
	 */
	for (i = 0; i < n; i++) {
		int32_t id = g_chain[i];

		if ((w->mask[id] & COMPONENT_NAME) &&
		    w->name_off[id] != WORLD_NO_NAME)
			names += (uint32_t)strlen(w->names + w->name_off[id]) + 1u;
	}
	if (w->name_bytes + names > WORLD_NAME_BYTES)
		return -1;

	for (i = 0; i < n; i++) {
		int32_t id = g_chain[i];
		int32_t p  = w->parent[id];
		int32_t to;

		/* The subtree root keeps e's parent; everything else follows
		 * its own parent's copy, which i's ordering guarantees exists. */
		if (i > 0 && p >= 0 && (uint32_t)p < w->count)
			p = copy_of[p];
		to = world_create_entity(w, p, &w->local[id],
					 w->mask[id] & ~(uint32_t)COMPONENT_NAME);
		if (to < 0)
			return root;   /* pre-checked; belt and braces */
		copy_of[id] = to;
		if (i == 0)
			root = to;

		w->render_ref[to]   = w->render_ref[id];
		w->material_ref[to] = w->material_ref[id];
		w->script_ref[to]   = w->script_ref[id];
		/* The name is set through world_set_name rather than copied,
		 * because the offset is into an append-only blob and a shared
		 * one would be freed out from under the copy by a rename. */
		if ((w->mask[id] & COMPONENT_NAME) &&
		    w->name_off[id] != WORLD_NO_NAME)
			world_set_name(w, to, w->names + w->name_off[id]);

		w->script_param_len[to] = w->script_param_len[id];
		memcpy(w->script_params[to], w->script_params[id],
		       WORLD_SCRIPT_PARAM_CAP);
		w->material_param_len[to] = w->material_param_len[id];
		memcpy(w->material_params[to], w->material_params[id],
		       WORLD_MATERIAL_PARAM_CAP);
		w->mesh_param_len[to] = w->mesh_param_len[id];
		memcpy(w->mesh_params[to], w->mesh_params[id],
		       WORLD_MESH_PARAM_CAP);
		w->texture_param_len[to] = w->texture_param_len[id];
		memcpy(w->texture_params[to], w->texture_params[id],
		       WORLD_TEXTURE_PARAM_CAP);
	}

	if (root >= 0)
		propagate_subtree(w, root);
	return root;
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

void world_set_script_ref(struct world *w, int32_t e, uint32_t script_ref)
{
	if (e < 0 || (uint32_t)e >= w->count || !w->alive[e])
		return;

	/* A zero ref unbinds: clear the component rather than leave a dangling
	 * script_ref that resolves to no script (mirrors world_set_render_ref). */
	if (script_ref == 0) {
		w->script_ref[e]  = 0;
		w->mask[e]       &= ~(uint32_t)COMPONENT_SCRIPT;
		return;
	}
	w->script_ref[e]  = script_ref;
	w->mask[e]       |= COMPONENT_SCRIPT;
}

void world_set_script_params(struct world *w, int32_t e,
			     const uint8_t *bytes, uint32_t len)
{
	if (e < 0 || (uint32_t)e >= w->count || !w->alive[e])
		return;
	if (!bytes || len == 0) {
		w->script_param_len[e] = 0;
		return;
	}
	if (len > WORLD_SCRIPT_PARAM_CAP)
		len = WORLD_SCRIPT_PARAM_CAP;
	memcpy(w->script_params[e], bytes, len);
	w->script_param_len[e] = len;
}

const uint8_t *world_script_params(const struct world *w, uint32_t e,
				   uint32_t *len)
{
	if (e >= w->count || w->script_param_len[e] == 0) {
		if (len)
			*len = 0;
		return NULL;
	}
	if (len)
		*len = w->script_param_len[e];
	return w->script_params[e];
}

void world_set_material_params(struct world *w, int32_t e,
			       const uint8_t *bytes, uint32_t len)
{
	if (e < 0 || (uint32_t)e >= w->count || !w->alive[e])
		return;
	if (!bytes || len == 0) {
		w->material_param_len[e] = 0;
		return;
	}
	if (len > WORLD_MATERIAL_PARAM_CAP)
		len = WORLD_MATERIAL_PARAM_CAP;
	memcpy(w->material_params[e], bytes, len);
	w->material_param_len[e] = len;
}

const uint8_t *world_material_params(const struct world *w, uint32_t e,
				     uint32_t *len)
{
	if (e >= w->count || w->material_param_len[e] == 0) {
		if (len)
			*len = 0;
		return NULL;
	}
	if (len)
		*len = w->material_param_len[e];
	return w->material_params[e];
}

void world_set_mesh_params(struct world *w, int32_t e,
			   const uint8_t *bytes, uint32_t len)
{
	if (e < 0 || (uint32_t)e >= w->count || !w->alive[e])
		return;
	if (!bytes || len == 0) {
		w->mesh_param_len[e] = 0;
		return;
	}
	if (len > WORLD_MESH_PARAM_CAP)
		len = WORLD_MESH_PARAM_CAP;
	memcpy(w->mesh_params[e], bytes, len);
	w->mesh_param_len[e] = len;
}

const uint8_t *world_mesh_params(const struct world *w, uint32_t e,
				 uint32_t *len)
{
	if (e >= w->count || w->mesh_param_len[e] == 0) {
		if (len)
			*len = 0;
		return NULL;
	}
	if (len)
		*len = w->mesh_param_len[e];
	return w->mesh_params[e];
}

void world_set_texture_params(struct world *w, int32_t e,
			      const uint8_t *bytes, uint32_t len)
{
	if (e < 0 || (uint32_t)e >= w->count || !w->alive[e])
		return;
	if (!bytes || len == 0) {
		w->texture_param_len[e] = 0;
		return;
	}
	if (len > WORLD_TEXTURE_PARAM_CAP)
		len = WORLD_TEXTURE_PARAM_CAP;
	memcpy(w->texture_params[e], bytes, len);
	w->texture_param_len[e] = len;
}

const uint8_t *world_texture_params(const struct world *w, uint32_t e,
				    uint32_t *len)
{
	if (e >= w->count || w->texture_param_len[e] == 0) {
		if (len)
			*len = 0;
		return NULL;
	}
	if (len)
		*len = w->texture_param_len[e];
	return w->texture_params[e];
}

void world_select_clear(struct world *w)
{
	if (w->count)
		memset(w->selected_set, 0, w->count);
	w->selected = -1;
}

void world_set_selected(struct world *w, int32_t e)
{
	if (e < 0) {
		world_select_clear(w);
		return;
	}
	/*
	 * An id that is not live leaves the selection alone rather than
	 * clearing it — the contract this function has always had, and the one
	 * that keeps a stale id arriving from a script or a stale editor bundle
	 * from wiping what the reader had picked.
	 */
	if ((uint32_t)e >= w->count || !w->alive[e])
		return;

	world_select_clear(w);
	w->selected_set[e] = 1;
	w->selected = e;
}

void world_select_add(struct world *w, int32_t e)
{
	if (e < 0 || (uint32_t)e >= w->count || !w->alive[e])
		return;
	w->selected_set[e] = 1;
	w->selected = e;
}

void world_select_remove(struct world *w, int32_t e)
{
	uint32_t i;

	if (e < 0 || (uint32_t)e >= w->count)
		return;
	w->selected_set[e] = 0;
	if (w->selected != e)
		return;

	w->selected = -1;
	for (i = 0; i < w->count; i++) {
		if (w->alive[i] && w->selected_set[i]) {
			w->selected = (int32_t)i;
			return;
		}
	}
}

int32_t world_is_selected(const struct world *w, int32_t e)
{
	if (e < 0 || (uint32_t)e >= w->count || !w->alive[e])
		return 0;
	return w->selected_set[e] ? 1 : 0;
}

uint32_t world_selection(const struct world *w, int32_t *out, uint32_t cap)
{
	uint32_t i, n = 0;

	for (i = 0; i < w->count; i++) {
		if (!w->alive[i] || !w->selected_set[i])
			continue;
		if (out && n < cap)
			out[n] = (int32_t)i;
		n++;
	}
	return n;
}

void world_set_entity_flags(struct world *w, int32_t e, uint32_t flags)
{
	if (e < 0 || (uint32_t)e >= w->count || !w->alive[e])
		return;
	w->flags[e] = (uint8_t)(flags & (WORLD_ENTITY_HIDDEN |
					 WORLD_ENTITY_LOCKED));
}

uint32_t world_entity_flags(const struct world *w, int32_t e)
{
	if (e < 0 || (uint32_t)e >= w->count || !w->alive[e])
		return 0;
	return w->flags[e];
}

int32_t world_get_selected(const struct world *w)
{
	return w->selected;
}

void world_set_outline(struct world *w, int32_t e)
{
	if (e < 0) {
		w->outline = -1;
		return;
	}
	if ((uint32_t)e < w->count && w->alive[e])
		w->outline = e;
}

int32_t world_get_outline(const struct world *w)
{
	return w->outline;
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
		/* world_reset leaves the columns alone and this writes them
		 * directly rather than through world_create_entity, so the
		 * selection flags have to be cleared here too. */
		w->selected_set[i] = 0;
		w->flags[i]       = 0;
		w->local[i]       = t;
		w->world_xform[i] = t;
		w->name_off[i]    = WORLD_NO_NAME;
		w->render_ref[i]  = (se->mask & COMPONENT_RENDER)
				    ? se->render_ref : 0;
		w->material_ref[i] = (se->mask & COMPONENT_MATERIAL)
				    ? se->material_ref : 0;
		w->script_ref[i]  = (se->mask & COMPONENT_SCRIPT)
				    ? se->script_ref : 0;
		/* Per-entity script-, material-, and mesh-param overrides are not
		 * carried by the .scene transfer format yet (its codec is
		 * unwritten); ingest starts each entity with no override. */
		w->script_param_len[i] = 0;
		w->material_param_len[i] = 0;
		w->mesh_param_len[i] = 0;
		w->texture_param_len[i] = 0;

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
	int32_t      *remap = NULL, *order = NULL;
	uint32_t      i, live = 0, placed = 0, name_bytes = 0, no = 0;
	int32_t       progress;

	if (!mem)
		return NULL;
	s = mem->alloc_zero(sizeof(*s));
	if (!s)
		return NULL;
	if (w->count == 0)
		return s;   /* empty world -> valid empty scene */

	remap = mem->alloc(w->count * sizeof(*remap));
	order = mem->alloc(w->count * sizeof(*order));
	if (!remap || !order) {
		mem->free(remap);
		mem->free(order);
		mem->free(s);
		return NULL;
	}

	/* Pass 1: count the live entities and tally the name blob. */
	for (i = 0; i < w->count; i++) {
		remap[i] = -1;
		if (!w->alive[i])
			continue;
		live++;
		if ((w->mask[i] & COMPONENT_NAME)
		    && w->name_off[i] != WORLD_NO_NAME)
			name_bytes += (uint32_t)strlen(w->names
						       + w->name_off[i]) + 1u;
	}

	if (live == 0) {
		mem->free(remap);
		mem->free(order);
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

	/*
	 * Pass 2: assign compacted indices parent-before-child.
	 *
	 * This used to be id order, on the strength of the world's columns
	 * being topological; #950's reparent ended that, and struct scene's
	 * ordering (scene.h) is a file-format guarantee rather than an
	 * implementation detail — a decoder resolves parents in one forward
	 * pass and would read a forward reference as a parent that does not
	 * exist yet. So the ordering is re-established here, at the one place
	 * it is actually promised.
	 *
	 * Ascending id within each sweep keeps siblings in id order, so a world
	 * exports identically every time and a diff of two saves is readable.
	 */
	do {
		progress = 0;
		for (i = 0; i < w->count; i++) {
			int32_t p = w->parent[i];

			if (!w->alive[i] || remap[i] >= 0)
				continue;
			/* A live entity never has a tombstoned parent (destroy
			 * cascades), so a parent that is gone means the column
			 * is corrupt; treat it as a root rather than orphaning
			 * the entity out of the file. */
			if (p >= 0 && (uint32_t)p < w->count && w->alive[p] &&
			    remap[p] < 0)
				continue;
			order[placed] = (int32_t)i;
			remap[i]      = (int32_t)placed++;
			progress      = 1;
		}
	} while (progress && placed < live);

	/*
	 * placed < live only if some entity's ancestry never bottoms out, which
	 * world_set_parent refuses to build. Reporting the count actually
	 * written keeps the scene self-consistent if it ever happens anyway.
	 */
	s->count = placed;

	/* Pass 3: emit in that order, with remapped parents and packed names. */
	for (i = 0; i < placed; i++) {
		int32_t              id = order[i];
		int32_t              p  = w->parent[id];
		struct scene_entity *se = &s->entities[i];

		se->mask   = w->mask[id];
		se->parent = (p < 0 || (uint32_t)p >= w->count) ? -1 : remap[p];
		memcpy(&se->position, w->local[id].position, sizeof(se->position));
		memcpy(&se->rotation, w->local[id].rotation, sizeof(se->rotation));
		memcpy(&se->scale,    w->local[id].scale,    sizeof(se->scale));
		se->render_ref = (w->mask[id] & COMPONENT_RENDER)
				 ? w->render_ref[id] : 0u;
		se->material_ref = (w->mask[id] & COMPONENT_MATERIAL)
				 ? w->material_ref[id] : 0u;
		se->script_ref = (w->mask[id] & COMPONENT_SCRIPT)
				 ? w->script_ref[id] : 0u;

		if ((w->mask[id] & COMPONENT_NAME)
		    && w->name_off[id] != WORLD_NO_NAME) {
			const char *nm  = w->names + w->name_off[id];
			uint32_t    len = (uint32_t)strlen(nm) + 1u;

			memcpy(s->names + no, nm, len);
			se->name_off = no;
			no += len;
		} else {
			se->name_off = SCENE_NO_NAME;
		}
	}

	mem->free(remap);
	mem->free(order);
	return s;

oom:
	mem->free(remap);
	mem->free(order);
	mem->free(s->entities);
	mem->free(s->names);
	mem->free(s);
	return NULL;
}

/*
 * Full-fidelity snapshot of the world's used prefix. Column arrays are sized
 * to `count` (the high-water mark) and the name blob to `name_bytes`, so a
 * snapshot costs O(scene) rather than O(WORLD_MAX_ENTITIES). world_xform is
 * captured and restored verbatim rather than re-derived from `local`: a
 * script can leave it well away from the rest pose (an orbiting camera), and
 * a restore is a checkpoint jump, not a tick — it must bring that back
 * exactly, not snap every entity to its authored rest pose.
 */
struct world_snapshot {
	uint32_t          count;
	uint32_t          name_bytes;
	int32_t           selected;
	uint8_t          *alive;
	uint8_t          *selected_set;
	uint32_t         *mask;
	int32_t          *parent;
	struct transform *local;
	struct transform *world_xform;
	uint32_t         *name_off;
	uint32_t         *render_ref;
	uint32_t         *material_ref;
	uint32_t         *script_ref;
	uint32_t         *script_param_len;
	uint8_t          *script_params;   /* count * WORLD_SCRIPT_PARAM_CAP */
	uint32_t         *material_param_len;
	uint8_t          *material_params; /* count * WORLD_MATERIAL_PARAM_CAP */
	uint32_t         *mesh_param_len;
	uint8_t          *mesh_params;     /* count * WORLD_MESH_PARAM_CAP */
	uint32_t         *texture_param_len;
	uint8_t          *texture_params;  /* count * WORLD_TEXTURE_PARAM_CAP */
	char             *names;
};

void world_snapshot_free(struct world_snapshot *s, const struct memory_api *mem)
{
	if (!s)
		return;
	mem->free(s->alive);
	mem->free(s->selected_set);
	mem->free(s->mask);
	mem->free(s->parent);
	mem->free(s->local);
	mem->free(s->world_xform);
	mem->free(s->name_off);
	mem->free(s->render_ref);
	mem->free(s->material_ref);
	mem->free(s->script_ref);
	mem->free(s->script_param_len);
	mem->free(s->script_params);
	mem->free(s->material_param_len);
	mem->free(s->material_params);
	mem->free(s->mesh_param_len);
	mem->free(s->mesh_params);
	mem->free(s->texture_param_len);
	mem->free(s->texture_params);
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
	/* The whole selection, not just the primary. Undoing a delete has to
	 * bring back what was selected when it happened, and with a set that is
	 * a column like any other rather than one int32. */
	s->selected_set = (uint8_t *)dup_bytes(mem, w->selected_set, n);
	s->mask       = (uint32_t *)dup_bytes(mem, w->mask, n * sizeof(*w->mask));
	s->parent     = (int32_t *)
			dup_bytes(mem, w->parent, n * sizeof(*w->parent));
	s->local      = (struct transform *)
			dup_bytes(mem, w->local, n * sizeof(*w->local));
	s->world_xform = (struct transform *)
			dup_bytes(mem, w->world_xform, n * sizeof(*w->world_xform));
	s->name_off   = (uint32_t *)
			dup_bytes(mem, w->name_off, n * sizeof(*w->name_off));
	s->render_ref = (uint32_t *)
			dup_bytes(mem, w->render_ref, n * sizeof(*w->render_ref));
	s->material_ref = (uint32_t *)
			dup_bytes(mem, w->material_ref,
				  n * sizeof(*w->material_ref));
	s->script_ref = (uint32_t *)
			dup_bytes(mem, w->script_ref,
				  n * sizeof(*w->script_ref));
	s->script_param_len = (uint32_t *)
			dup_bytes(mem, w->script_param_len,
				  n * sizeof(*w->script_param_len));
	s->script_params = (uint8_t *)
			dup_bytes(mem, w->script_params,
				  (size_t)n * WORLD_SCRIPT_PARAM_CAP);
	s->material_param_len = (uint32_t *)
			dup_bytes(mem, w->material_param_len,
				  n * sizeof(*w->material_param_len));
	s->material_params = (uint8_t *)
			dup_bytes(mem, w->material_params,
				  (size_t)n * WORLD_MATERIAL_PARAM_CAP);
	s->mesh_param_len = (uint32_t *)
			dup_bytes(mem, w->mesh_param_len,
				  n * sizeof(*w->mesh_param_len));
	s->mesh_params = (uint8_t *)
			dup_bytes(mem, w->mesh_params,
				  (size_t)n * WORLD_MESH_PARAM_CAP);
	s->texture_param_len = (uint32_t *)
			dup_bytes(mem, w->texture_param_len,
				  n * sizeof(*w->texture_param_len));
	s->texture_params = (uint8_t *)
			dup_bytes(mem, w->texture_params,
				  (size_t)n * WORLD_TEXTURE_PARAM_CAP);
	s->names      = (char *)dup_bytes(mem, w->names, w->name_bytes);

	/* A zero-length column dups to NULL; only a real short alloc is OOM. */
	if ((n && (!s->alive || !s->selected_set || !s->mask || !s->parent || !s->local ||
		   !s->world_xform ||
		   !s->name_off || !s->render_ref || !s->material_ref ||
		   !s->script_ref || !s->script_param_len ||
		   !s->script_params || !s->material_param_len ||
		   !s->material_params || !s->mesh_param_len ||
		   !s->mesh_params || !s->texture_param_len ||
		   !s->texture_params)) ||
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
		memcpy(w->selected_set, s->selected_set, n);
		memcpy(w->mask,       s->mask,       n * sizeof(*w->mask));
		memcpy(w->parent,     s->parent,     n * sizeof(*w->parent));
		memcpy(w->local,      s->local,      n * sizeof(*w->local));
		memcpy(w->world_xform, s->world_xform,
		       n * sizeof(*w->world_xform));
		memcpy(w->name_off,   s->name_off,   n * sizeof(*w->name_off));
		memcpy(w->render_ref, s->render_ref, n * sizeof(*w->render_ref));
		memcpy(w->material_ref, s->material_ref,
		       n * sizeof(*w->material_ref));
		memcpy(w->script_ref, s->script_ref,
		       n * sizeof(*w->script_ref));
		memcpy(w->script_param_len, s->script_param_len,
		       n * sizeof(*w->script_param_len));
		memcpy(w->script_params, s->script_params,
		       (size_t)n * WORLD_SCRIPT_PARAM_CAP);
		memcpy(w->material_param_len, s->material_param_len,
		       n * sizeof(*w->material_param_len));
		memcpy(w->material_params, s->material_params,
		       (size_t)n * WORLD_MATERIAL_PARAM_CAP);
		memcpy(w->mesh_param_len, s->mesh_param_len,
		       n * sizeof(*w->mesh_param_len));
		memcpy(w->mesh_params, s->mesh_params,
		       (size_t)n * WORLD_MESH_PARAM_CAP);
		memcpy(w->texture_param_len, s->texture_param_len,
		       n * sizeof(*w->texture_param_len));
		memcpy(w->texture_params, s->texture_params,
		       (size_t)n * WORLD_TEXTURE_PARAM_CAP);
	}
	if (s->name_bytes)
		memcpy(w->names, s->names, s->name_bytes);
}

/*
 * Resolve `e` and every unresolved ancestor of it, marking each in `done`.
 *
 * Climbs to the first ancestor already resolved (or to a root), then composes
 * back down the chain it collected. The climb stops at anything resolved, so
 * across a whole-world pass every entity is pushed exactly once and the total
 * work is linear in the live count — the single forward sweep this replaces was
 * linear too, and this buys the same bound without the ordering it needed.
 *
 * The chain is capped at the entity count. A cycle cannot be built through
 * world_set_parent, and the cap is what makes that a refusal rather than a
 * hang if a column is ever corrupted some other way: an entity whose parent
 * did not get resolved is treated as a root, so the frame renders wrong rather
 * than not at all.
 */
static void resolve_chain(struct world *w, int32_t e, uint8_t *done)
{
	uint32_t depth = 0;

	while (e >= 0 && (uint32_t)e < w->count && w->alive[e] && !done[e] &&
	       depth < w->count) {
		g_chain[depth++] = e;
		e = w->parent[e];
	}

	while (depth > 0) {
		int32_t id = g_chain[--depth];
		int32_t p  = w->parent[id];

		if (p >= 0 && (uint32_t)p < w->count && done[p])
			w->world_xform[id] = compose(&w->world_xform[p],
						     &w->local[id]);
		else
			w->world_xform[id] = w->local[id];
		done[id] = 1;
	}
}

void world_propagate_transforms(struct world *w, float dt)
{
	uint32_t i;

	(void)dt;
	memset(g_mark, 0, w->count);
	for (i = 0; i < w->count; i++) {
		if (!w->alive[i] || g_mark[i])
			continue;
		resolve_chain(w, (int32_t)i, g_mark);
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
