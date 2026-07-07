/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SHADER_GRAPH_API_H
#define SHADER_GRAPH_API_H

#include "vscript_api.h"

#include <stdint.h>

/*
 * Cross-plugin shader-graph service, published as "shader_graph":
 *
 *   const struct shader_graph_api *sg =
 *           subsystem_manager_get_api(mgr, "shader_graph");
 *   uint32_t shader_id = sg->compile(graph, "project://shader/gen.frag");
 *
 * Handles are opaque vscript graphs owned by the caller (target must be
 * VSCRIPT_TARGET_SHADER); the backend reads them through the "vscript" api it
 * resolved at entry.  The GLSL ES 3.00 backend lives behind this vtable so the
 * editor (sub-issue 4) never links the codegen directly.
 */
struct shader_graph_api {
	/*
	 * Emit GLSL ES 3.00 fragment source for a shader-target graph.
	 * Returns a freshly allocated, NUL-terminated string (size incl. the
	 * NUL in *out_size), or NULL on failure.  Caller frees via the engine
	 * allocator.
	 */
	char       *(*emit_fragment)(vscript_graph_t g, uint32_t *out_size);
	/*
	 * Compile a graph and deliver the fragment as a derived,
	 * dialect/stage-tagged ASSET_TYPE_SHADER asset at out_path.  Returns
	 * the new asset id, or 0 on failure.
	 */
	uint32_t    (*compile)(vscript_graph_t g, const char *out_path);
	/* The fixed v1 passthrough vertex shader (GLSL ES 3.00) source. */
	const char *(*vertex_source)(void);
};

#endif /* SHADER_GRAPH_API_H */
