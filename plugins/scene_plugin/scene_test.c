/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "scene.h"
#include "scene_plugin.h"
#include "asset.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SCENE_PATH "/tmp/scene_test_krudd.scene"

/*
 * Name blob: "root\0child\0" — 11 bytes.
 * "root" at offset 0, "child" at offset 5.
 */
static const char names_blob[11] = {
	'r', 'o', 'o', 't', '\0', 'c', 'h', 'i', 'l', 'd', '\0'
};

static void write_scene_file(void)
{
	struct scene_header hdr;
	struct scene_entity ents[3];
	FILE               *f;

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic[0]     = 'K';
	hdr.magic[1]     = 'S';
	hdr.magic[2]     = 'C';
	hdr.magic[3]     = 'N';
	hdr.version      = 1u;
	hdr.entity_count = 3u;
	hdr.string_bytes = (uint32_t)sizeof(names_blob);

	memset(ents, 0, sizeof(ents));

	/* entity 0: root, named "root", identity transform */
	ents[0].mask       = COMPONENT_NAME;
	ents[0].parent     = -1;
	ents[0].rotation.w = 1.0f;
	ents[0].scale.x    = 1.0f;
	ents[0].scale.y    = 1.0f;
	ents[0].scale.z    = 1.0f;
	ents[0].name_off   = 0u;
	ents[0].render_ref = 0u;

	/* entity 1: child of 0, named "child", translated */
	ents[1].mask        = COMPONENT_NAME;
	ents[1].parent      = 0;
	ents[1].position.x  = 1.0f;
	ents[1].rotation.w  = 1.0f;
	ents[1].scale.x     = 1.0f;
	ents[1].scale.y     = 1.0f;
	ents[1].scale.z     = 1.0f;
	ents[1].name_off    = 5u;
	ents[1].render_ref  = 0u;

	/* entity 2: child of 1, render only */
	ents[2].mask        = COMPONENT_RENDER;
	ents[2].parent      = 1;
	ents[2].position.x  = 2.0f;
	ents[2].position.y  = 3.0f;
	ents[2].position.z  = 4.0f;
	ents[2].rotation.w  = 1.0f;
	ents[2].scale.x     = 2.0f;
	ents[2].scale.y     = 2.0f;
	ents[2].scale.z     = 2.0f;
	ents[2].name_off    = SCENE_NO_NAME;
	ents[2].render_ref  = 42u;

	f = fopen(SCENE_PATH, "wb");
	assert(f);
	fwrite(&hdr, 1, sizeof(hdr), f);
	fwrite(ents, 1, sizeof(ents), f);
	fwrite(names_blob, 1, sizeof(names_blob), f);
	fclose(f);
}

static void test_roundtrip(void)
{
	struct scene *s;

	write_scene_file();

	asset_request(SCENE_PATH);
	assert(asset_state_of(SCENE_PATH) == ASSET_LOADED);

	asset_codec_register("scene", scene_decode);
	s = asset_codec_get_typed(SCENE_PATH);
	assert(s != NULL);

	assert(s->count == 3u);

	/* entity 0: root */
	assert(s->entities[0].parent == -1);
	assert(s->entities[0].mask == COMPONENT_NAME);
	assert(s->entities[0].position.x == 0.0f);
	assert(s->names != NULL);
	assert(strcmp(s->names + s->entities[0].name_off, "root") == 0);

	/* entity 1: child of 0 */
	assert(s->entities[1].parent == 0);
	assert(s->entities[1].parent < (int32_t)1);
	assert(s->entities[1].position.x == 1.0f);
	assert(strcmp(s->names + s->entities[1].name_off, "child") == 0);

	/* entity 2: child of 1, render */
	assert(s->entities[2].parent == 1);
	assert(s->entities[2].parent < (int32_t)2);
	assert(s->entities[2].mask == COMPONENT_RENDER);
	assert(s->entities[2].render_ref == 42u);
	assert(s->entities[2].position.x == 2.0f);
	assert(s->entities[2].position.y == 3.0f);
	assert(s->entities[2].position.z == 4.0f);
	assert(s->entities[2].name_off == SCENE_NO_NAME);

	mem_free(s->entities);
	mem_free(s->names);
	mem_free(s);

	asset_release(SCENE_PATH);
}

/* Build a raw buffer from a header + optional entities, call scene_decode. */
static void build_buf(uint8_t *buf, const struct scene_header *hdr,
		      const struct scene_entity *ents, uint32_t n)
{
	memcpy(buf, hdr, sizeof(*hdr));
	if (ents && n > 0)
		memcpy(buf + sizeof(*hdr), ents,
		       n * sizeof(struct scene_entity));
}

static void test_bad_magic(void)
{
	struct scene_header hdr;

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic[0] = 'B';
	hdr.magic[1] = 'A';
	hdr.magic[2] = 'D';
	hdr.magic[3] = '!';
	hdr.version  = 1u;
	assert(scene_decode(&hdr, sizeof(hdr)) == NULL);
}

static void test_wrong_version(void)
{
	struct scene_header hdr;

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic[0] = 'K';
	hdr.magic[1] = 'S';
	hdr.magic[2] = 'C';
	hdr.magic[3] = 'N';
	hdr.version  = 2u;
	assert(scene_decode(&hdr, sizeof(hdr)) == NULL);
}

static void test_truncated(void)
{
	struct scene_header hdr;
	uint8_t             buf[sizeof(hdr)];

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic[0]     = 'K';
	hdr.magic[1]     = 'S';
	hdr.magic[2]     = 'C';
	hdr.magic[3]     = 'N';
	hdr.version      = 1u;
	hdr.entity_count = 5u; /* claims 5 entities but buffer has none */
	hdr.string_bytes = 0u;
	memcpy(buf, &hdr, sizeof(hdr));
	assert(scene_decode(buf, sizeof(buf)) == NULL);
}

static void test_bad_topology(void)
{
	struct scene_header hdr;
	struct scene_entity ents[2];
	uint8_t             buf[sizeof(hdr) + sizeof(ents)];

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic[0]     = 'K';
	hdr.magic[1]     = 'S';
	hdr.magic[2]     = 'C';
	hdr.magic[3]     = 'N';
	hdr.version      = 1u;
	hdr.entity_count = 2u;
	hdr.string_bytes = 0u;

	memset(ents, 0, sizeof(ents));
	ents[0].parent = -1;
	ents[1].parent = 1; /* parent == own index — invalid */

	build_buf(buf, &hdr, ents, 2);
	assert(scene_decode(buf, sizeof(buf)) == NULL);
}

static void test_empty_scene(void)
{
	struct scene_header hdr;
	struct scene       *s;

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic[0]     = 'K';
	hdr.magic[1]     = 'S';
	hdr.magic[2]     = 'C';
	hdr.magic[3]     = 'N';
	hdr.version      = 1u;
	hdr.entity_count = 0u;
	hdr.string_bytes = 0u;

	s = scene_decode(&hdr, sizeof(hdr));
	assert(s != NULL);
	assert(s->count == 0u);
	assert(s->entities == NULL);
	assert(s->names == NULL);
	mem_free(s);
}

/*
 * scene_encode is the exact inverse of scene_decode: encoding a decoded scene
 * reproduces the original bytes, and encoding is deterministic.
 */
static void test_encode_roundtrip(void)
{
	struct scene_header hdr;
	struct scene_entity ents[3];
	uint8_t             src[sizeof(struct scene_header)
			      + 3 * sizeof(struct scene_entity)
			      + sizeof(names_blob)];
	struct scene       *s;
	void               *enc, *enc2;
	uint32_t            enc_size = 0, enc2_size = 0;

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic[0]     = 'K';
	hdr.magic[1]     = 'S';
	hdr.magic[2]     = 'C';
	hdr.magic[3]     = 'N';
	hdr.version      = 1u;
	hdr.entity_count = 3u;
	hdr.string_bytes = (uint32_t)sizeof(names_blob);

	memset(ents, 0, sizeof(ents));
	ents[0].mask       = COMPONENT_NAME;
	ents[0].parent     = -1;
	ents[0].rotation.w = 1.0f;
	ents[0].scale.x    = ents[0].scale.y = ents[0].scale.z = 1.0f;
	ents[0].name_off   = 0u;
	ents[1].mask       = COMPONENT_NAME;
	ents[1].parent     = 0;
	ents[1].position.x = 1.0f;
	ents[1].rotation.w = 1.0f;
	ents[1].scale.x    = ents[1].scale.y = ents[1].scale.z = 1.0f;
	ents[1].name_off   = 5u;
	ents[2].mask       = COMPONENT_RENDER;
	ents[2].parent     = 1;
	ents[2].position.x = 2.0f;
	ents[2].position.y = 3.0f;
	ents[2].position.z = 4.0f;
	ents[2].rotation.w = 1.0f;
	ents[2].scale.x    = ents[2].scale.y = ents[2].scale.z = 2.0f;
	ents[2].name_off   = SCENE_NO_NAME;
	ents[2].render_ref = 42u;

	memcpy(src, &hdr, sizeof(hdr));
	memcpy(src + sizeof(hdr), ents, sizeof(ents));
	memcpy(src + sizeof(hdr) + sizeof(ents), names_blob, sizeof(names_blob));

	s = scene_decode(src, (uint32_t)sizeof(src));
	assert(s != NULL);

	enc = scene_encode(s, &enc_size);
	assert(enc != NULL);
	assert(enc_size == (uint32_t)sizeof(src));
	assert(memcmp(enc, src, enc_size) == 0);   /* canonical: inverse of decode */

	enc2 = scene_encode(s, &enc2_size);
	assert(enc2 != NULL);
	assert(enc2_size == enc_size);
	assert(memcmp(enc, enc2, enc_size) == 0);   /* deterministic */

	mem_free(enc);
	mem_free(enc2);
	mem_free(s->entities);
	mem_free(s->names);
	mem_free(s);
}

/* Encoding an empty scene yields a bare, decodable header. */
static void test_encode_empty(void)
{
	struct scene s = { 0u, NULL, NULL };
	struct scene *back;
	void         *enc;
	uint32_t      enc_size = 0;

	enc = scene_encode(&s, &enc_size);
	assert(enc != NULL);
	assert(enc_size == (uint32_t)sizeof(struct scene_header));

	back = scene_decode(enc, enc_size);
	assert(back != NULL);
	assert(back->count == 0u);

	mem_free(enc);
	mem_free(back);
}

int main(void)
{
	mem_init();
	log_init();

	test_roundtrip();
	test_bad_magic();
	test_wrong_version();
	test_truncated();
	test_bad_topology();
	test_empty_scene();
	test_encode_roundtrip();
	test_encode_empty();

	log_shutdown();
	mem_shutdown();

	printf("scene tests passed\n");
	return 0;
}
