/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * An entity's editable parameters, resolved. See bridge_params.h for why the
 * resolution happens here and not across the boundary.
 */
#include "bridge_params.h"

#include "world.h"

#include <string.h>

/* The three slots, and where each one's declaration comes from. */
enum slot_kind {
	SLOT_MESH = 0,
	SLOT_MATERIAL,
	SLOT_SCRIPT
};

static const char *const SLOT_NAME[] = { "mesh", "material", "script" };

/* The interned slot name back to its kind. Names come from SLOT_NAME. */
static enum slot_kind slot_of(const char *name)
{
	if (name == SLOT_NAME[SLOT_MESH])
		return SLOT_MESH;
	if (name == SLOT_NAME[SLOT_MATERIAL])
		return SLOT_MATERIAL;
	return SLOT_SCRIPT;
}

static int32_t live_entity(const struct world *w, int32_t id)
{
	if (!w || id < 0 || (uint32_t)id >= w->count)
		return 0;
	return w->alive[id] != 0;
}

/*
 * An asset's bytes as a NUL-terminated source string, or NULL.
 *
 * The catalog hands back a length and a pointer into its own storage, and the
 * script introspection entry points want a C string. Rather than copy every
 * asset into a scratch buffer, this checks that the bytes already end in a NUL
 * — which every script asset does, because they are authored text and the
 * seeder stores them with their terminator. An asset that does not is refused
 * rather than read past.
 */
static const char *asset_source(const struct asset_api *asset, uint32_t ref)
{
	const void *bytes;
	uint32_t    size = 0;

	if (!asset || !asset->get_data || ref == 0u)
		return NULL;
	bytes = asset->get_data(ref, &size);
	if (!bytes || size == 0)
		return NULL;
	if (((const char *)bytes)[size - 1] != '\0')
		return NULL;
	return (const char *)bytes;
}

static const char *asset_path(const struct asset_api *asset, uint32_t ref)
{
	struct asset_info info;

	if (!asset || !asset->find || ref == 0u)
		return NULL;
	memset(&info, 0, sizeof(info));
	if (asset->find(ref, &info) != 0)
		return NULL;
	return info.path;
}

/*
 * The shader a material draws with.
 *
 * A material's wire form begins with its shader ref as a u32, followed by the
 * std140 Material block — see asset_plugin.c's seeders. That header is the only
 * part of the format this module reads, and it reads it because the params a
 * material exposes are the *shader's* Material block: a material is values over
 * a declaration it does not own.
 */
static uint32_t material_shader_ref(const struct asset_api *asset, uint32_t ref)
{
	const void *bytes;
	uint32_t    size = 0, shader = 0;

	if (!asset || !asset->get_data || ref == 0u)
		return 0;
	bytes = asset->get_data(ref, &size);
	if (!bytes || size < sizeof(uint32_t))
		return 0;
	memcpy(&shader, bytes, sizeof(shader));
	return shader;
}

/* Declared fields for one slot, or -1 when the source is unusable. */
static int32_t declare(enum slot_kind kind, const char *src,
		       struct shader_param *out, uint32_t *total)
{
	switch (kind) {
	case SLOT_MESH:
		return script_mesh_params(src, out, BRIDGE_MAX_PARAM_FIELDS,
					  total);
	case SLOT_MATERIAL:
		return script_shader_material_params(src, out,
						     BRIDGE_MAX_PARAM_FIELDS,
						     total);
	default:
		return script_entity_params(src, out, BRIDGE_MAX_PARAM_FIELDS,
					    total);
	}
}

/* The entity's override bytes for one slot, or NULL when it has none. */
static const uint8_t *override_bytes(const struct world *w, enum slot_kind kind,
				     int32_t id, uint32_t *len)
{
	switch (kind) {
	case SLOT_MESH:
		return world_mesh_params(w, (uint32_t)id, len);
	case SLOT_MATERIAL:
		return world_material_params(w, (uint32_t)id, len);
	default:
		return world_script_params(w, (uint32_t)id, len);
	}
}

/*
 * One field's current value, component by component.
 *
 * The rule is the engine's own, not a new one: read from the override where it
 * is present *and long enough*, else fall back to the authored default, else to
 * zero. mesh_script_generate and its siblings resolve exactly this way when
 * they use the value, and the inspector showing a different number than the
 * renderer draws with is the one bug this whole module exists to prevent.
 */
static void resolve(const struct shader_param *field, const uint8_t *bytes,
		    uint32_t len, float out[4])
{
	uint32_t c, at;
	int32_t  as_int;
	float    as_float;
	int32_t  integral = strcmp(field->type, "int") == 0;

	for (c = 0; c < 4; c++)
		out[c] = 0.0f;

	for (c = 0; c < field->components && c < 4; c++) {
		at = field->offset + c * 4u;
		if (bytes && at + 4u <= len) {
			if (integral) {
				memcpy(&as_int, bytes + at, sizeof(as_int));
				out[c] = (float)as_int;
			} else {
				memcpy(&as_float, bytes + at, sizeof(as_float));
				out[c] = as_float;
			}
			continue;
		}
		/*
		 * No override for this component. The authored default covers
		 * its leading `default_count` components and no more, which is
		 * the declaration's own rule — a field may declare fewer
		 * defaults than it has components.
		 */
		if (c < field->default_count)
			out[c] = field->edit_default[c];
	}
}

static int32_t collect_slot(const struct world *w, const struct asset_api *asset,
			    enum slot_kind kind, uint32_t ref, int32_t id,
			    struct bridge_param_block *out)
{
	const char    *src;
	const uint8_t *bytes;
	uint32_t       len = 0, declared_ref = ref;
	int32_t        count, i;

	if (ref == 0u)
		return 0;

	/*
	 * A material's declaration belongs to its shader; the other two declare
	 * their own. This is the only place the difference shows.
	 */
	if (kind == SLOT_MATERIAL) {
		declared_ref = material_shader_ref(asset, ref);
		if (declared_ref == 0u)
			return 0;
	}

	src = asset_source(asset, declared_ref);
	if (!src)
		return 0;

	memset(out, 0, sizeof(*out));
	count = declare(kind, src, out->fields, &out->total);
	if (count < 0)
		return 0;
	/*
	 * Zero fields is a real answer — an asset with no (params ...) clause —
	 * and it is reported as a block with no fields rather than as no block,
	 * so the panel can say "this mesh has no parameters" instead of leaving
	 * the reader wondering whether it failed to ask.
	 */

	bytes = override_bytes(w, kind, id, &len);

	out->slot       = SLOT_NAME[kind];
	out->asset      = ref;
	out->path       = asset_path(asset, ref);
	out->count      = count;
	out->overridden = bytes != NULL && len > 0;
	out->truncated  = count >= BRIDGE_MAX_PARAM_FIELDS;

	for (i = 0; i < count; i++)
		resolve(&out->fields[i], bytes, len, out->values[i]);
	return 1;
}

int32_t bridge_params_collect(const struct entity_api *entity,
			      const struct asset_api *asset, int32_t id,
			      struct bridge_param_block *out, int32_t max)
{
	const struct world *w;
	int32_t             written = 0;

	if (!entity || !entity->get_world || !out || max <= 0)
		return -1;
	w = entity->get_world();
	if (!live_entity(w, id))
		return -1;

	if (written < max &&
	    collect_slot(w, asset, SLOT_MESH, w->render_ref[id], id,
			 &out[written]))
		written++;
	if (written < max &&
	    collect_slot(w, asset, SLOT_MATERIAL, w->material_ref[id], id,
			 &out[written]))
		written++;
	if (written < max &&
	    collect_slot(w, asset, SLOT_SCRIPT, w->script_ref[id], id,
			 &out[written]))
		written++;

	return written;
}

/* The largest block any slot can hold, so one scratch buffer serves all three. */
#define SLOT_BYTES_MAX WORLD_MATERIAL_PARAM_CAP

/* Write one field's components into `bytes` at the declaration's offset. */
static void pack(const struct shader_param *field, const float value[4],
		 uint8_t *bytes, uint32_t len)
{
	uint32_t c, at;
	int32_t  as_int;
	float    as_float;
	int32_t  integral = strcmp(field->type, "int") == 0;

	for (c = 0; c < field->components && c < 4; c++) {
		at = field->offset + c * 4u;
		if (at + 4u > len)
			return;
		if (integral) {
			as_int = (int32_t)value[c];
			memcpy(bytes + at, &as_int, sizeof(as_int));
		} else {
			as_float = value[c];
			memcpy(bytes + at, &as_float, sizeof(as_float));
		}
	}
}

int32_t bridge_params_write(const struct entity_api *entity,
			    const struct asset_api *asset, int32_t id,
			    int32_t slot, int32_t field, const float value[4])
{
	struct bridge_param_block blocks[BRIDGE_MAX_PARAM_BLOCKS];
	uint8_t                   bytes[SLOT_BYTES_MAX];
	uint32_t                  total;
	int32_t                   count, i;

	if (!value)
		return 0;
	count = bridge_params_collect(entity, asset, id, blocks,
				      BRIDGE_MAX_PARAM_BLOCKS);
	if (count <= 0 || slot < 0 || slot >= count)
		return 0;
	if (field < 0 || field >= blocks[slot].count)
		return 0;

	total = blocks[slot].total;
	if (total == 0 || total > sizeof(bytes))
		return 0;

	/*
	 * Rebuilt from the resolved values rather than from the entity's
	 * existing override, so a field that was still on its default keeps that
	 * default instead of becoming a zero the moment a neighbour is edited.
	 * That is the difference between "set width" and "set width and quietly
	 * flatten height".
	 */
	memset(bytes, 0, sizeof(bytes));
	for (i = 0; i < blocks[slot].count; i++)
		pack(&blocks[slot].fields[i], blocks[slot].values[i], bytes,
		     total);
	pack(&blocks[slot].fields[field], value, bytes, total);

	switch (slot_of(blocks[slot].slot)) {
	case SLOT_MESH:
		if (!entity->set_mesh_params)
			return 0;
		entity->set_mesh_params(id, bytes, total);
		return 1;
	case SLOT_MATERIAL:
		if (!entity->set_material_params)
			return 0;
		entity->set_material_params(id, bytes, total);
		return 1;
	default:
		if (!entity->set_script_params)
			return 0;
		entity->set_script_params(id, bytes, total);
		return 1;
	}
}
