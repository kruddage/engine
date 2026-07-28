/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The world, written back out as a (scene ...) form. See scene_save.h for why
 * this is the writer half of an existing path rather than a format of its own.
 */
#include "scene_save.h"

#include "scene.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int32_t symbol_ok(const char *name);

/*
 * A bounded text writer that latches its overflow.
 *
 * Every put() past the end is a no-op and sets `full`, so the caller checks
 * once at the end instead of after each of the dozen writes an entity takes.
 * The same shape as bridge.c's `jw`, and for the same reason: a partial
 * document must never be mistaken for a complete one.
 */
struct sw {
	char	*buf;
	int32_t	 cap;
	int32_t	 len;
	int32_t	 full;
};

static void sw_put(struct sw *w, const char *s)
{
	int32_t n;

	if (w->full || !s)
		return;
	n = (int32_t)strlen(s);
	if (w->len + n >= w->cap) {
		w->full = 1;
		return;
	}
	memcpy(w->buf + w->len, s, (size_t)n);
	w->len += n;
	w->buf[w->len] = '\0';
}

/*
 * A float, in a form that reads back as the same float.
 *
 * "%.9g" is the shortest precision that round-trips every IEEE-754 binary32,
 * so a position saved and loaded is bit-identical rather than merely close.
 * The value is read back as a double and narrowed on the way into the world,
 * which is exactly the conversion that produced it.
 *
 * Non-finite values are written as 0 rather than as "nan" or "inf": s7 would
 * not read those back as numbers, and a scene that fails to parse is a worse
 * outcome than an entity that saved a corrupt transform as the origin.
 */
static void sw_f32(struct sw *w, float v)
{
	char text[32];

	if (!isfinite(v)) {
		sw_put(w, "0");
		return;
	}
	snprintf(text, sizeof(text), "%.9g", (double)v);
	sw_put(w, text);
}

static void sw_vec3(struct sw *w, const char *clause, const float v[3])
{
	sw_put(w, clause);
	sw_f32(w, v[0]);
	sw_put(w, " ");
	sw_f32(w, v[1]);
	sw_put(w, " ");
	sw_f32(w, v[2]);
	sw_put(w, ")");
}

/*
 * A Scheme string literal.
 *
 * Only `"` and `\` are escaped, because those are the only two characters s7's
 * reader treats specially inside a string. A control character is dropped
 * rather than escaped: entity names come from a text field, a stray newline in
 * one is a paste accident rather than intent, and writing it raw would break
 * the form's line structure for a human reading the file.
 */
static void sw_string(struct sw *w, const char *s)
{
	char one[2] = { 0, 0 };

	sw_put(w, "\"");
	for (; s && *s; s++) {
		if (*s == '"' || *s == '\\') {
			one[0] = '\\';
			sw_put(w, one);
		} else if ((unsigned char)*s < 0x20) {
			continue;
		}
		one[0] = *s;
		sw_put(w, one);
	}
	sw_put(w, "\"");
}

static void sw_indent(struct sw *w, int32_t depth)
{
	int32_t i;

	sw_put(w, "\n");
	for (i = 0; i < depth; i++)
		sw_put(w, "  ");
}

/* The catalog path a ref binds to, or NULL when it cannot be resolved. */
static const char *ref_path(const struct asset_api *assets, uint32_t ref)
{
	struct asset_info info;

	if (!assets || !assets->find || ref == 0u)
		return NULL;
	memset(&info, 0, sizeof(info));
	if (assets->find(ref, &info) != 0)
		return NULL;
	return info.path && info.path[0] ? info.path : NULL;
}

/*
 * One binding clause, when the ref is set and the catalog still knows it.
 *
 * A ref the catalog cannot resolve is skipped rather than written as a number:
 * the form binds by path, ids are not stable across a rebuild of the catalog,
 * and a `(mesh 7)` clause would be a scene that loads differently tomorrow.
 * The entity keeps every clause that did resolve.
 */
static void write_binding(struct sw *w, const struct asset_api *assets,
			  const char *clause, uint32_t mask, uint32_t bit,
			  uint32_t ref, int32_t depth)
{
	const char *path;

	if (!(mask & bit))
		return;
	path = ref_path(assets, ref);
	if (!path)
		return;
	sw_indent(w, depth);
	sw_put(w, clause);
	sw_string(w, path);
	sw_put(w, ")");
}

static int32_t write_entity(struct sw *w, const struct world *world,
			    const struct asset_api *assets, int32_t id,
			    int32_t depth);

/*
 * Every live child of `parent`, in id order.
 *
 * The scan is the whole world for each parent — see the cost note in the
 * header. Ordering falls out of the world's topological layout for free, and
 * it matters: a save that reorders siblings produces a different file from one
 * run to the next, and a diff nobody can read.
 */
static int32_t write_children(struct sw *w, const struct world *world,
			      const struct asset_api *assets, int32_t parent,
			      int32_t depth)
{
	int32_t id, first = 1;

	for (id = 0; id < (int32_t)world->count; id++) {
		if (!world->alive[id] || world->parent[id] != parent)
			continue;
		if (first) {
			sw_indent(w, depth);
			sw_put(w, "(children");
			first = 0;
		}
		if (!write_entity(w, world, assets, id, depth + 1))
			return 0;
	}
	if (!first)
		sw_put(w, ")");
	return 1;
}

static int32_t write_entity(struct sw *w, const struct world *world,
			    const struct asset_api *assets, int32_t id,
			    int32_t depth)
{
	const struct transform *t = &world->local[id];
	const char             *name;

	if (depth > SCENE_SAVE_MAX_DEPTH)
		return 0;

	sw_indent(w, depth);
	sw_put(w, "(entity");

	name = world_entity_name(world, (uint32_t)id);
	if (name && name[0]) {
		sw_indent(w, depth + 1);
		sw_put(w, "(name ");
		sw_string(w, name);
		sw_put(w, ")");
	}

	write_binding(w, assets, "(mesh ", world->mask[id], COMPONENT_RENDER,
		      world->render_ref[id], depth + 1);
	write_binding(w, assets, "(material ", world->mask[id],
		      COMPONENT_MATERIAL, world->material_ref[id], depth + 1);
	write_binding(w, assets, "(script ", world->mask[id], COMPONENT_SCRIPT,
		      world->script_ref[id], depth + 1);

	/*
	 * All three transform clauses, always, even when they hold the default.
	 * The reader defaults a missing clause to identity, so omitting them
	 * would be equivalent — but a save is also what a human reads to see
	 * where something is, and "no position clause" is a worse answer to
	 * that than "(at 0 0 0)".
	 */
	sw_indent(w, depth + 1);
	sw_vec3(w, "(at ", t->position);
	sw_put(w, " (quat ");
	sw_f32(w, t->rotation[0]);
	sw_put(w, " ");
	sw_f32(w, t->rotation[1]);
	sw_put(w, " ");
	sw_f32(w, t->rotation[2]);
	sw_put(w, " ");
	sw_f32(w, t->rotation[3]);
	sw_put(w, ")");
	sw_put(w, " ");
	sw_vec3(w, "(scale ", t->scale);

	if (!write_children(w, world, assets, id, depth + 1))
		return 0;

	sw_put(w, ")");
	return 1;
}

int32_t scene_save_write(const struct world *w, const struct asset_api *assets,
			 const char *name, char *out, int32_t cap)
{
	struct sw writer;
	int32_t   id;

	if (!w || !out || cap <= 1)
		return -1;

	writer.buf  = out;
	writer.cap  = cap;
	writer.len  = 0;
	writer.full = 0;
	out[0]      = '\0';

	sw_put(&writer, "(scene ");
	/*
	 * The scene's name is a symbol in the form, not a string, so it cannot
	 * carry spaces or parens. Anything unusable is replaced wholesale
	 * rather than sanitized character by character — a half-mangled symbol
	 * is a name nobody asked for, and "scene" is at least honest.
	 */
	sw_put(&writer, symbol_ok(name) ? name : "scene");

	for (id = 0; id < (int32_t)w->count; id++) {
		if (!w->alive[id] || w->parent[id] != WORLD_NO_PARENT)
			continue;
		if (!write_entity(&writer, w, assets, id, 1))
			return -1;
	}
	sw_put(&writer, ")\n");

	return writer.full ? -1 : writer.len;
}

/*
 * Whether `name` can be written as a bare symbol.
 *
 * Deliberately strict — letters, digits, dash and underscore — rather than
 * "everything s7's reader happens to accept". The permissive set includes
 * characters that read back as something other than a symbol depending on
 * position, and a scene name is not worth a table of reader edge cases.
 */
static int32_t symbol_ok(const char *name)
{
	const char *c;

	if (!name || !name[0])
		return 0;
	for (c = name; *c; c++) {
		if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
		    (*c >= '0' && *c <= '9') || *c == '-' || *c == '_')
			continue;
		return 0;
	}
	/* A leading digit reads as a number, not a symbol. */
	return !(name[0] >= '0' && name[0] <= '9');
}
