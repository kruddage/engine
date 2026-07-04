/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "shader_graph.h"
#include "vscript_api.h"
#include "asset_api.h"
#include "memory_api.h"
#include "subsystem_manager.h"

#include <stdint.h>

/*
 * Shader-graph plugin entry: contributes the shader node set to the "vscript"
 * registry and publishes the GLSL backend as the "shader_graph" service the
 * editor (sub-issue 4) drives to compile a graph into a shader asset.
 */

static const struct vscript_api   *g_vs;
static const struct asset_mut_api *g_mut;

static char *svc_emit_fragment(vscript_graph_t g, uint32_t *out_size)
{
	return sg_emit_fragment(g_vs, g, out_size);
}

static uint32_t svc_compile(vscript_graph_t g, const char *out_path)
{
	return sg_compile_to_shader(g_vs, g_mut, g, out_path);
}

static const char *svc_vertex(void)
{
	return sg_passthrough_vertex();
}

static const struct shader_graph_api g_api = {
	.emit_fragment = svc_emit_fragment,
	.compile       = svc_compile,
	.vertex_source = svc_vertex,
};

const struct shader_graph_api *shader_graph_native_api(void)
{
	return &g_api;
}

static const struct subsystem shader_graph_desc = {
	.name = "shader_graph",
	.api  = &g_api,
};

#ifdef __EMSCRIPTEN__
void plugin_entry(struct subsystem_manager *mgr)
#else
void shader_graph_plugin_entry(struct subsystem_manager *mgr)
#endif
{
#ifdef __EMSCRIPTEN__
	sg_set_mem(subsystem_manager_get_api(mgr, "memory"));
#endif
	g_vs  = subsystem_manager_get_api(mgr, "vscript");
	g_mut = subsystem_manager_get_api(mgr, "asset_mut");
	if (g_vs)
		shader_nodes_register(g_vs);
	subsystem_manager_register(mgr, &shader_graph_desc);
}
