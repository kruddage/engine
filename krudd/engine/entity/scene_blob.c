/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "scene.h"
#include "scene_blob.h"
#include "memory_api.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * struct scene carries no explicit name-blob length, so derive it: the
 * blob is tightly packed by every producer in this tree (world_export_scene,
 * and any hand-built struct scene following the same convention), so the
 * furthest (name_off + strlen + 1) reached by a referenced name is the
 * blob's true length.
 */
static uint32_t scene_names_len(const struct scene *s)
{
	uint32_t i, end, max_end = 0;

	if (!s->names)
		return 0;
	for (i = 0; i < s->count; i++) {
		const struct scene_entity *se = &s->entities[i];

		if (!(se->mask & COMPONENT_NAME) || se->name_off == SCENE_NO_NAME)
			continue;
		end = se->name_off + (uint32_t)strlen(s->names + se->name_off) + 1u;
		if (end > max_end)
			max_end = end;
	}
	return max_end;
}

uint32_t scene_blob_size(const struct scene *s)
{
	return (uint32_t)sizeof(struct scene_blob_header)
	       + s->count * (uint32_t)sizeof(struct scene_entity)
	       + scene_names_len(s);
}

uint32_t scene_blob_encode(const struct scene *s, void *out, uint32_t cap)
{
	struct scene_blob_header hdr;
	uint32_t                 string_bytes = scene_names_len(s);
	uint32_t                 need = (uint32_t)sizeof(hdr)
					 + s->count * (uint32_t)sizeof(struct scene_entity)
					 + string_bytes;
	uint8_t                  *p = out;

	if (cap < need)
		return 0;

	hdr.magic          = SCENE_BLOB_MAGIC;
	hdr.format_version = SCENE_BLOB_FORMAT_VERSION;
	hdr.entity_count   = s->count;
	hdr.string_bytes   = string_bytes;
	memcpy(p, &hdr, sizeof(hdr));
	p += sizeof(hdr);

	if (s->count)
		memcpy(p, s->entities, s->count * sizeof(*s->entities));
	p += s->count * sizeof(*s->entities);

	if (string_bytes)
		memcpy(p, s->names, string_bytes);

	return need;
}

struct scene *scene_blob_decode(const void *bytes, uint32_t size,
				const struct memory_api *mem)
{
	struct scene_blob_header hdr;
	const uint8_t            *p = bytes;
	struct scene              *s;
	uint32_t                   entities_bytes;

	if (!mem || size < sizeof(hdr))
		return NULL;
	memcpy(&hdr, p, sizeof(hdr));
	if (hdr.magic != SCENE_BLOB_MAGIC
	    || hdr.format_version != SCENE_BLOB_FORMAT_VERSION)
		return NULL;
	p += sizeof(hdr);

	entities_bytes = hdr.entity_count * (uint32_t)sizeof(struct scene_entity);
	if (size < sizeof(hdr) + entities_bytes + hdr.string_bytes)
		return NULL;

	s = mem->alloc_zero(sizeof(*s));
	if (!s)
		return NULL;
	s->count = hdr.entity_count;

	if (hdr.entity_count) {
		s->entities = mem->alloc(entities_bytes);
		if (!s->entities) {
			mem->free(s);
			return NULL;
		}
		memcpy(s->entities, p, entities_bytes);
	}
	p += entities_bytes;

	if (hdr.string_bytes) {
		s->names = mem->alloc(hdr.string_bytes);
		if (!s->names) {
			mem->free(s->entities);
			mem->free(s);
			return NULL;
		}
		memcpy(s->names, p, hdr.string_bytes);
	}

	return s;
}
