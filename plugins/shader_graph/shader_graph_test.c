/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "shader_graph.h"
#include "vscript.h"
#include "vscript_api.h"
#include "asset.h"
#include "asset_api.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const struct vscript_api *vs;

/* Native asset_mut vtable so the backend can emit a derived shader asset. */
static const struct asset_mut_api native_mut = {
	asset_mut_create, asset_mut_set_data, asset_mut_destroy,
	asset_mut_set_decl, asset_mut_inject,
};

/*
 * Build the canonical non-trivial surface: a texture sampled at the varying UV,
 * multiplied by a parameterised tint.  uv -> sample(u_tex, uv) -> mul(., u_tint)
 * -> output.  Returns the graph; *out receives the output node id.
 */
static vscript_graph_t build_surface(void)
{
	vscript_graph_t g = vs->create(VSCRIPT_TARGET_SHADER);
	int32_t uv, tex, smp, tint, mul, out;

	uv   = vs->add_node(g, "uv", NULL);
	tex  = vs->add_node(g, "param_tex", "u_tex");
	smp  = vs->add_node(g, "sample", NULL);
	tint = vs->add_node(g, "param_vec4", "u_tint");
	mul  = vs->add_node(g, "mul", NULL);
	out  = vs->add_node(g, "output", NULL);
	assert(uv >= 1 && tex >= 1 && smp >= 1 && tint >= 1 && mul >= 1
	       && out >= 1);

	assert(vs->connect(g, tex, 0, smp, 0) == 0); /* sampler -> sample.tex */
	assert(vs->connect(g, uv, 0, smp, 1) == 0);  /* uv -> sample.uv */
	assert(vs->connect(g, smp, 0, mul, 0) == 0); /* sample -> mul.a */
	assert(vs->connect(g, tint, 0, mul, 1) == 0);/* tint -> mul.b */
	assert(vs->connect(g, mul, 0, out, 0) == 0); /* mul -> output */
	return g;
}

static void test_type_mismatch(void)
{
	vscript_graph_t g = vs->create(VSCRIPT_TARGET_SHADER);
	int32_t uv  = vs->add_node(g, "uv", NULL);
	int32_t out = vs->add_node(g, "output", NULL);

	/* vec2 out cannot drive the vec4 surface input; no implicit promote. */
	assert(vs->connect(g, uv, 0, out, 0) == -1);
	vs->destroy(g);
}

static void test_emit_fragment(void)
{
	vscript_graph_t g = build_surface();
	char           *src;
	uint32_t        size = 0;

	src = sg_emit_fragment(vs, g, &size);
	assert(src != NULL);
	assert(size == (uint32_t)strlen(src) + 1);

	/* Header + precision + varying. */
	assert(strstr(src, "#version 300 es") == src);
	assert(strstr(src, "precision highp float;") != NULL);
	assert(strstr(src, "in vec2 v_uv;") != NULL);

	/* Parameter nodes surfaced as uniforms of the right type. */
	assert(strstr(src, "uniform sampler2D u_tex;") != NULL);
	assert(strstr(src, "uniform vec4 u_tint;") != NULL);

	/* Sampler is inlined at the call site, never bound to a local. */
	assert(strstr(src, "texture(u_tex, v") != NULL);
	assert(strstr(src, "\tsampler2D") == NULL);

	/* Surface output writes the fragment colour. */
	assert(strstr(src, "out vec4 frag_color;") != NULL);
	assert(strstr(src, "void main()") != NULL);
	assert(strstr(src, "frag_color = v") != NULL);

	mem_free(src);
	vs->destroy(g);
}

static void test_missing_output(void)
{
	vscript_graph_t g = vs->create(VSCRIPT_TARGET_SHADER);
	uint32_t        size = 0;

	vs->add_node(g, "uv", NULL); /* no output node */
	assert(sg_emit_fragment(vs, g, &size) == NULL);
	vs->destroy(g);
}

static void test_non_shader_target_refused(void)
{
	vscript_graph_t g = vs->create(VSCRIPT_TARGET_GAMESCRIPT);
	uint32_t        size = 0;

	assert(sg_emit_fragment(vs, g, &size) == NULL);
	vs->destroy(g);
}

/* Locate a catalog entry index by path, or -1. */
static int32_t index_of(const char *path)
{
	struct asset_info info;
	uint32_t          i, n = asset_catalog_count();

	for (i = 0; i < n; i++) {
		if (asset_catalog_info(i, &info) == 0
				&& strcmp(info.path, path) == 0)
			return (int32_t)i;
	}
	return -1;
}

static const char *decl_value(int32_t idx, const char *key)
{
	struct asset_decl_field fields[8];
	uint32_t                f, k;

	f = asset_catalog_describe((uint32_t)idx, fields, 8);
	for (k = 0; k < f; k++) {
		if (strcmp(fields[k].key, key) == 0)
			return fields[k].value;
	}
	return NULL;
}

static void test_compile_to_asset(void)
{
	static const char *path = "project://shader/generated.frag";
	vscript_graph_t    g = build_surface();
	struct asset_info  info;
	const char        *out;
	uint32_t           size = 0;
	uint32_t           id;
	int32_t            idx;

	id = sg_compile_to_shader(vs, &native_mut, g, path);
	assert(id != 0);

	idx = index_of(path);
	assert(idx >= 0);
	assert(asset_catalog_info((uint32_t)idx, &info) == 0);
	assert(info.type == ASSET_TYPE_SHADER);

	/* Derived asset carries NUL-terminated GLSL the renderer can read. */
	out = asset_catalog_get_data(info.id, &size);
	assert(out != NULL && size > 0);
	assert(out[size - 1] == '\0');
	assert(strstr(out, "#version 300 es") == out);

	/* Tagged dialect + stage so the WebGL backend compiles it unchanged. */
	assert(strcmp(decl_value(idx, "dialect"), "glsl_es_300") == 0);
	assert(strcmp(decl_value(idx, "stage"), "fragment") == 0);

	/* The derived shader is a distinct asset from the source graph. */
	assert(info.type != ASSET_TYPE_VSCRIPT);

	assert(asset_mut_destroy(id) == 0);
	vs->destroy(g);
}

static void test_passthrough_vertex(void)
{
	const char *v = sg_passthrough_vertex();

	assert(strstr(v, "#version 300 es") == v);
	assert(strstr(v, "out vec2 v_uv;") != NULL);
	assert(strstr(v, "gl_Position") != NULL);
}

int main(void)
{
	mem_init();
	log_init();
	asset_init();

	vs = vscript_native_api();
	assert(shader_nodes_register(vs) == 0);

	test_type_mismatch();
	test_emit_fragment();
	test_missing_output();
	test_non_shader_target_refused();
	test_compile_to_asset();
	test_passthrough_vertex();

	log_shutdown();
	mem_shutdown();

	printf("shader_graph tests passed\n");
	return 0;
}
