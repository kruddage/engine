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
	if (codec)
		codec->register_codec("scene", scene_decode);
}
