/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SHADER_GRAPH_H
#define SHADER_GRAPH_H

#include "vscript_api.h"
#include "memory_api.h"
#include "asset_api.h"
#include "subsystem_manager.h"

#include <stdint.h>

/*
 * Shader domain for the visual-scripting core: the node set (sub-issue 2) and
 * the GLSL ES 3.00 codegen backend (sub-issue 3) of the #242 epic.  The graph
 * core stays target-agnostic; everything shader-specific lives here.
 */

/*
 * Shader type lattice.  These are the domain type tags carried on node ports;
 * the graph core only equality-checks them (no implicit promotion in v1 — use
 * an explicit construct node to change type).
 */
#define SG_TYPE_FLOAT     1u
#define SG_TYPE_VEC2      2u
#define SG_TYPE_VEC3      3u
#define SG_TYPE_VEC4      4u
#define SG_TYPE_SAMPLER2D 5u

/*
 * Per-node-type codegen metadata, hung off vscript_node_type.user.  tmpl is a
 * GLSL expression template: "$0".."$9" expand to the driving expression of the
 * matching input port, and "$p" expands to the node's param string.  A uniform
 * node emits a `uniform` declaration (its single output type + param name); the
 * output node assigns its input to the fragment colour.
 */
struct sg_node_meta {
	const char *tmpl;
	uint8_t     is_uniform;
	uint8_t     is_output;
};

/* Register the shader node set against the "vscript" registry. 0 / -1. */
int32_t shader_nodes_register(const struct vscript_api *vs);

/* The fixed v1 passthrough vertex shader (GLSL ES 3.00) source. */
const char *sg_passthrough_vertex(void);

/*
 * Emit a GLSL ES 3.00 fragment shader from a shader-target graph.  Returns a
 * freshly allocated, NUL-terminated source string (size, incl. the NUL, in
 * *out_size), or NULL if the target isn't shader, the graph is invalid, or it
 * lacks exactly one output node.  Caller frees via the engine allocator.
 */
char *sg_emit_fragment(const struct vscript_api *vs, vscript_graph_t g,
		       uint32_t *out_size);

/*
 * Compile a shader-target graph and deliver the fragment source as a derived
 * ASSET_TYPE_SHADER asset at out_path, tagged dialect=glsl_es_300 /
 * stage=fragment.  Returns the new asset id, or 0 on failure.
 */
uint32_t sg_compile_to_shader(const struct vscript_api *vs,
			      const struct asset_mut_api *mut,
			      vscript_graph_t g, const char *out_path);

/* Set the allocator used by the backend (WASM entry wiring). */
void sg_set_mem(const struct memory_api *mem);

/* Native accessor for the "shader_graph" service vtable (tests). */
const struct shader_graph_api *shader_graph_native_api(void);

/*
 * Cross-plugin shader-graph service, published as "shader_graph".  Handles are
 * opaque vscript graphs owned by the caller; the backend reads them through the
 * "vscript" api it resolved at entry.
 */
struct shader_graph_api {
	char     *(*emit_fragment)(vscript_graph_t g, uint32_t *out_size);
	uint32_t  (*compile)(vscript_graph_t g, const char *out_path);
	const char *(*vertex_source)(void);
};

#ifndef __EMSCRIPTEN__
void shader_graph_plugin_entry(struct subsystem_manager *mgr);
#endif

#endif /* SHADER_GRAPH_H */
