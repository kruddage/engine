/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "scene.h"
#include "scene_plugin.h"
#include "asset_codec_api.h"
#include "subsystem_manager.h"
#include "memory_api.h"

#include <string.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __EMSCRIPTEN__
static const struct memory_api *g_mem;
#else
#include "memory.h"
static const struct memory_api native_mem = {
	mem_alloc, mem_alloc_zero, mem_free,
	mem_pool_create, mem_pool_alloc, mem_pool_free, mem_pool_destroy,
};
static const struct memory_api *g_mem = &native_mem;
#endif

void *scene_decode(const void *bytes, uint32_t size)
{
	const struct scene_header *hdr;
	const struct scene_entity *src;
	const char                *blob;
	struct scene              *s;
	size_t                     ent_bytes;
	uint32_t                   i;

	if (size < (uint32_t)sizeof(struct scene_header))
		return NULL;

	hdr = (const struct scene_header *)bytes;
	if (memcmp(hdr->magic, "KSCN", 4) != 0)
		return NULL;
	if (hdr->version != 1u)
		return NULL;

	ent_bytes = (size_t)hdr->entity_count
		* sizeof(struct scene_entity);
	if ((size_t)size < sizeof(*hdr) + ent_bytes
			+ (size_t)hdr->string_bytes)
		return NULL;

	src  = (const struct scene_entity *)
		((const uint8_t *)bytes + sizeof(*hdr));
	blob = (const char *)
		((const uint8_t *)bytes + sizeof(*hdr) + ent_bytes);

	for (i = 0; i < hdr->entity_count; i++) {
		if (src[i].parent != -1
				&& src[i].parent >= (int32_t)i)
			return NULL;
	}

	s = g_mem->alloc(sizeof(*s));
	if (!s)
		return NULL;
	s->count    = hdr->entity_count;
	s->entities = NULL;
	s->names    = NULL;

	if (hdr->entity_count > 0) {
		s->entities = g_mem->alloc(ent_bytes);
		if (!s->entities) {
			g_mem->free(s);
			return NULL;
		}
		memcpy(s->entities, src, ent_bytes);
	}

	if (hdr->string_bytes > 0) {
		s->names = g_mem->alloc(hdr->string_bytes);
		if (!s->names) {
			g_mem->free(s->entities);
			g_mem->free(s);
			return NULL;
		}
		memcpy(s->names, blob, hdr->string_bytes);
	}

	return s;
}

/*
 * Size of the name blob actually referenced by named entities: the highest
 * name offset plus the length of the string stored there.  A scene emitted by
 * world_export_scene packs names gap-free, so this reproduces its exact blob
 * size; returns 0 when nothing is named.
 */
static uint32_t scene_blob_size(const struct scene *s)
{
	uint32_t i, end = 0;

	if (!s->names)
		return 0;

	for (i = 0; i < s->count; i++) {
		const struct scene_entity *e = &s->entities[i];
		uint32_t                   stop;

		if (!(e->mask & COMPONENT_NAME) || e->name_off == SCENE_NO_NAME)
			continue;
		stop = e->name_off + (uint32_t)strlen(s->names + e->name_off) + 1u;
		if (stop > end)
			end = stop;
	}
	return end;
}

void *scene_encode(const struct scene *s, uint32_t *out_size)
{
	struct scene_header *hdr;
	uint8_t             *buf;
	uint32_t             string_bytes;
	size_t               ent_bytes, total;

	string_bytes = scene_blob_size(s);
	ent_bytes    = (size_t)s->count * sizeof(struct scene_entity);
	total        = sizeof(*hdr) + ent_bytes + (size_t)string_bytes;

	buf = g_mem->alloc(total);
	if (!buf)
		return NULL;

	hdr = (struct scene_header *)buf;
	memcpy(hdr->magic, "KSCN", 4);
	hdr->version      = 1u;
	hdr->entity_count = s->count;
	hdr->string_bytes = string_bytes;

	if (ent_bytes)
		memcpy(buf + sizeof(*hdr), s->entities, ent_bytes);
	if (string_bytes)
		memcpy(buf + sizeof(*hdr) + ent_bytes, s->names, string_bytes);

	if (out_size)
		*out_size = (uint32_t)total;
	return buf;
}

/* Adapter matching the codec encoder signature (typed -> bytes). */
static void *scene_encode_typed(const void *typed, uint32_t *out_size)
{
	return scene_encode((const struct scene *)typed, out_size);
}

#ifdef __EMSCRIPTEN__
void plugin_entry(struct subsystem_manager *mgr)
#else
void scene_plugin_entry(struct subsystem_manager *mgr)
#endif
{
	const struct asset_codec_api *codec;

#ifdef __EMSCRIPTEN__
	g_mem = subsystem_manager_get_api(mgr, "memory");
#endif
	codec = subsystem_manager_get_api(mgr, "asset_codec");
	if (codec) {
		codec->register_codec("scene", scene_decode);
		codec->register_encoder("scene", scene_encode_typed);
	}
}
