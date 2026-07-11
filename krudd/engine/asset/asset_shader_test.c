/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "asset.h"
#include "asset_api.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SCENE_PATH "builtin://shader/scene"

/* Locate a built-in by path; return its catalog index, or -1 if absent. */
static int32_t index_of(const char *path)
{
	struct asset_info info;
	uint32_t          i;
	uint32_t          n;

	n = asset_catalog_count();
	for (i = 0; i < n; i++) {
		if (asset_catalog_info(i, &info) != 0)
			continue;
		if (strcmp(info.path, path) == 0)
			return (int32_t)i;
	}
	return -1;
}

/* Look up a decl field's value by key for catalog entry i. */
static const char *decl_value(uint32_t i, const char *key)
{
	struct asset_decl_field fields[8];
	uint32_t                n;
	uint32_t                f;

	n = asset_catalog_describe(i, fields, 8);
	for (f = 0; f < n; f++) {
		if (strcmp(fields[f].key, key) == 0)
			return fields[f].value;
	}
	return NULL;
}

/*
 * A built-in shader asset must exist, be a loaded read-only primitive of type
 * SHADER, deliver its NUL-terminated DSL source through get_data() (one asset
 * holding every stage — no .vert/.frag split), and advertise the DSL format
 * plus the stages it defines via describe().
 */
static void check_shader(const char *path)
{
	struct asset_info info;
	const char       *src;
	uint32_t          size;
	int32_t           idx;

	idx = index_of(path);
	assert(idx >= 0);

	assert(asset_catalog_info((uint32_t)idx, &info) == 0);
	assert(info.type      == ASSET_TYPE_SHADER);
	assert(info.kind      == ASSET_KIND_PRIMITIVE);
	assert(info.read_only == 1);
	assert(info.state     == ASSET_LOADED);
	assert(info.id        != 0);

	/* The stored source is the DSL, NUL-terminated — what the editor shows. */
	size = 0;
	src  = asset_catalog_get_data(info.id, &size);
	assert(src != NULL);
	assert(size > 0);
	assert(src[size - 1] == '\0');
	assert(strlen(src) + 1 == size);
	assert(strstr(src, "(shader") == src);
	assert(strstr(src, "(vertex") != NULL);
	assert(strstr(src, "(fragment") != NULL);
	assert(strstr(src, "#version") == NULL);

	/* Source format + present stages ride on the asset. */
	assert(decl_value((uint32_t)idx, "format") != NULL);
	assert(strcmp(decl_value((uint32_t)idx, "format"), "krudd-shader") == 0);
	assert(decl_value((uint32_t)idx, "stages") != NULL);
	assert(strcmp(decl_value((uint32_t)idx, "stages"),
		      "vertex, fragment") == 0);
}

/*
 * Authored shaders round-trip arbitrary source through the same path: the
 * asset system is dialect-agnostic storage; the renderer owns compilation.
 */
static void check_authored_roundtrip(void)
{
	static const char  src[] = "#version 300 es\nvoid main() {}\n";
	const char        *out;
	uint32_t           size;
	uint32_t           id;

	id = asset_mut_create("project://shader/custom.frag",
			      ASSET_TYPE_SHADER, src, (uint32_t)sizeof(src));
	assert(id != 0);

	size = 0;
	out  = asset_catalog_get_data(id, &size);
	assert(out != NULL);
	assert(size == (uint32_t)sizeof(src));
	assert(memcmp(out, src, sizeof(src)) == 0);

	assert(asset_mut_destroy(id) == 0);
}

/*
 * Authored shader declarations: an authored shader carries no decl until one
 * is set (there is no path-derived stage anymore — stages live inside the DSL);
 * set_decl publishes editor metadata; n == 0 clears it; and it is rejected on
 * read-only built-ins, over-capacity counts, and null fields.
 */
static void check_authored_decl(void)
{
	struct asset_decl_field fields[2];
	struct asset_decl_field two[2];
	struct asset_info       info;
	uint32_t                sh;
	int32_t                 idx;

	/* No explicit decl: describe() reports nothing for an authored shader. */
	sh = asset_mut_create("project://shader/ripple", ASSET_TYPE_SHADER,
			      "", 0);
	assert(sh != 0);
	idx = index_of("project://shader/ripple");
	assert(idx >= 0);
	assert(asset_catalog_describe((uint32_t)idx, fields, 2) == 0);

	/* set_decl publishes editor-set metadata. */
	two[0].key = "format"; two[0].value = "krudd-shader";
	two[1].key = "stages"; two[1].value = "fragment";
	assert(asset_mut_set_decl(sh, two, 2) == 0);
	idx = index_of("project://shader/ripple");
	assert(strcmp(decl_value((uint32_t)idx, "stages"), "fragment") == 0);

	/* n == 0 clears the decl; describe() reports nothing again. */
	assert(asset_mut_set_decl(sh, NULL, 0) == 0);
	idx = index_of("project://shader/ripple");
	assert(asset_catalog_describe((uint32_t)idx, fields, 2) == 0);

	/* An over-capacity count is rejected (max is small and internal). */
	assert(asset_mut_set_decl(sh, two, 1000) == -1);

	/* null fields with n > 0 is rejected. */
	assert(asset_mut_set_decl(sh, NULL, 1) == -1);

	/* set_decl on a read-only built-in is rejected. */
	assert(asset_catalog_info(0, &info) == 0);
	assert(info.read_only == 1);
	two[0].key = "stages"; two[0].value = "vertex";
	assert(asset_mut_set_decl(info.id, two, 1) == -1);

	assert(asset_mut_destroy(sh) == 0);
}

int main(void)
{
	mem_init();
	log_init();

	asset_init();

	check_shader(SCENE_PATH);
	check_authored_roundtrip();
	check_authored_decl();

	log_shutdown();
	mem_shutdown();

	printf("asset_shader tests passed\n");
	return 0;
}
