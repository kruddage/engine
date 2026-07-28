/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * bridge — the editor boundary. See include/bridge.h for the shape of it and
 * why it is shaped that way; this file is the decoder, the dispatcher, the
 * fingerprints and the JSON writer, in that order.
 *
 * The world's columns are read here directly rather than through entity.c's
 * accessors, and that is deliberate: this module is a reader of the world's
 * data layout — it already walks mask[], parent[] and local[] — so reaching
 * for names[] the same way is one act rather than two conventions. It also
 * keeps the native test hermetic: a hand-built `struct world` and two fake
 * vtables drive the whole boundary with nothing else linked in.
 */
#include "bridge.h"

#include "bridge_params.h"

#include "world.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 * JSON writing
 *
 * A bounded appender over the reply buffer. Every write checks the cap and
 * sets `overflow` once; nothing truncates. The caller tests overflow at the
 * end and reports BRIDGE_ERR_OVERFLOW rather than handing out a JSON document
 * that stops mid-string.
 * ------------------------------------------------------------------ */

/*
 * Bounded copy. strncpy would do this, but it leaves the destination
 * unterminated on exact-fit input and gcc warns about the pattern that fixes
 * that — so the copy is written out, where the truncation is visible.
 */
static void copy_str(char *dst, size_t cap, const char *src)
{
	size_t n;

	if (cap == 0)
		return;
	n = src ? strlen(src) : 0;
	if (n > cap - 1)
		n = cap - 1;
	if (n)
		memcpy(dst, src, n);
	dst[n] = '\0';
}

struct jw {
	char	*buf;
	int32_t	cap;
	int32_t	len;
	int32_t	overflow;
};

static void jw_init(struct jw *w, char *buf, int32_t cap)
{
	w->buf      = buf;
	w->cap      = cap;
	w->len      = 0;
	w->overflow = 0;
	if (cap > 0)
		buf[0] = '\0';
}

static void jw_putn(struct jw *w, const char *s, int32_t n)
{
	if (w->overflow)
		return;
	/* One byte reserved for the terminator, always. */
	if (n < 0 || w->len + n >= w->cap) {
		w->overflow = 1;
		return;
	}
	memcpy(w->buf + w->len, s, (size_t)n);
	w->len += n;
	w->buf[w->len] = '\0';
}

static void jw_put(struct jw *w, const char *s)
{
	jw_putn(w, s, (int32_t)strlen(s));
}

/*
 * A JSON string literal. Escapes what RFC 8259 requires and nothing else —
 * bytes at or above 0x20 pass through, so a UTF-8 entity name survives intact
 * rather than being mangled into \u escapes by a serializer that does not
 * decode it.
 */
static void jw_str(struct jw *w, const char *s)
{
	char		esc[8];
	unsigned char	c;

	jw_put(w, "\"");
	for (; s && *s; s++) {
		c = (unsigned char)*s;
		switch (c) {
		case '"':  jw_put(w, "\\\""); break;
		case '\\': jw_put(w, "\\\\"); break;
		case '\n': jw_put(w, "\\n");  break;
		case '\r': jw_put(w, "\\r");  break;
		case '\t': jw_put(w, "\\t");  break;
		case '\b': jw_put(w, "\\b");  break;
		case '\f': jw_put(w, "\\f");  break;
		default:
			if (c < 0x20) {
				snprintf(esc, sizeof(esc), "\\u%04x", c);
				jw_put(w, esc);
			} else {
				jw_putn(w, (const char *)&c, 1);
			}
		}
	}
	jw_put(w, "\"");
}

/* A string, or the literal null — the two are different answers. */
static void jw_str_or_null(struct jw *w, const char *s)
{
	if (s)
		jw_str(w, s);
	else
		jw_put(w, "null");
}

static void jw_i32(struct jw *w, int32_t v)
{
	char tmp[16];

	snprintf(tmp, sizeof(tmp), "%d", (int)v);
	jw_put(w, tmp);
}

static void jw_u32(struct jw *w, uint32_t v)
{
	char tmp[16];

	snprintf(tmp, sizeof(tmp), "%u", (unsigned)v);
	jw_put(w, tmp);
}

static void jw_bool(struct jw *w, int32_t v)
{
	jw_put(w, v ? "true" : "false");
}

/*
 * %.9g round-trips a float32 exactly, which matters: a transform that goes out
 * to the inspector and comes straight back must not drift a bit per trip.
 *
 * JSON has no NaN or Infinity. A non-finite component is a bug upstream, and
 * emitting `null` keeps the document parseable so the editor can show that
 * something is wrong instead of failing to parse and showing nothing.
 */
static void jw_f32(struct jw *w, float v)
{
	char tmp[32];

	if (!isfinite((double)v)) {
		jw_put(w, "null");
		return;
	}
	snprintf(tmp, sizeof(tmp), "%.9g", (double)v);
	jw_put(w, tmp);
}

static void jw_key(struct jw *w, const char *name)
{
	jw_str(w, name);
	jw_put(w, ":");
}

/* ------------------------------------------------------------------ *
 * Tape reading
 *
 * Every scalar is read through memcpy: a record's payload begins wherever the
 * previous one ended, so nothing on the tape is guaranteed aligned, and a
 * direct cast would be undefined behaviour that happens to work on x86 and
 * wasm until it does not.
 *
 * Little-endian, matching both wasm and every host this tree builds on. It is
 * asserted rather than assumed — see the magic check in bridge_exchange: a
 * big-endian host reads the magic backwards and rejects the tape whole.
 * ------------------------------------------------------------------ */

struct tape {
	const uint8_t	*bytes;
	int32_t		len;
	int32_t		pos;
};

static int32_t tape_take(struct tape *t, void *out, int32_t n)
{
	if (t->pos + n > t->len)
		return 0;
	memcpy(out, t->bytes + t->pos, (size_t)n);
	t->pos += n;
	return 1;
}

static int32_t tape_u16(struct tape *t, uint16_t *out)
{
	return tape_take(t, out, (int32_t)sizeof(*out));
}

static int32_t tape_u32(struct tape *t, uint32_t *out)
{
	return tape_take(t, out, (int32_t)sizeof(*out));
}

static int32_t tape_f32(struct tape *t, float *out)
{
	return tape_take(t, out, 4);
}

static int32_t tape_i32(struct tape *t, int32_t *out)
{
	return tape_take(t, out, (int32_t)sizeof(*out));
}

/*
 * A transform is ten floats in the order the editor's encoder writes them
 * (BRIDGE_TAPE_TRANSFORM_BYTES). Read component-wise rather than as one
 * 40-byte memcpy into `struct transform`, so the wire format stays ours and
 * does not silently become a view of the C struct's padding.
 */
static int32_t tape_transform(struct tape *t, struct transform *out)
{
	int i;

	for (i = 0; i < 3; i++)
		if (!tape_take(t, &out->position[i], 4))
			return 0;
	for (i = 0; i < 4; i++)
		if (!tape_take(t, &out->rotation[i], 4))
			return 0;
	for (i = 0; i < 3; i++)
		if (!tape_take(t, &out->scale[i], 4))
			return 0;
	return 1;
}

/*
 * A length-prefixed string, copied out NUL-terminated. An over-long string is
 * truncated at the destination rather than rejected: a name is cosmetic, and
 * failing a whole batch because someone typed 200 characters into the outliner
 * would be a worse answer than storing the first 47.
 */
static int32_t tape_string(struct tape *t, char *out, int32_t cap)
{
	uint16_t n;
	int32_t  copy;

	if (!tape_u16(t, &n))
		return 0;
	if (t->pos + (int32_t)n > t->len)
		return 0;
	copy = (int32_t)n < cap - 1 ? (int32_t)n : cap - 1;
	memcpy(out, t->bytes + t->pos, (size_t)copy);
	out[copy] = '\0';
	t->pos += (int32_t)n;
	return 1;
}

/* ------------------------------------------------------------------ *
 * Fingerprints
 * ------------------------------------------------------------------ */

#define FNV64_OFFSET	0xcbf29ce484222325ull
#define FNV64_PRIME	0x00000100000001b3ull

static uint64_t fnv(uint64_t h, const void *data, size_t n)
{
	const uint8_t *p = data;
	size_t         i;

	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= FNV64_PRIME;
	}
	return h;
}

static uint64_t fnv_str(uint64_t h, const char *s)
{
	if (!s)
		return fnv(h, "\0nul", 4);
	return fnv(h, s, strlen(s));
}

/*
 * The scene's fingerprint covers everything a scene tree or an entity query
 * can report, plus the simulation's paused flag — the world's own state, which
 * is why it rides here rather than in a domain of its own.
 *
 * It walks 0..count, not 0..WORLD_MAX_ENTITIES: count is a high-water mark, so
 * this is O(entities that have ever existed) rather than O(4096). A world that
 * has never held more than a few hundred entities costs a few hundred
 * iterations a frame.
 */
static uint64_t fingerprint_scene(const struct bridge *b)
{
	const struct world *w;
	uint64_t            h = FNV64_OFFSET;
	uint32_t            i;
	int32_t             paused;

	if (!b->host.entity)
		return h;
	w = b->host.entity->get_world();
	if (!w)
		return h;

	paused = b->host.entity->get_paused ? b->host.entity->get_paused() : 0;
	h = fnv(h, &paused, sizeof(paused));
	h = fnv(h, &w->count, sizeof(w->count));

	for (i = 0; i < w->count && i < WORLD_MAX_ENTITIES; i++) {
		h = fnv(h, &w->alive[i], sizeof(w->alive[i]));
		if (!w->alive[i])
			continue;
		h = fnv(h, &w->mask[i], sizeof(w->mask[i]));
		h = fnv(h, &w->parent[i], sizeof(w->parent[i]));
		h = fnv(h, &w->local[i], sizeof(w->local[i]));
		h = fnv(h, &w->render_ref[i], sizeof(w->render_ref[i]));
		h = fnv(h, &w->material_ref[i], sizeof(w->material_ref[i]));
		h = fnv(h, &w->script_ref[i], sizeof(w->script_ref[i]));
		/*
		 * The name's bytes, not its offset. Renaming appends to the
		 * blob and abandons the old bytes, so the offset moves on every
		 * rename — but so would a rename to the same string, and that
		 * is not a change the editor should have to redraw for.
		 */
		if (w->name_off[i] != WORLD_NO_NAME &&
		    w->name_off[i] < WORLD_NAME_BYTES)
			h = fnv_str(h, w->names + w->name_off[i]);
		else
			h = fnv(h, "\0non", 4);
	}
	return h;
}

static uint64_t fingerprint_selection(const struct bridge *b)
{
	int32_t id = -1;

	if (b->host.entity && b->host.entity->get_selected)
		id = b->host.entity->get_selected();
	return fnv(FNV64_OFFSET, &id, sizeof(id));
}

/*
 * The viewport's fingerprint. Everything the viewport query reports, and
 * nothing else — in particular not the camera pose, for the reason the domain's
 * comment in bridge.h gives.
 */
static uint64_t fingerprint_viewport(const struct bridge *b)
{
	uint64_t          h = FNV64_OFFSET;
	struct gizmo_snap snap;
	float             w = 0.0f, hgt = 0.0f;
	int32_t           v;

	h = fnv(h, &b->drag_serial, sizeof(b->drag_serial));

	v = b->host.get_editor_mode ? b->host.get_editor_mode() : 0;
	h = fnv(h, &v, sizeof(v));

	if (b->host.viewport && b->host.viewport->get_size)
		b->host.viewport->get_size(&w, &hgt);
	h = fnv(h, &w, sizeof(w));
	h = fnv(h, &hgt, sizeof(hgt));

	if (!b->host.gizmo)
		return h;

	v = b->host.gizmo->get_mode ? b->host.gizmo->get_mode() : 0;
	h = fnv(h, &v, sizeof(v));
	v = b->host.gizmo->axis ? b->host.gizmo->axis() : GIZMO_AXIS_NONE;
	h = fnv(h, &v, sizeof(v));
	v = b->host.gizmo->grid_shown ? b->host.gizmo->grid_shown() : 0;
	h = fnv(h, &v, sizeof(v));
	if (b->host.gizmo->grid_spacing) {
		float spacing = b->host.gizmo->grid_spacing();

		h = fnv(h, &spacing, sizeof(spacing));
	}
	memset(&snap, 0, sizeof(snap));
	if (b->host.gizmo->get_snap)
		b->host.gizmo->get_snap(&snap);
	h = fnv(h, &snap, sizeof(snap));
	return h;
}

static uint64_t fingerprint_history(const struct bridge *b)
{
	const struct edit_api *e = b->host.edit;
	uint64_t               h = FNV64_OFFSET;
	int32_t                can;

	if (!e)
		return h;
	can = e->can_undo ? e->can_undo() : 0;
	h   = fnv(h, &can, sizeof(can));
	can = e->can_redo ? e->can_redo() : 0;
	h   = fnv(h, &can, sizeof(can));
	h   = fnv_str(h, e->undo_label ? e->undo_label() : NULL);
	h   = fnv_str(h, e->redo_label ? e->redo_label() : NULL);
	return h;
}

void bridge_refresh(struct bridge *b)
{
	uint64_t next[BRIDGE_DOMAIN_COUNT];
	int      i;

	next[BRIDGE_SCENE]     = fingerprint_scene(b);
	next[BRIDGE_SELECTION] = fingerprint_selection(b);
	next[BRIDGE_HISTORY]   = fingerprint_history(b);
	next[BRIDGE_VIEWPORT]  = fingerprint_viewport(b);

	for (i = 0; i < BRIDGE_DOMAIN_COUNT; i++) {
		/*
		 * The first refresh seeds every generation to 1 rather than
		 * comparing against a zeroed fingerprint that no state could
		 * legitimately produce. 0 is reserved for "the caller holds
		 * nothing", so a query carrying 0 always misses.
		 */
		if (!b->primed || b->fingerprint[i] != next[i])
			b->generation[i]++;
		b->fingerprint[i] = next[i];
	}
	b->primed = 1;
}

uint32_t bridge_generation(const struct bridge *b, enum bridge_domain d)
{
	if ((int)d < 0 || (int)d >= BRIDGE_DOMAIN_COUNT)
		return 0;
	return b->generation[d];
}

/* ------------------------------------------------------------------ *
 * Events
 * ------------------------------------------------------------------ */

void bridge_emit(struct bridge *b, const char *type, int32_t code,
		 const char *text)
{
	struct bridge_event *ev;

	if (b->event_count >= BRIDGE_MAX_EVENTS) {
		b->events_dropped++;
		return;
	}
	ev       = &b->events[b->event_count++];
	ev->type = type;
	ev->code = code;
	copy_str(ev->text, sizeof(ev->text), text);
}

/* ------------------------------------------------------------------ *
 * Command dispatch
 * ------------------------------------------------------------------ */

/*
 * Copy a tape-borne gesture label somewhere it will outlive the history entry
 * that holds it. See BRIDGE_LABEL_SLOTS.
 */
static const char *intern_label(struct bridge *b, const char *label)
{
	char *slot;

	if (!label || !*label)
		return "Edit";
	slot = b->labels[b->label_next];
	b->label_next = (b->label_next + 1) % BRIDGE_LABEL_SLOTS;
	copy_str(slot, BRIDGE_LABEL_BYTES, label);
	return slot;
}

static int32_t live_entity(const struct world *w, int32_t id)
{
	if (!w || id < 0 || (uint32_t)id >= w->count)
		return 0;
	return w->alive[id] != 0;
}

/*
 * Apply one command. Returns a bridge_status: BRIDGE_OK, or the reason the
 * tape is malformed. A command that is well-formed but cannot be satisfied —
 * selecting an entity that was destroyed a frame ago, say — is not an error.
 * It emits a notice and the batch continues, because the editor racing the
 * engine by one frame is normal operation, not a protocol violation.
 */
static int32_t apply_command(struct bridge *b, uint16_t op, struct tape *t)
{
	const struct entity_api *en = b->host.entity;
	const struct edit_api   *ed = b->host.edit;
	struct transform         xf;
	char                     text[BRIDGE_LABEL_BYTES];
	int32_t                  id, parent, flag;
	uint32_t                 mask, ref;

	switch (op) {
	case BRIDGE_OP_GESTURE_BEGIN:
		if (!tape_string(t, text, sizeof(text)))
			return BRIDGE_ERR_PAYLOAD;
		if (ed && ed->begin) {
			ed->begin(intern_label(b, text));
			b->gesture_depth++;
		}
		return BRIDGE_OK;

	case BRIDGE_OP_GESTURE_COMMIT:
		if (ed && ed->commit && b->gesture_depth > 0) {
			ed->commit();
			b->gesture_depth--;
		}
		return BRIDGE_OK;

	case BRIDGE_OP_GESTURE_ABORT:
		if (ed && ed->abort && b->gesture_depth > 0) {
			ed->abort();
			b->gesture_depth--;
		}
		return BRIDGE_OK;

	case BRIDGE_OP_UNDO:
		if (ed && ed->undo && !ed->undo())
			bridge_emit(b, "history.empty", 0, "nothing to undo");
		return BRIDGE_OK;

	case BRIDGE_OP_REDO:
		if (ed && ed->redo && !ed->redo())
			bridge_emit(b, "history.empty", 0, "nothing to redo");
		return BRIDGE_OK;

	case BRIDGE_OP_SELECT:
		if (!tape_i32(t, &id))
			return BRIDGE_ERR_PAYLOAD;
		if (en && en->set_selected)
			en->set_selected(id);
		return BRIDGE_OK;

	case BRIDGE_OP_ENTITY_CREATE:
		if (!tape_i32(t, &parent) || !tape_transform(t, &xf) ||
		    !tape_u32(t, &mask) || !tape_u32(t, &ref))
			return BRIDGE_ERR_PAYLOAD;
		if (en && en->create_entity &&
		    en->create_entity(parent, &xf, mask, ref) < 0)
			bridge_emit(b, "command.rejected",
				    BRIDGE_OP_ENTITY_CREATE,
				    "the world is full, or the parent is gone");
		return BRIDGE_OK;

	case BRIDGE_OP_ENTITY_DESTROY:
		if (!tape_i32(t, &id))
			return BRIDGE_ERR_PAYLOAD;
		if (en && en->destroy_entity)
			en->destroy_entity(id);
		return BRIDGE_OK;

	case BRIDGE_OP_ENTITY_TRANSFORM:
		if (!tape_i32(t, &id) || !tape_transform(t, &xf))
			return BRIDGE_ERR_PAYLOAD;
		if (en && en->set_transform)
			en->set_transform(id, &xf);
		return BRIDGE_OK;

	case BRIDGE_OP_ENTITY_NAME:
		if (!tape_i32(t, &id) || !tape_string(t, text, sizeof(text)))
			return BRIDGE_ERR_PAYLOAD;
		if (en && en->set_name)
			en->set_name(id, text);
		return BRIDGE_OK;

	case BRIDGE_OP_ENTITY_RENDER_REF:
		if (!tape_i32(t, &id) || !tape_u32(t, &ref))
			return BRIDGE_ERR_PAYLOAD;
		if (en && en->set_render_ref)
			en->set_render_ref(id, ref);
		return BRIDGE_OK;

	case BRIDGE_OP_ENTITY_MATERIAL_REF:
		if (!tape_i32(t, &id) || !tape_u32(t, &ref))
			return BRIDGE_ERR_PAYLOAD;
		if (en && en->set_material_ref)
			en->set_material_ref(id, ref);
		return BRIDGE_OK;

	case BRIDGE_OP_ENTITY_SCRIPT_REF:
		if (!tape_i32(t, &id) || !tape_u32(t, &ref))
			return BRIDGE_ERR_PAYLOAD;
		if (en && en->set_script_ref)
			en->set_script_ref(id, ref);
		return BRIDGE_OK;

	case BRIDGE_OP_ENTITY_PARAM: {
		int32_t slot, field, c;
		float   value[4];

		if (!tape_i32(t, &id) || !tape_i32(t, &slot) ||
		    !tape_i32(t, &field))
			return BRIDGE_ERR_PAYLOAD;
		for (c = 0; c < 4; c++)
			if (!tape_f32(t, &value[c]))
				return BRIDGE_ERR_PAYLOAD;
		/*
		 * A rejected write is a notice, not a failed batch. The editor
		 * racing the engine by one frame — a field index from a
		 * declaration that has since changed — is ordinary operation.
		 */
		if (!bridge_params_write(en, b->host.asset, id, slot, field,
					 value))
			bridge_emit(b, "command.rejected",
				    BRIDGE_OP_ENTITY_PARAM,
				    "no such entity, slot or parameter");
		return BRIDGE_OK;
	}

	case BRIDGE_OP_SET_PAUSED:
		if (!tape_i32(t, &flag))
			return BRIDGE_ERR_PAYLOAD;
		if (en && en->set_paused)
			en->set_paused(flag);
		return BRIDGE_OK;

	case BRIDGE_OP_SCENE_LOAD:
		if (!tape_string(t, b->scene_text, BRIDGE_SCENE_TEXT_BYTES))
			return BRIDGE_ERR_PAYLOAD;
		if (!en || !en->build_scene_scm || !en->clear_world) {
			bridge_emit(b, "command.rejected", BRIDGE_OP_SCENE_LOAD,
				    "this build cannot load a scene");
			return BRIDGE_OK;
		}
		/*
		 * Cleared, not merged. A load replaces the document; building
		 * into a populated world would layer one scene over another,
		 * which is what the launcher's clear_world exists to prevent.
		 */
		en->clear_world();
		if (en->build_scene_scm(b->scene_text) < 0)
			bridge_emit(b, "command.rejected", BRIDGE_OP_SCENE_LOAD,
				    "not a (scene ...) form, or the "
				    "interpreter is down");
		/*
		 * The history goes with the old document. Undo across a load
		 * would drive mementos captured against a world that no longer
		 * exists — every entity id in them now means something else.
		 */
		if (ed && ed->clear)
			ed->clear();
		return BRIDGE_OK;

	case BRIDGE_OP_VIEWPORT_SIZE: {
		float w, h;

		if (!tape_f32(t, &w) || !tape_f32(t, &h))
			return BRIDGE_ERR_PAYLOAD;
		if (b->host.viewport && b->host.viewport->set_size)
			b->host.viewport->set_size(w, h);
		return BRIDGE_OK;
	}

	case BRIDGE_OP_CAMERA_ORBIT: {
		float yaw, pitch;

		if (!tape_f32(t, &yaw) || !tape_f32(t, &pitch))
			return BRIDGE_ERR_PAYLOAD;
		if (b->host.camera && b->host.camera->orbit)
			b->host.camera->orbit(yaw, pitch);
		return BRIDGE_OK;
	}

	case BRIDGE_OP_CAMERA_PAN: {
		float dx, dy;

		if (!tape_f32(t, &dx) || !tape_f32(t, &dy))
			return BRIDGE_ERR_PAYLOAD;
		if (b->host.camera && b->host.camera->pan)
			b->host.camera->pan(dx, dy);
		return BRIDGE_OK;
	}

	case BRIDGE_OP_CAMERA_DOLLY: {
		float amount;

		if (!tape_f32(t, &amount))
			return BRIDGE_ERR_PAYLOAD;
		if (b->host.camera && b->host.camera->dolly)
			b->host.camera->dolly(amount);
		return BRIDGE_OK;
	}

	case BRIDGE_OP_CAMERA_FRAME: {
		float centre[3];
		float radius = 0.0f;

		if (!tape_i32(t, &id))
			return BRIDGE_ERR_PAYLOAD;
		/* -1 means "whatever is selected", so the shortcut does not
		 * have to know the id and cannot disagree about it. */
		if (id < 0 && en && en->get_selected)
			id = en->get_selected();
		if (id < 0) {
			bridge_emit(b, "viewport.nothing", BRIDGE_OP_CAMERA_FRAME,
				    "nothing is selected to frame");
			return BRIDGE_OK;
		}
		if (!b->host.viewport || !b->host.viewport->bounds ||
		    !b->host.viewport->bounds(id, centre, &radius)) {
			/*
			 * No mesh to measure. Not an error — an empty group
			 * node is a perfectly ordinary selection — but the
			 * reader pressed a key and deserves to know why
			 * nothing moved.
			 */
			bridge_emit(b, "viewport.nothing",
				    BRIDGE_OP_CAMERA_FRAME,
				    "this entity has nothing drawn to frame");
			return BRIDGE_OK;
		}
		if (b->host.camera && b->host.camera->frame)
			b->host.camera->frame(centre, radius);
		return BRIDGE_OK;
	}

	case BRIDGE_OP_CAMERA_RESET:
		if (b->host.camera && b->host.camera->reset_view)
			b->host.camera->reset_view();
		return BRIDGE_OK;

	case BRIDGE_OP_PICK: {
		float   sx, sy;
		int32_t hit = -1;

		if (!tape_f32(t, &sx) || !tape_f32(t, &sy))
			return BRIDGE_ERR_PAYLOAD;
		if (b->host.viewport && b->host.viewport->pick)
			hit = b->host.viewport->pick(sx, sy);
		/*
		 * Through set_selected like any other selection, so the pick
		 * and the outliner's click land in exactly the same place.
		 * A miss selects -1, which clears — clicking empty space
		 * deselecting is what every editor does and what the game
		 * path already did.
		 */
		if (en && en->set_selected)
			en->set_selected(hit);
		return BRIDGE_OK;
	}

	case BRIDGE_OP_EDITOR_MODE:
		if (!tape_i32(t, &flag))
			return BRIDGE_ERR_PAYLOAD;
		if (b->host.set_editor_mode)
			b->host.set_editor_mode(flag);
		/*
		 * Leaving the editor abandons any drag in progress. The
		 * handles are not drawn in game mode, so a drag that survived
		 * would be one the reader could neither see nor finish.
		 */
		if (!flag && b->host.gizmo && b->host.gizmo->end)
			b->host.gizmo->end();
		return BRIDGE_OK;

	case BRIDGE_OP_GIZMO_MODE:
		if (!tape_i32(t, &flag))
			return BRIDGE_ERR_PAYLOAD;
		if (b->host.gizmo && b->host.gizmo->set_mode)
			b->host.gizmo->set_mode(flag);
		return BRIDGE_OK;

	case BRIDGE_OP_GIZMO_SNAP: {
		float translate, rotate, scale;

		if (!tape_f32(t, &translate) || !tape_f32(t, &rotate) ||
		    !tape_f32(t, &scale))
			return BRIDGE_ERR_PAYLOAD;
		if (b->host.gizmo && b->host.gizmo->set_snap)
			b->host.gizmo->set_snap(translate, rotate, scale);
		return BRIDGE_OK;
	}

	case BRIDGE_OP_GIZMO_DRAG: {
		struct transform moved;
		float            sx, sy;
		int32_t          phase;

		if (!tape_i32(t, &phase) || !tape_f32(t, &sx) ||
		    !tape_f32(t, &sy))
			return BRIDGE_ERR_PAYLOAD;
		if (!b->host.gizmo)
			return BRIDGE_OK;

		switch (phase) {
		case BRIDGE_DRAG_BEGIN:
			/*
			 * Bumped whether or not a handle was grabbed. It is
			 * the editor's "answered" signal, not its "grabbed"
			 * one — `axis` is that, and the distinction is what
			 * lets a missed press be reported at all.
			 */
			b->drag_serial++;
			if (b->host.gizmo->begin)
				b->host.gizmo->begin(sx, sy);
			return BRIDGE_OK;
		case BRIDGE_DRAG_MOVE:
			if (!b->host.gizmo->move ||
			    !b->host.gizmo->move(sx, sy, &moved))
				return BRIDGE_OK;
			id = en && en->get_selected ? en->get_selected() : -1;
			/*
			 * The one line that makes a gizmo drag an ordinary
			 * edit: it goes through set_transform, so it lands on
			 * world/edit's ring inside the editor's gesture and
			 * coalesces exactly as an inspector slider does.
			 */
			if (id >= 0 && en->set_transform)
				en->set_transform(id, &moved);
			return BRIDGE_OK;
		case BRIDGE_DRAG_END:
			if (b->host.gizmo->end)
				b->host.gizmo->end();
			return BRIDGE_OK;
		default:
			bridge_emit(b, "command.rejected",
				    BRIDGE_OP_GIZMO_DRAG,
				    "not a drag phase this build knows");
			return BRIDGE_OK;
		}
	}

	case BRIDGE_OP_GRID: {
		float spacing;

		if (!tape_i32(t, &flag) || !tape_f32(t, &spacing))
			return BRIDGE_ERR_PAYLOAD;
		if (b->host.gizmo && b->host.gizmo->set_grid)
			b->host.gizmo->set_grid(flag, spacing);
		return BRIDGE_OK;
	}

	default:
		return BRIDGE_ERR_OPCODE;
	}
}

/* ------------------------------------------------------------------ *
 * Query answering
 * ------------------------------------------------------------------ */

static const char *entity_name(const struct world *w, uint32_t id)
{
	if (w->name_off[id] == WORLD_NO_NAME ||
	    w->name_off[id] >= WORLD_NAME_BYTES)
		return NULL;
	return w->names + w->name_off[id];
}

static void write_transform(struct jw *w, const struct transform *xf)
{
	int i;

	jw_put(w, "{");
	jw_key(w, "position");
	jw_put(w, "[");
	for (i = 0; i < 3; i++) {
		if (i)
			jw_put(w, ",");
		jw_f32(w, xf->position[i]);
	}
	jw_put(w, "],");
	jw_key(w, "rotation");
	jw_put(w, "[");
	for (i = 0; i < 4; i++) {
		if (i)
			jw_put(w, ",");
		jw_f32(w, xf->rotation[i]);
	}
	jw_put(w, "],");
	jw_key(w, "scale");
	jw_put(w, "[");
	for (i = 0; i < 3; i++) {
		if (i)
			jw_put(w, ",");
		jw_f32(w, xf->scale[i]);
	}
	jw_put(w, "]}");
}

/*
 * A tree node carries what an outliner draws — identity, parentage, name, and
 * which components are bound — and not the transform. #950 renders a list of
 * these for the whole world; sending ten floats per row it will never display
 * would multiply the tree query's size for nothing. The inspector asks for one
 * entity at a time and gets the transform there.
 */
static void write_tree_node(struct jw *w, const struct world *world,
			    uint32_t id)
{
	jw_put(w, "{");
	jw_key(w, "id");
	jw_u32(w, id);
	jw_put(w, ",");
	jw_key(w, "parent");
	jw_i32(w, world->parent[id]);
	jw_put(w, ",");
	jw_key(w, "name");
	jw_str_or_null(w, entity_name(world, id));
	jw_put(w, ",");
	jw_key(w, "mask");
	jw_u32(w, world->mask[id]);
	jw_put(w, ",");
	jw_key(w, "render");
	jw_u32(w, world->render_ref[id]);
	jw_put(w, ",");
	jw_key(w, "material");
	jw_u32(w, world->material_ref[id]);
	jw_put(w, ",");
	jw_key(w, "script");
	jw_u32(w, world->script_ref[id]);
	jw_put(w, "}");
}

static void write_tree(struct bridge *b, struct jw *w)
{
	const struct world *world;
	uint32_t            i;
	int32_t             first = 1;

	world = b->host.entity ? b->host.entity->get_world() : NULL;
	if (!world) {
		jw_put(w, "null");
		return;
	}
	jw_put(w, "{");
	jw_key(w, "paused");
	jw_bool(w, b->host.entity->get_paused ?
		       b->host.entity->get_paused() : 0);
	jw_put(w, ",");
	jw_key(w, "entities");
	jw_put(w, "[");
	for (i = 0; i < world->count && i < WORLD_MAX_ENTITIES; i++) {
		if (!world->alive[i])
			continue;
		if (!first)
			jw_put(w, ",");
		first = 0;
		write_tree_node(w, world, i);
	}
	jw_put(w, "]}");
}

static void write_entity(struct bridge *b, struct jw *w, int32_t id)
{
	const struct world *world;

	world = b->host.entity ? b->host.entity->get_world() : NULL;
	if (!live_entity(world, id)) {
		/*
		 * Not an error. The editor asking about an entity that has just
		 * been destroyed is one frame of ordinary lag, and `null` is
		 * the honest answer to "what is entity 7" when there is none.
		 */
		jw_put(w, "null");
		return;
	}
	jw_put(w, "{");
	jw_key(w, "id");
	jw_i32(w, id);
	jw_put(w, ",");
	jw_key(w, "parent");
	jw_i32(w, world->parent[id]);
	jw_put(w, ",");
	jw_key(w, "name");
	jw_str_or_null(w, entity_name(world, (uint32_t)id));
	jw_put(w, ",");
	jw_key(w, "mask");
	jw_u32(w, world->mask[id]);
	jw_put(w, ",");
	jw_key(w, "local");
	write_transform(w, &world->local[id]);
	jw_put(w, ",");
	jw_key(w, "world");
	write_transform(w, &world->world_xform[id]);
	jw_put(w, ",");
	jw_key(w, "render");
	jw_u32(w, world->render_ref[id]);
	jw_put(w, ",");
	jw_key(w, "material");
	jw_u32(w, world->material_ref[id]);
	jw_put(w, ",");
	jw_key(w, "script");
	jw_u32(w, world->script_ref[id]);
	jw_put(w, "}");
}

/*
 * The viewport's state (#949).
 *
 * Never null, unlike the history: the size and the editor-mode flag are the
 * bridge's own answers rather than a service's, so there is always something
 * true to say even on a build with no gizmo and no canvas. A panel that has to
 * render mode buttons wants "translate, nothing grabbed" rather than "null".
 */
static void write_viewport(struct bridge *b, struct jw *w)
{
	const struct gizmo_api *g = b->host.gizmo;
	struct gizmo_snap       snap;
	float                   vw = 0.0f, vh = 0.0f;

	memset(&snap, 0, sizeof(snap));
	if (g && g->get_snap)
		g->get_snap(&snap);
	if (b->host.viewport && b->host.viewport->get_size)
		b->host.viewport->get_size(&vw, &vh);

	jw_put(w, "{");
	jw_key(w, "width");
	jw_f32(w, vw);
	jw_put(w, ",");
	jw_key(w, "height");
	jw_f32(w, vh);
	jw_put(w, ",");
	jw_key(w, "editorMode");
	jw_bool(w, b->host.get_editor_mode ? b->host.get_editor_mode() : 0);
	jw_put(w, ",");
	jw_key(w, "mode");
	jw_i32(w, g && g->get_mode ? g->get_mode() : GIZMO_MODE_NONE);
	jw_put(w, ",");
	jw_key(w, "axis");
	jw_i32(w, g && g->axis ? g->axis() : GIZMO_AXIS_NONE);
	jw_put(w, ",");
	jw_key(w, "dragSerial");
	jw_u32(w, b->drag_serial);
	jw_put(w, ",");
	jw_key(w, "snap");
	jw_put(w, "{");
	jw_key(w, "translate");
	jw_f32(w, snap.translate);
	jw_put(w, ",");
	jw_key(w, "rotate");
	jw_f32(w, snap.rotate_degrees);
	jw_put(w, ",");
	jw_key(w, "scale");
	jw_f32(w, snap.scale);
	jw_put(w, "},");
	jw_key(w, "grid");
	jw_put(w, "{");
	jw_key(w, "shown");
	jw_bool(w, g && g->grid_shown ? g->grid_shown() : 0);
	jw_put(w, ",");
	jw_key(w, "spacing");
	jw_f32(w, g && g->grid_spacing ? g->grid_spacing() : 0.0f);
	jw_put(w, "}}");
}

static void write_history(struct bridge *b, struct jw *w)
{
	const struct edit_api *e = b->host.edit;

	if (!e) {
		jw_put(w, "null");
		return;
	}
	jw_put(w, "{");
	jw_key(w, "canUndo");
	jw_bool(w, e->can_undo ? e->can_undo() : 0);
	jw_put(w, ",");
	jw_key(w, "canRedo");
	jw_bool(w, e->can_redo ? e->can_redo() : 0);
	jw_put(w, ",");
	jw_key(w, "undoLabel");
	jw_str_or_null(w, e->undo_label ? e->undo_label() : NULL);
	jw_put(w, ",");
	jw_key(w, "redoLabel");
	jw_str_or_null(w, e->redo_label ? e->redo_label() : NULL);
	jw_put(w, "}");
}

/*
 * The world as (scene ...) text.
 *
 * `null` rather than an empty string when the engine cannot write one: "" is a
 * scene with no entities, which is a legitimate answer, and a client that
 * cannot tell the two apart would happily save an empty document over a full
 * one. So `null` is the failure, and it is self-describing.
 *
 * The reason goes out as an event, which arrives on the *next* exchange rather
 * than this one — write_envelope drains the event list before the query pass
 * runs, so anything a query emits has missed the reply it is being written
 * into. That is fine for a reason and would not be fine for the failure
 * itself, which is why the failure is the value and not the event.
 */
static void write_scene_text(struct bridge *b, struct jw *w)
{
	const struct entity_api *en = b->host.entity;
	int32_t                  n;

	if (!en || !en->save_scene_scm) {
		bridge_emit(b, "query.unavailable", BRIDGE_OP_QUERY_SCENE_TEXT,
			    "this build cannot save a scene");
		jw_put(w, "null");
		return;
	}
	n = en->save_scene_scm(b->scene_text, BRIDGE_SCENE_TEXT_BYTES);
	if (n < 0) {
		bridge_emit(b, "query.unavailable", BRIDGE_OP_QUERY_SCENE_TEXT,
			    "the scene did not fit, or could not be written");
		jw_put(w, "null");
		return;
	}
	jw_str(w, b->scene_text);
}

/*
 * An entity's parameter blocks — the declaration and the current value of
 * every field the inspector could show (#951).
 *
 * The declaration travels with the values on purpose. A client that cached
 * declarations separately would have to invalidate that cache when an asset
 * was rebound, and getting that wrong means a control laid out for the old
 * mesh writing into the new one's block.
 */
static void write_entity_params(struct bridge *b, struct jw *w, int32_t id)
{
	struct bridge_param_block blocks[BRIDGE_MAX_PARAM_BLOCKS];
	int32_t                   count, i, f, c;

	count = bridge_params_collect(b->host.entity, b->host.asset, id, blocks,
				      BRIDGE_MAX_PARAM_BLOCKS);
	if (count < 0) {
		/* Same answer write_entity gives for a gone entity, and for the
		 * same reason: `null` is honest, an empty list is a claim. */
		jw_put(w, "null");
		return;
	}

	jw_put(w, "{");
	jw_key(w, "id");
	jw_i32(w, id);
	jw_put(w, ",");
	jw_key(w, "blocks");
	jw_put(w, "[");
	for (i = 0; i < count; i++) {
		const struct bridge_param_block *bl = &blocks[i];

		if (i)
			jw_put(w, ",");
		jw_put(w, "{");
		jw_key(w, "slot");
		jw_str(w, bl->slot);
		jw_put(w, ",");
		jw_key(w, "asset");
		jw_u32(w, bl->asset);
		jw_put(w, ",");
		jw_key(w, "path");
		jw_str_or_null(w, bl->path);
		jw_put(w, ",");
		jw_key(w, "size");
		jw_u32(w, bl->total);
		jw_put(w, ",");
		jw_key(w, "overridden");
		jw_bool(w, bl->overridden);
		jw_put(w, ",");
		jw_key(w, "truncated");
		jw_bool(w, bl->truncated);
		jw_put(w, ",");
		jw_key(w, "fields");
		jw_put(w, "[");
		for (f = 0; f < bl->count; f++) {
			const struct shader_param *p = &bl->fields[f];

			if (f)
				jw_put(w, ",");
			jw_put(w, "{");
			jw_key(w, "name");
			jw_str(w, p->name);
			jw_put(w, ",");
			jw_key(w, "type");
			jw_str(w, p->type);
			jw_put(w, ",");
			jw_key(w, "components");
			jw_u32(w, p->components);
			jw_put(w, ",");
			/*
			 * "none", "color" or "range" — the authored (edit ...)
			 * hint, verbatim. This one string is what the whole
			 * derivation turns on, and the editor must never infer
			 * it from the name or the type.
			 */
			jw_key(w, "edit");
			jw_str(w, p->edit);
			jw_put(w, ",");
			jw_key(w, "min");
			jw_f32(w, p->edit_min);
			jw_put(w, ",");
			jw_key(w, "max");
			jw_f32(w, p->edit_max);
			jw_put(w, ",");
			jw_key(w, "value");
			jw_put(w, "[");
			for (c = 0; c < (int32_t)p->components && c < 4; c++) {
				if (c)
					jw_put(w, ",");
				jw_f32(w, bl->values[f][c]);
			}
			jw_put(w, "]}");
		}
		jw_put(w, "]}");
	}
	jw_put(w, "]}");
}

/*
 * One query result. The generation is always reported, even on a hit: it is
 * what the caller stores, and returning it unconditionally means the client
 * never has to reason about which branch it took.
 */
static int32_t answer_query(struct bridge *b, struct jw *w, uint16_t op,
			    struct tape *t, int32_t first)
{
	enum bridge_domain domain;
	const char        *kind;
	uint32_t           known, now;
	int32_t            id = -1;

	switch (op) {
	case BRIDGE_OP_QUERY_TREE:
		kind = "scene.tree";
		domain = BRIDGE_SCENE;
		break;
	case BRIDGE_OP_QUERY_ENTITY:
		kind = "entity";
		domain = BRIDGE_SCENE;
		if (!tape_i32(t, &id))
			return BRIDGE_ERR_PAYLOAD;
		break;
	case BRIDGE_OP_QUERY_SELECTION:
		kind = "selection";
		domain = BRIDGE_SELECTION;
		break;
	case BRIDGE_OP_QUERY_HISTORY:
		kind = "history";
		domain = BRIDGE_HISTORY;
		break;
	case BRIDGE_OP_QUERY_SCENE_TEXT:
		kind = "scene.text";
		domain = BRIDGE_SCENE;
		break;
	case BRIDGE_OP_QUERY_ENTITY_PARAMS:
		kind = "entity.params";
		domain = BRIDGE_SCENE;
		if (!tape_i32(t, &id))
			return BRIDGE_ERR_PAYLOAD;
		break;
	case BRIDGE_OP_QUERY_VIEWPORT:
		kind = "viewport";
		domain = BRIDGE_VIEWPORT;
		break;
	default:
		return BRIDGE_ERR_OPCODE;
	}

	if (!tape_u32(t, &known))
		return BRIDGE_ERR_PAYLOAD;
	now = b->generation[domain];

	if (!first)
		jw_put(w, ",");
	jw_put(w, "{");
	jw_key(w, "kind");
	jw_str(w, kind);
	jw_put(w, ",");
	if (op == BRIDGE_OP_QUERY_ENTITY ||
	    op == BRIDGE_OP_QUERY_ENTITY_PARAMS) {
		jw_key(w, "id");
		jw_i32(w, id);
		jw_put(w, ",");
	}
	jw_key(w, "generation");
	jw_u32(w, now);
	jw_put(w, ",");

	/*
	 * The ETag. 0 never matches, so a caller holding nothing always gets a
	 * value — which is what makes the first frame after a reload correct
	 * without a special case anywhere in the client.
	 */
	if (known != 0 && known == now) {
		jw_key(w, "fresh");
		jw_put(w, "true}");
		return BRIDGE_OK;
	}

	jw_key(w, "fresh");
	jw_put(w, "false,");
	jw_key(w, "value");
	switch (op) {
	case BRIDGE_OP_QUERY_TREE:
		write_tree(b, w);
		break;
	case BRIDGE_OP_QUERY_ENTITY:
		write_entity(b, w, id);
		break;
	case BRIDGE_OP_QUERY_SELECTION:
		jw_put(w, "{");
		jw_key(w, "id");
		jw_i32(w, b->host.entity && b->host.entity->get_selected ?
			      b->host.entity->get_selected() : -1);
		jw_put(w, "}");
		break;
	case BRIDGE_OP_QUERY_SCENE_TEXT:
		write_scene_text(b, w);
		break;
	case BRIDGE_OP_QUERY_ENTITY_PARAMS:
		write_entity_params(b, w, id);
		break;
	case BRIDGE_OP_QUERY_VIEWPORT:
		write_viewport(b, w);
		break;
	default:
		write_history(b, w);
		break;
	}
	jw_put(w, "}");
	return BRIDGE_OK;
}

/* ------------------------------------------------------------------ *
 * The exchange
 * ------------------------------------------------------------------ */

static const char *status_text(int32_t status)
{
	switch (status) {
	case BRIDGE_ERR_MAGIC:     return "magic";
	case BRIDGE_ERR_TRUNCATED: return "truncated";
	case BRIDGE_ERR_OPCODE:    return "opcode";
	case BRIDGE_ERR_PAYLOAD:   return "payload";
	case BRIDGE_ERR_OVERFLOW:  return "overflow";
	default:                   return "ok";
	}
}

static void write_events(struct bridge *b, struct jw *w)
{
	int32_t i;

	jw_key(w, "events");
	jw_put(w, "[");
	for (i = 0; i < b->event_count; i++) {
		if (i)
			jw_put(w, ",");
		jw_put(w, "{");
		jw_key(w, "type");
		jw_str(w, b->events[i].type ? b->events[i].type : "unknown");
		jw_put(w, ",");
		jw_key(w, "code");
		jw_i32(w, b->events[i].code);
		jw_put(w, ",");
		jw_key(w, "text");
		jw_str(w, b->events[i].text);
		jw_put(w, "}");
	}
	jw_put(w, "],");
	jw_key(w, "eventsDropped");
	jw_i32(w, b->events_dropped);
	b->events_sent = b->event_count;
}

static void write_generations(struct bridge *b, struct jw *w)
{
	jw_key(w, "generations");
	jw_put(w, "{");
	jw_key(w, "scene");
	jw_u32(w, b->generation[BRIDGE_SCENE]);
	jw_put(w, ",");
	jw_key(w, "selection");
	jw_u32(w, b->generation[BRIDGE_SELECTION]);
	jw_put(w, ",");
	jw_key(w, "history");
	jw_u32(w, b->generation[BRIDGE_HISTORY]);
	jw_put(w, ",");
	jw_key(w, "viewport");
	jw_u32(w, b->generation[BRIDGE_VIEWPORT]);
	jw_put(w, "}");
}

/*
 * How many payload bytes an opcode takes, when that is a constant.
 *
 * Returns 1 and fills *out for a fixed-size op, 0 for one carrying a string,
 * and -1 for an opcode this build does not know. Keeping the sizes here rather
 * than inferring them from what the decoder happened to read is what lets the
 * whole tape be checked before any of it is applied.
 */
static int32_t op_fixed_bytes(uint16_t op, int32_t *out)
{
	switch (op) {
	case BRIDGE_OP_GESTURE_COMMIT:
	case BRIDGE_OP_GESTURE_ABORT:
	case BRIDGE_OP_UNDO:
	case BRIDGE_OP_REDO:
		*out = 0;
		return 1;
	case BRIDGE_OP_CAMERA_RESET:
		*out = 0;
		return 1;
	case BRIDGE_OP_SELECT:
	case BRIDGE_OP_ENTITY_DESTROY:
	case BRIDGE_OP_SET_PAUSED:
	case BRIDGE_OP_CAMERA_DOLLY:
	case BRIDGE_OP_CAMERA_FRAME:
	case BRIDGE_OP_EDITOR_MODE:
	case BRIDGE_OP_GIZMO_MODE:
	case BRIDGE_OP_QUERY_TREE:
	case BRIDGE_OP_QUERY_SELECTION:
	case BRIDGE_OP_QUERY_HISTORY:
	case BRIDGE_OP_QUERY_SCENE_TEXT:
	case BRIDGE_OP_QUERY_VIEWPORT:
		*out = 4;
		return 1;
	case BRIDGE_OP_ENTITY_RENDER_REF:
	case BRIDGE_OP_ENTITY_MATERIAL_REF:
	case BRIDGE_OP_ENTITY_SCRIPT_REF:
	case BRIDGE_OP_QUERY_ENTITY:
	case BRIDGE_OP_QUERY_ENTITY_PARAMS:
	case BRIDGE_OP_VIEWPORT_SIZE:
	case BRIDGE_OP_CAMERA_ORBIT:
	case BRIDGE_OP_CAMERA_PAN:
	case BRIDGE_OP_PICK:
	case BRIDGE_OP_GRID:
		*out = 8;
		return 1;
	case BRIDGE_OP_GIZMO_SNAP:
		*out = 12;
		return 1;
	case BRIDGE_OP_GIZMO_DRAG:
		/* phase, then the pixel. */
		*out = 4 + 4 + 4;
		return 1;
	case BRIDGE_OP_ENTITY_TRANSFORM:
		*out = 4 + BRIDGE_TAPE_TRANSFORM_BYTES;
		return 1;
	case BRIDGE_OP_ENTITY_CREATE:
		*out = 4 + BRIDGE_TAPE_TRANSFORM_BYTES + 4 + 4;
		return 1;
	case BRIDGE_OP_ENTITY_PARAM:
		/* id, slot, field, then four components. */
		*out = 4 + 4 + 4 + 16;
		return 1;
	case BRIDGE_OP_GESTURE_BEGIN:
	case BRIDGE_OP_ENTITY_NAME:
	case BRIDGE_OP_SCENE_LOAD:
		return 0;
	default:
		return -1;
	}
}

/*
 * Check every record before applying any of them.
 *
 * The header promises a malformed tape is rejected whole, and this pass is
 * what makes that true rather than aspirational: without it, a bad opcode in
 * record nine would leave records one through eight applied, and the editor's
 * view and the engine's document would disagree with nothing able to say so.
 *
 * Cursors are local — the caller's tape is left untouched for the real walk.
 */
static int32_t validate_tape(const struct tape *src, uint32_t count)
{
	struct tape t = *src;
	uint16_t    op, len;
	uint32_t    i;
	int32_t     want, end, lead;

	for (i = 0; i < count; i++) {
		if (!tape_u16(&t, &op) || !tape_u16(&t, &len))
			return BRIDGE_ERR_TRUNCATED;
		end = t.pos + (int32_t)len;
		if (end > t.len)
			return BRIDGE_ERR_TRUNCATED;

		switch (op_fixed_bytes(op, &want)) {
		case -1:
			return BRIDGE_ERR_OPCODE;
		case 1:
			if ((int32_t)len != want)
				return BRIDGE_ERR_PAYLOAD;
			break;
		default:
			/*
			 * A string-bearing op: the id (when it has one) plus a
			 * u16 length prefix plus exactly that many bytes, and
			 * nothing after it.
			 */
			lead = op == BRIDGE_OP_ENTITY_NAME ? 4 : 0;
			if ((int32_t)len < lead + 2)
				return BRIDGE_ERR_PAYLOAD;
			t.pos += lead;
			if (!tape_u16(&t, &len))
				return BRIDGE_ERR_TRUNCATED;
			if (t.pos + (int32_t)len != end)
				return BRIDGE_ERR_PAYLOAD;
			break;
		}
		t.pos = end;
	}
	return BRIDGE_OK;
}

/*
 * Walk the tape twice. The first pass applies commands and skips queries; the
 * second answers queries and skips commands.
 *
 * That ordering is the whole point: a batch that moves an entity and then asks
 * for the scene tree must get the tree *after* the move, or the editor renders
 * a frame it already knows is stale and corrects itself on the next one. One
 * pass would make the answer depend on where in the batch the query happened
 * to sit, which is a race the client could not even see.
 */
static int32_t run_commands(struct bridge *b, struct tape *t, uint32_t count,
			    int32_t *applied)
{
	uint16_t op, len;
	uint32_t i;
	int32_t  end, status;

	for (i = 0; i < count; i++) {
		if (!tape_u16(t, &op) || !tape_u16(t, &len))
			return BRIDGE_ERR_TRUNCATED;
		end = t->pos + (int32_t)len;
		if (end > t->len)
			return BRIDGE_ERR_TRUNCATED;
		if (op < BRIDGE_OP_QUERY_TREE) {
			status = apply_command(b, op, t);
			if (status != BRIDGE_OK)
				return status;
			/*
			 * The declared length is the authority on where the
			 * next record starts, so a decoder that read fewer
			 * bytes than the encoder wrote still stays in sync.
			 * It is checked, not trusted: a mismatch means the two
			 * sides disagree about the format and every record
			 * after this one would be garbage.
			 */
			if (t->pos != end)
				return BRIDGE_ERR_PAYLOAD;
			(*applied)++;
		} else {
			t->pos = end;
		}
	}
	return BRIDGE_OK;
}

static int32_t run_queries(struct bridge *b, struct jw *w, struct tape *t,
			   uint32_t count)
{
	uint16_t op, len;
	uint32_t i;
	int32_t  end, status, first = 1;

	jw_key(w, "results");
	jw_put(w, "[");
	for (i = 0; i < count; i++) {
		if (!tape_u16(t, &op) || !tape_u16(t, &len))
			return BRIDGE_ERR_TRUNCATED;
		end = t->pos + (int32_t)len;
		if (end > t->len)
			return BRIDGE_ERR_TRUNCATED;
		if (op >= BRIDGE_OP_QUERY_TREE) {
			status = answer_query(b, w, op, t, first);
			if (status != BRIDGE_OK)
				return status;
			if (t->pos != end)
				return BRIDGE_ERR_PAYLOAD;
			first = 0;
		}
		t->pos = end;
	}
	jw_put(w, "]");
	return BRIDGE_OK;
}

/*
 * Everything in the reply that is not the results array. Written once on the
 * happy path, and again from scratch if a query fails after the array was
 * opened — rebuilding is how a half-emitted array never reaches the client.
 */
static void write_envelope(struct bridge *b, struct jw *w, int32_t applied,
			   int32_t status)
{
	jw_put(w, "{");
	jw_key(w, "protocol");
	jw_i32(w, BRIDGE_PROTOCOL);
	jw_put(w, ",");
	jw_key(w, "serial");
	jw_u32(w, b->serial);
	jw_put(w, ",");
	jw_key(w, "applied");
	jw_i32(w, applied);
	jw_put(w, ",");
	jw_key(w, "error");
	if (status == BRIDGE_OK)
		jw_put(w, "null");
	else
		jw_str(w, status_text(status));
	jw_put(w, ",");
	jw_key(w, "code");
	jw_i32(w, status);
	jw_put(w, ",");
	write_generations(b, w);
	jw_put(w, ",");
	write_events(b, w);
	jw_put(w, ",");
}

const char *bridge_exchange(struct bridge *b, const uint8_t *tape, int32_t len)
{
	struct jw   w;
	struct tape t;
	uint32_t    magic = 0, count = 0;
	int32_t     applied = 0, status = BRIDGE_OK;

	b->serial++;

	t.bytes = tape;
	t.len   = tape && len > 0 ? len : 0;
	t.pos   = 0;

	if (!tape_u32(&t, &magic) || !tape_u32(&t, &count))
		status = BRIDGE_ERR_TRUNCATED;
	else if (magic != BRIDGE_TAPE_MAGIC)
		status = BRIDGE_ERR_MAGIC;

	/* Whole-tape validation first — nothing is applied until all of it is
	 * known to be well-formed. */
	if (status == BRIDGE_OK)
		status = validate_tape(&t, count);
	if (status == BRIDGE_OK)
		status = run_commands(b, &t, count, &applied);

	/*
	 * Refresh after the commands and before the queries, always — including
	 * on a rejected tape, where the generations still have to reflect
	 * whatever the rest of the engine did this frame.
	 */
	bridge_refresh(b);

	jw_init(&w, b->reply, BRIDGE_REPLY_BYTES);
	write_envelope(b, &w, applied, status);

	if (status == BRIDGE_OK) {
		t.pos = 8;	/* rewind past the header for the query pass */
		status = run_queries(b, &w, &t, count);
		if (status != BRIDGE_OK) {
			jw_init(&w, b->reply, BRIDGE_REPLY_BYTES);
			write_envelope(b, &w, applied, status);
			jw_key(&w, "results");
			jw_put(&w, "[]");
		}
	} else {
		jw_key(&w, "results");
		jw_put(&w, "[]");
	}
	jw_put(&w, "}");

	/*
	 * An overflowed reply is not shipped. The client would get a JSON
	 * document that stops mid-token, and "unexpected end of input" is the
	 * least useful thing a boundary can say about itself.
	 */
	if (w.overflow) {
		snprintf(b->reply, BRIDGE_REPLY_BYTES,
			 "{\"protocol\":%d,\"serial\":%u,\"applied\":%d,"
			 "\"error\":\"overflow\",\"code\":%d,"
			 "\"generations\":{\"scene\":%u,\"selection\":%u,"
			 "\"history\":%u},\"events\":[],\"eventsDropped\":%d,"
			 "\"results\":[]}",
			 BRIDGE_PROTOCOL, (unsigned)b->serial, (int)applied,
			 (int)BRIDGE_ERR_OVERFLOW,
			 (unsigned)b->generation[BRIDGE_SCENE],
			 (unsigned)b->generation[BRIDGE_SELECTION],
			 (unsigned)b->generation[BRIDGE_HISTORY],
			 (int)b->events_dropped);
		b->reply_overflow = 1;
		b->reply_len      = (int32_t)strlen(b->reply);
		/* That fallback carries "events":[] — nothing was delivered. */
		b->events_sent    = 0;
	} else {
		b->reply_overflow = 0;
		b->reply_len      = w.len;
	}

	/*
	 * Drop exactly what went out, and keep the rest.
	 *
	 * Anything a query emitted was queued after the envelope had already
	 * been written, so it has not reached the client yet. Clearing the
	 * whole list here — which is what "events are per-exchange" used to
	 * mean — discarded those unread, and the only reason it never showed is
	 * that until scene.text no query emitted anything.
	 */
	if (b->events_sent > 0) {
		int32_t kept = b->event_count - b->events_sent;

		if (kept > 0)
			memmove(b->events, b->events + b->events_sent,
				(size_t)kept * sizeof(b->events[0]));
		b->event_count    = kept > 0 ? kept : 0;
		b->events_dropped = 0;
	}
	b->events_sent = 0;
	return b->reply;
}

void bridge_init(struct bridge *b, const struct bridge_host *host)
{
	memset(b, 0, sizeof(*b));
	if (host)
		b->host = *host;
}
