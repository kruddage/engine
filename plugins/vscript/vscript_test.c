/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "vscript.h"
#include "vscript_api.h"
#include "asset.h"
#include "asset_api.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/*
 * The core ships no domain nodes, so the test contributes a tiny throwaway
 * node set purely to exercise the graph mechanics: two opaque type tags and a
 * source/transform/sink chain over them.
 */
#define T_A 1u
#define T_B 2u

static int g_user_marker = 42;

static const struct vscript_port a_out[]  = { { "out", T_A } };
static const struct vscript_port a_in[]   = { { "in",  T_A } };
static const struct vscript_port b_out[]  = { { "out", T_B } };
static const struct vscript_port b_in[]   = { { "in",  T_B } };

static const struct vscript_node_type ty_src = {
	.name = "src", .outputs = a_out, .output_count = 1,
	.user = &g_user_marker,
};
static const struct vscript_node_type ty_mid = {
	.name = "mid", .inputs = a_in, .input_count = 1,
	.outputs = b_out, .output_count = 1,
};
static const struct vscript_node_type ty_sink = {
	.name = "sink", .inputs = b_in, .input_count = 1,
};
static const struct vscript_node_type ty_loop = {
	.name = "loop", .inputs = a_in, .input_count = 1,
	.outputs = a_out, .output_count = 1,
};

static const struct vscript_api *vs;

static void test_registry(void)
{
	struct vscript_port bad[] = { { "x", 0u } };
	struct vscript_node_type badty = {
		.name = "bad", .inputs = bad, .input_count = 1,
	};

	assert(vs->register_node_type(&ty_src) == 0);
	assert(vs->register_node_type(&ty_mid) == 0);
	assert(vs->register_node_type(&ty_sink) == 0);
	assert(vs->register_node_type(&ty_loop) == 0);

	/* Duplicate name and zero-tag ports are rejected. */
	assert(vs->register_node_type(&ty_src) == -1);
	assert(vs->register_node_type(&badty) == -1);
}

static void test_build_and_validate(void)
{
	vscript_graph_t g = vs->create(VSCRIPT_TARGET_SHADER);
	int32_t a, m, s, a2;

	assert(g != NULL);
	assert(vs->target(g) == VSCRIPT_TARGET_SHADER);

	a = vs->add_node(g, "src", NULL);
	m = vs->add_node(g, "mid", NULL);
	s = vs->add_node(g, "sink", NULL);
	assert(a >= 1 && m >= 1 && s >= 1);
	assert(vs->add_node(g, "nope", NULL) == -1); /* unknown type */

	/* Well-typed wiring. */
	assert(vs->connect(g, a, 0, m, 0) == 0);
	assert(vs->connect(g, m, 0, s, 0) == 0);

	/* Type mismatch (T_A out -> T_B in) is rejected. */
	a2 = vs->add_node(g, "src", NULL);
	assert(vs->connect(g, a2, 0, s, 0) == -1);
	/* Single driver per input: s.0 already driven. */
	assert(vs->connect(g, m, 0, s, 0) == -1);
	/* Out-of-range ports. */
	assert(vs->connect(g, a, 1, m, 0) == -1);
	assert(vs->connect(g, a, 0, m, 9) == -1);

	assert(vs->validate(g) == 0);
	vs->destroy(g);
}

static void test_topo_order(void)
{
	vscript_graph_t g = vs->create(VSCRIPT_TARGET_SHADER);
	int32_t out[8];
	int32_t a, m, s, n;
	int     ia = -1, im = -1, is = -1, i;

	a = vs->add_node(g, "src", NULL);
	m = vs->add_node(g, "mid", NULL);
	s = vs->add_node(g, "sink", NULL);
	assert(vs->connect(g, a, 0, m, 0) == 0);
	assert(vs->connect(g, m, 0, s, 0) == 0);

	n = vs->topo_order(g, out, 8);
	assert(n == 3);
	for (i = 0; i < n; i++) {
		if (out[i] == a) ia = i;
		if (out[i] == m) im = i;
		if (out[i] == s) is = i;
	}
	assert(ia >= 0 && im >= 0 && is >= 0);
	assert(ia < im && im < is); /* drivers precede dependents */

	vs->destroy(g);
}

static void test_cycle_rejected(void)
{
	vscript_graph_t g = vs->create(VSCRIPT_TARGET_SHADER);
	int32_t out[8];
	int32_t x, y;

	x = vs->add_node(g, "loop", NULL);
	y = vs->add_node(g, "loop", NULL);
	assert(vs->connect(g, x, 0, y, 0) == 0);
	assert(vs->connect(g, y, 0, x, 0) == 0);

	assert(vs->validate(g) == -1);
	assert(vs->topo_order(g, out, 8) == -1);
	vs->destroy(g);
}

static void test_introspection(void)
{
	vscript_graph_t g = vs->create(VSCRIPT_TARGET_SHADER);
	int32_t a, m;
	int32_t src_node = 0;
	uint32_t src_port = 99;

	a = vs->add_node(g, "src", "hello");
	m = vs->add_node(g, "mid", NULL);
	assert(vs->connect(g, a, 0, m, 0) == 0);

	assert(strcmp(vs->node_type_name(g, a), "src") == 0);
	assert(strcmp(vs->node_param(g, a), "hello") == 0);
	assert(vs->node_param(g, m) == NULL);
	assert(vs->node_user(g, a) == &g_user_marker);
	assert(vs->node_input_count(g, m) == 1);
	assert(vs->node_output_count(g, a) == 1);
	assert(vs->port_type(g, a, 1, 0) == T_A);
	assert(vs->port_type(g, m, 0, 0) == T_A);
	assert(vs->node_count(g) == 2);

	assert(vs->input_source(g, m, 0, &src_node, &src_port) == 0);
	assert(src_node == a && src_port == 0);
	assert(vs->input_source(g, a, 0, NULL, NULL) == -1); /* unconnected */

	vs->destroy(g);
}

static void test_target_checks(void)
{
	vscript_graph_t g = vs->create(VSCRIPT_TARGET_SHADER);

	assert(vs->require_target(g, VSCRIPT_TARGET_SHADER) == 0);
	assert(vs->require_target(g, VSCRIPT_TARGET_GAMESCRIPT) == -1);
	assert(strcmp(vscript_target_decl_value(VSCRIPT_TARGET_SHADER),
		      "shader") == 0);
	assert(vscript_target_decl_value(VSCRIPT_TARGET_INVALID) == NULL);
	vs->destroy(g);
}

static void test_editing(void)
{
	vscript_graph_t g = vs->create(VSCRIPT_TARGET_SHADER);
	int32_t a, m, s;
	int32_t dummy = 0;

	a = vs->add_node(g, "src", NULL);
	m = vs->add_node(g, "mid", NULL);
	s = vs->add_node(g, "sink", NULL);
	assert(vs->connect(g, a, 0, m, 0) == 0);
	assert(vs->connect(g, m, 0, s, 0) == 0);
	assert(vs->node_count(g) == 3);

	/* node_id_at enumerates the live nodes. */
	assert(vs->node_id_at(g, 0) == a);
	assert(vs->node_id_at(g, 2) == s);
	assert(vs->node_id_at(g, 3) == -1);

	/* disconnect drops a single wire and frees the input. */
	assert(vs->disconnect(g, s, 0) == 0);
	assert(vs->input_source(g, s, 0, NULL, NULL) == -1);
	assert(vs->disconnect(g, s, 0) == -1); /* already gone */
	assert(vs->connect(g, m, 0, s, 0) == 0); /* re-wire now allowed */

	/* set_param edits a node's param in place. */
	assert(vs->set_param(g, a, "edited") == 0);
	assert(strcmp(vs->node_param(g, a), "edited") == 0);
	assert(vs->set_param(g, a, NULL) == 0);
	assert(vs->node_param(g, a) == NULL);
	assert(vs->set_param(g, 9999, "x") == -1); /* unknown node */

	/* remove_node drops the node and its incident connections. */
	assert(vs->remove_node(g, m) == 0);
	assert(vs->node_count(g) == 2);
	assert(vs->node_type_name(g, m) == NULL);
	assert(vs->input_source(g, s, 0, &dummy, NULL) == -1); /* m->s gone */
	assert(vs->remove_node(g, m) == -1);                   /* already gone */

	vs->destroy(g);
}

static void test_registry_enum(void)
{
	uint32_t n = vs->type_count();
	uint32_t i;
	int      saw_src = 0;

	assert(n >= 4); /* src, mid, sink, loop from test_registry */
	for (i = 0; i < n; i++) {
		const char *name = vs->type_name_at(i);

		assert(name != NULL);
		if (strcmp(name, "src") == 0)
			saw_src = 1;
	}
	assert(saw_src);
	assert(vs->type_name_at(n) == NULL);
}

/* Encode a graph, decode it back, and confirm structure + ids survive. */
static void test_codec_roundtrip(void)
{
	vscript_graph_t g = vs->create(VSCRIPT_TARGET_SHADER);
	vscript_graph_t back;
	void           *bytes;
	uint32_t        size = 0;
	int32_t         a, m, s, node = 0;
	uint32_t        port = 0;

	a = vs->add_node(g, "src", "seed");
	m = vs->add_node(g, "mid", NULL);
	s = vs->add_node(g, "sink", NULL);
	assert(vs->connect(g, a, 0, m, 0) == 0);
	assert(vs->connect(g, m, 0, s, 0) == 0);

	bytes = vscript_encode(g, &size);
	assert(bytes != NULL && size > 0);

	back = vscript_decode(bytes, size);
	assert(back != NULL);
	assert(vs->target(back) == VSCRIPT_TARGET_SHADER);
	assert(vs->node_count(back) == 3);
	/* Stable ids and connections survive the round-trip. */
	assert(strcmp(vs->node_type_name(back, a), "src") == 0);
	assert(strcmp(vs->node_param(back, a), "seed") == 0);
	assert(vs->input_source(back, m, 0, &node, &port) == 0);
	assert(node == a && port == 0);
	assert(vs->input_source(back, s, 0, &node, &port) == 0);
	assert(node == m);

	/* Malformed buffers are rejected. */
	assert(vscript_decode(bytes, 3) == NULL);
	((uint8_t *)bytes)[0] = 'X';
	assert(vscript_decode(bytes, size) == NULL);

	mem_free(bytes);
	vs->destroy(back);
	vs->destroy(g);
}

/* Find a catalog entry's decl value by key, or NULL. */
static const char *decl_value(const char *path, const char *key)
{
	struct asset_decl_field fields[8];
	struct asset_info       info;
	uint32_t                i, n, f, k;

	n = asset_catalog_count();
	for (i = 0; i < n; i++) {
		if (asset_catalog_info(i, &info) != 0)
			continue;
		if (strcmp(info.path, path) != 0)
			continue;
		f = asset_catalog_describe(i, fields, 8);
		for (k = 0; k < f; k++) {
			if (strcmp(fields[k].key, key) == 0)
				return fields[k].value;
		}
		return NULL;
	}
	return NULL;
}

/*
 * A graph asset advertises its target two ways: cheaply via a mirrored decl
 * (filter without decoding) and authoritatively via the decoded header.
 */
static void test_asset_target_mirror(void)
{
	struct asset_decl_field decl[1];
	vscript_graph_t         g = vs->create(VSCRIPT_TARGET_SHADER);
	vscript_graph_t         back;
	void                   *bytes;
	uint32_t                size = 0;
	uint32_t                id;
	const char             *val;

	vs->add_node(g, "src", NULL);
	bytes = vscript_encode(g, &size);
	assert(bytes != NULL);

	id = asset_mut_create("project://graph/surface.vscript",
			      ASSET_TYPE_VSCRIPT, bytes, size);
	assert(id != 0);

	decl[0].key   = VSCRIPT_TARGET_DECL_KEY;
	decl[0].value = vscript_target_decl_value(VSCRIPT_TARGET_SHADER);
	assert(asset_mut_set_decl(id, decl, 1) == 0);

	/* Cheap path: the decl surfaces the target without decoding. */
	val = decl_value("project://graph/surface.vscript",
			 VSCRIPT_TARGET_DECL_KEY);
	assert(val != NULL && strcmp(val, "shader") == 0);

	/* Authoritative path: decode + require_target. */
	back = vscript_decode(bytes, size);
	assert(back != NULL);
	assert(vs->require_target(back, VSCRIPT_TARGET_SHADER) == 0);

	mem_free(bytes);
	vs->destroy(back);
	vs->destroy(g);
	assert(asset_mut_destroy(id) == 0);
}

int main(void)
{
	mem_init();
	log_init();
	asset_init();

	vs = vscript_native_api();

	test_registry();
	test_build_and_validate();
	test_topo_order();
	test_cycle_rejected();
	test_introspection();
	test_editing();
	test_registry_enum();
	test_target_checks();
	test_codec_roundtrip();
	test_asset_target_mirror();

	log_shutdown();
	mem_shutdown();

	printf("vscript tests passed\n");
	return 0;
}
