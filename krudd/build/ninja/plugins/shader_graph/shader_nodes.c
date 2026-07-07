/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "shader_graph.h"
#include "vscript_api.h"

#include <stdint.h>

/*
 * The shader node set (sub-issue 2).  Each node type declares its typed ports
 * over the SG_TYPE_* lattice and carries a GLSL expression template (consumed
 * by the backend in glsl_backend.c).  v1 authors the fragment surface over a
 * fixed passthrough vertex, so there is a single surface Output node.
 */

/* Reusable port descriptors. */
static const struct vscript_port p_float[]  = { { "x", SG_TYPE_FLOAT } };
static const struct vscript_port p_vec2[]   = { { "x", SG_TYPE_VEC2 } };
static const struct vscript_port p_vec3[]   = { { "x", SG_TYPE_VEC3 } };
static const struct vscript_port p_vec4[]   = { { "x", SG_TYPE_VEC4 } };
static const struct vscript_port p_sampler[] = { { "s", SG_TYPE_SAMPLER2D } };

static const struct vscript_port in_sample[] = {
	{ "tex", SG_TYPE_SAMPLER2D }, { "uv", SG_TYPE_VEC2 },
};
static const struct vscript_port in_vec4_2[] = {
	{ "a", SG_TYPE_VEC4 }, { "b", SG_TYPE_VEC4 },
};
static const struct vscript_port in_vec4_float[] = {
	{ "a", SG_TYPE_VEC4 }, { "s", SG_TYPE_FLOAT },
};
static const struct vscript_port in_mix[] = {
	{ "a", SG_TYPE_VEC4 }, { "b", SG_TYPE_VEC4 }, { "t", SG_TYPE_FLOAT },
};
static const struct vscript_port in_make4[] = {
	{ "xyz", SG_TYPE_VEC3 }, { "w", SG_TYPE_FLOAT },
};
static const struct vscript_port in_dot3[] = {
	{ "a", SG_TYPE_VEC3 }, { "b", SG_TYPE_VEC3 },
};

/* Codegen metadata (vscript_node_type.user). */
static const struct sg_node_meta m_const_float = { "$p", 0, 0 };
static const struct sg_node_meta m_const_vec4  = { "vec4($p)", 0, 0 };
static const struct sg_node_meta m_param       = { "$p", 1, 0 };
static const struct sg_node_meta m_uv          = { "v_uv", 0, 0 };
static const struct sg_node_meta m_sample      = { "texture($0, $1)", 0, 0 };
static const struct sg_node_meta m_add         = { "($0 + $1)", 0, 0 };
static const struct sg_node_meta m_mul         = { "($0 * $1)", 0, 0 };
static const struct sg_node_meta m_scale       = { "($0 * $1)", 0, 0 };
static const struct sg_node_meta m_mix         = { "mix($0, $1, $2)", 0, 0 };
static const struct sg_node_meta m_make4       = { "vec4($0, $1)", 0, 0 };
static const struct sg_node_meta m_swizzle_rgb = { "$0.rgb", 0, 0 };
static const struct sg_node_meta m_dot         = { "dot($0, $1)", 0, 0 };
static const struct sg_node_meta m_output      = { "$0", 0, 1 };

static const struct vscript_node_type node_types[] = {
	/* Constants: param carries the literal component text. */
	{ .name = "const_float", .outputs = p_float, .output_count = 1,
	  .user = &m_const_float },
	{ .name = "const_vec4", .outputs = p_vec4, .output_count = 1,
	  .user = &m_const_vec4 },

	/* Parameters (uniforms): param carries the uniform name. */
	{ .name = "param_float", .outputs = p_float, .output_count = 1,
	  .user = &m_param },
	{ .name = "param_vec4", .outputs = p_vec4, .output_count = 1,
	  .user = &m_param },
	{ .name = "param_tex", .outputs = p_sampler, .output_count = 1,
	  .user = &m_param },

	/* Varying input from the passthrough vertex. */
	{ .name = "uv", .outputs = p_vec2, .output_count = 1, .user = &m_uv },

	/* Sample + arithmetic. */
	{ .name = "sample", .inputs = in_sample, .input_count = 2,
	  .outputs = p_vec4, .output_count = 1, .user = &m_sample },
	{ .name = "add", .inputs = in_vec4_2, .input_count = 2,
	  .outputs = p_vec4, .output_count = 1, .user = &m_add },
	{ .name = "mul", .inputs = in_vec4_2, .input_count = 2,
	  .outputs = p_vec4, .output_count = 1, .user = &m_mul },
	{ .name = "scale", .inputs = in_vec4_float, .input_count = 2,
	  .outputs = p_vec4, .output_count = 1, .user = &m_scale },
	{ .name = "mix", .inputs = in_mix, .input_count = 3,
	  .outputs = p_vec4, .output_count = 1, .user = &m_mix },

	/* Construct / split. */
	{ .name = "make_vec4", .inputs = in_make4, .input_count = 2,
	  .outputs = p_vec4, .output_count = 1, .user = &m_make4 },
	{ .name = "swizzle_rgb", .inputs = p_vec4, .input_count = 1,
	  .outputs = p_vec3, .output_count = 1, .user = &m_swizzle_rgb },
	{ .name = "dot", .inputs = in_dot3, .input_count = 2,
	  .outputs = p_float, .output_count = 1, .user = &m_dot },

	/* Surface output. */
	{ .name = "output", .inputs = p_vec4, .input_count = 1,
	  .user = &m_output },
};

#define NODE_TYPE_COUNT \
	(uint32_t)(sizeof(node_types) / sizeof(node_types[0]))

int32_t shader_nodes_register(const struct vscript_api *vs)
{
	uint32_t i;

	if (!vs)
		return -1;
	for (i = 0; i < NODE_TYPE_COUNT; i++) {
		if (vs->register_node_type(&node_types[i]) != 0)
			return -1;
	}
	return 0;
}
