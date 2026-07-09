/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "fg.h"
#include "renderer.h"
#include "entity_api.h"
#include "camera.h"
#include "camera_api.h"
#include "math_types.h"
#include "mesh.h"
#include "asset_api.h"
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef __EMSCRIPTEN__
#include "log.h"
static const struct log_api native_log = { log_write };
#endif

/*
 * Scene renderer — a pure frame-graph consumer (#172).
 *
 * It resolves "frame_graph", "scene" (entity_api) and "asset", and reads the
 * camera. It does NOT hold a persistent gpu_api pointer: the device is resolved
 * only inside init/shutdown to create and destroy persistent resources (the PSO
 * and the primitive mesh buffers) via the seam documented in fg.h. Per-frame
 * GPU access happens exclusively through the command context the graph lends
 * into the forward pass at execute time.
 */

#ifdef __EMSCRIPTEN__
static const struct log_api *g_log;
#else
static const struct log_api *g_log = &native_log;
#endif

static struct subsystem_manager *g_mgr;
static const struct fg_api      *g_fg_api;
static const struct entity_api  *g_scene;
static const struct asset_api   *g_asset;

/* Persistent resources, created once against the device (never per-frame). */
static gpu_pipeline_t g_pso;
static gpu_buffer_t   g_ubo;         /* 2 * mat4: { view_proj, model } */
static int            g_ready;

#define SCENE_UBO_FLOATS 32          /* view_proj[16] + model[16] */

/* One uploaded mesh, selected by an entity's render_ref (== asset id). */
struct mesh_gpu {
	gpu_buffer_t vbo;
	gpu_buffer_t ebo;
	uint32_t     render_ref;
	uint32_t     index_count;
};

#define SCENE_MAX_MESHES 8
static struct mesh_gpu g_meshes[SCENE_MAX_MESHES];
static uint32_t        g_mesh_count;

static struct camera g_cam;

/*
 * Camera service (#178) — lets editor overlays project world points with the
 * exact view·projection the forward pass draws with.  get_view_proj recomputes
 * on demand so a freshly set aspect is reflected immediately, without waiting
 * for the next tick.
 */
static void camera_get_view_proj(struct mat4 *out)
{
	if (!out)
		return;
	camera_update(&g_cam);
	*out = g_cam.view_proj;
}

static void camera_get_eye(float out[3])
{
	if (!out)
		return;
	out[0] = g_cam.eye[0];
	out[1] = g_cam.eye[1];
	out[2] = g_cam.eye[2];
}

static void camera_set_viewport(float width, float height)
{
	if (width > 0.0f && height > 0.0f)
		g_cam.aspect = width / height;
}

static const struct camera_api g_camera_api = {
	camera_get_view_proj,
	camera_get_eye,
	camera_set_viewport,
};

/* The built-in primitives that have uploadable geometry today. */
static const char *const PRIMITIVE_PATHS[] = {
	"builtin://cube",
	"builtin://sphere",
	"builtin://plane",
	"builtin://pyramid",
};

#define PRIMITIVE_COUNT \
	((uint32_t)(sizeof(PRIMITIVE_PATHS) / sizeof(PRIMITIVE_PATHS[0])))

/* Resolve a catalog path to its stable asset id, or 0 if absent. */
static uint32_t asset_id_by_path(const char *path)
{
	struct asset_info info;
	uint32_t          i, n;

	if (!g_asset)
		return 0;
	n = g_asset->count();
	for (i = 0; i < n; i++) {
		if (g_asset->info(i, &info) != 0)
			continue;
		if (strcmp(info.path, path) == 0)
			return info.id;
	}
	return 0;
}

/* Borrow a shader asset's NUL-terminated source by path (NULL if absent). */
static const char *shader_src_by_path(const char *path)
{
	uint32_t id = asset_id_by_path(path);

	if (!id)
		return NULL;
	return (const char *)g_asset->get_data(id, NULL);
}

static struct mesh_gpu *find_mesh(uint32_t render_ref)
{
	uint32_t i;

	for (i = 0; i < g_mesh_count; i++) {
		if (g_meshes[i].render_ref == render_ref)
			return &g_meshes[i];
	}
	return NULL;
}

/* Compile the entity pipeline from the seeded scene shader. */
static void build_pipeline(const struct gpu_api *gpu)
{
	struct gpu_pipeline_desc pd;
	const char              *src;

	/*
	 * One shader asset holds both stages as DSL source; the backend lowers
	 * each stage to GLSL at pipeline-create and errors if a stage is absent.
	 */
	src = shader_src_by_path("builtin://shader/scene");
	if (!src) {
		g_log->write(LOG_LEVEL_WARN,
			     "scene_renderer: scene shader asset unavailable");
		return;
	}

	memset(&pd, 0, sizeof(pd));
	pd.color_formats[0]   = GPU_FORMAT_RGBA8_UNORM;
	pd.color_format_count = 1;
	pd.depth_format       = GPU_FORMAT_DEPTH32_FLOAT;
	pd.topology           = GPU_TOPOLOGY_TRIANGLE_LIST;

	/* mesh_vertex: position(vec3) @0, normal(vec3) @12, uv0(vec2) @24. */
	pd.vertex_layout.attr_count = 3;
	pd.vertex_layout.stride     = (uint32_t)sizeof(struct mesh_vertex);
	pd.vertex_layout.attrs[0] = (struct gpu_vertex_attr){
		0, (uint32_t)offsetof(struct mesh_vertex, position),
		GPU_FORMAT_RGB32_FLOAT };
	pd.vertex_layout.attrs[1] = (struct gpu_vertex_attr){
		1, (uint32_t)offsetof(struct mesh_vertex, normal),
		GPU_FORMAT_RGB32_FLOAT };
	pd.vertex_layout.attrs[2] = (struct gpu_vertex_attr){
		2, (uint32_t)offsetof(struct mesh_vertex, uv0),
		GPU_FORMAT_RG32_FLOAT };

	pd.vert.src     = src;
	pd.vert.stage   = GPU_SHADER_STAGE_VERTEX;
	pd.vert.dialect = GPU_SHADER_DIALECT_KRUDD;
	pd.frag.src     = src;
	pd.frag.stage   = GPU_SHADER_STAGE_FRAGMENT;
	pd.frag.dialect = GPU_SHADER_DIALECT_KRUDD;

	g_pso = gpu->pipeline_create(&pd);
}

/* Upload one primitive's vertex/index buffers from its mesh blob. */
static void upload_mesh(const struct gpu_api *gpu, uint32_t render_ref)
{
	const struct mesh_blob *blob;
	struct gpu_buffer_desc  bd;
	struct mesh_gpu        *m;
	uint32_t                size = 0;

	if (g_mesh_count >= SCENE_MAX_MESHES)
		return;
	blob = g_asset->get_data(render_ref, &size);
	if (!blob || size < sizeof(*blob) || blob->magic != MESH_BLOB_MAGIC)
		return;

	m = &g_meshes[g_mesh_count];
	m->render_ref  = render_ref;
	m->index_count = blob->index_count;

	memset(&bd, 0, sizeof(bd));
	bd.usage        = GPU_BUFFER_USAGE_VERTEX;
	bd.size         = (size_t)blob->vertex_count * sizeof(struct mesh_vertex);
	bd.initial_data = mesh_blob_vertices(blob);
	m->vbo = gpu->buffer_create(&bd);

	bd.usage        = GPU_BUFFER_USAGE_INDEX;
	bd.size         = (size_t)blob->index_count * sizeof(uint16_t);
	bd.initial_data = mesh_blob_indices(blob);
	m->ebo = gpu->buffer_create(&bd);

	g_mesh_count++;
}

/*
 * Seed a small demo scene so the page shows content on load and exercises
 * depth-correct multi-entity rendering (#172 acceptance). Only runs when the
 * world holds no renderable entity yet, so it never clobbers a loaded scene.
 * Temporary — remove once scenes are authored/loaded routinely.
 */
static void seed_demo_scene(void)
{
	static const struct { const char *path; float pos[3]; } DEMO[] = {
		{ "builtin://cube",    { -1.5f, 0.0f,  0.0f } },
		{ "builtin://sphere",  {  0.0f, 0.0f, -1.0f } },
		{ "builtin://pyramid", {  1.5f, 0.0f,  0.5f } },
	};
	const struct world *w;
	uint32_t            i;

	if (!g_scene || !g_scene->create_entity)
		return;
	w = g_scene->get_world();
	if (!w)
		return;
	for (i = 0; i < w->count; i++) {
		if (w->alive[i] && (w->mask[i] & COMPONENT_RENDER))
			return; /* scene already has renderables */
	}

	for (i = 0; i < (uint32_t)(sizeof(DEMO) / sizeof(DEMO[0])); i++) {
		struct transform t;
		uint32_t         ref = asset_id_by_path(DEMO[i].path);

		if (!ref)
			continue;
		memset(&t, 0, sizeof(t));
		t.position[0] = DEMO[i].pos[0];
		t.position[1] = DEMO[i].pos[1];
		t.position[2] = DEMO[i].pos[2];
		t.rotation[3] = 1.0f;
		t.scale[0] = t.scale[1] = t.scale[2] = 1.0f;
		g_scene->create_entity(WORLD_NO_PARENT, &t, 0u, ref);
	}
}

static void scene_renderer_init(void)
{
	const struct gpu_api *gpu;
	uint32_t              i;

	/* Persistent-creation seam: resolve the device directly, outside a frame. */
	gpu = g_mgr ? subsystem_manager_get_api(g_mgr, "renderer") : NULL;
	if (!gpu) {
		g_log->write(LOG_LEVEL_WARN,
			     "scene_renderer: no renderer device; disabled");
		return;
	}

	build_pipeline(gpu);
	for (i = 0; i < PRIMITIVE_COUNT; i++) {
		uint32_t ref = asset_id_by_path(PRIMITIVE_PATHS[i]);

		if (ref)
			upload_mesh(gpu, ref);
	}

	{
		struct gpu_buffer_desc bd;

		memset(&bd, 0, sizeof(bd));
		bd.usage = GPU_BUFFER_USAGE_UNIFORM;
		bd.size  = SCENE_UBO_FLOATS * sizeof(float);
		g_ubo = gpu->buffer_create(&bd);
	}

	/* A fixed camera framing the unit primitives at the origin. */
	g_cam.eye[0]    = 2.5f; g_cam.eye[1]    = 2.0f; g_cam.eye[2]    = 4.0f;
	g_cam.target[0] = 0.0f; g_cam.target[1] = 0.0f; g_cam.target[2] = 0.0f;
	g_cam.up[0]     = 0.0f; g_cam.up[1]     = 1.0f; g_cam.up[2]     = 0.0f;
	g_cam.fov_y     = 0.8f;
	g_cam.aspect    = 1.6f;
	g_cam.near      = 0.1f;
	g_cam.far       = 100.0f;

	seed_demo_scene();

	g_ready = 1;
	g_log->write(LOG_LEVEL_INFO,
		     "scene_renderer: ready (%u primitive meshes)", g_mesh_count);
}

/*
 * The forward pass records real GPU commands on the lent context. It walks the
 * world the documented way, draws each COMPONENT_RENDER entity from its
 * world_xform and render_ref, and lets the context go. It never caches the
 * device, the command buffer, or resolves "renderer".
 */
static void forward_pass(struct fg_pass_ctx *ctx, void *userdata)
{
	const struct gpu_api *gpu = fg_ctx_gpu(ctx);
	gpu_cmd_buf_t         cmd = fg_ctx_cmd(ctx);
	const struct world   *w;
	uint32_t              i;
	float                 ubo[SCENE_UBO_FLOATS];

	(void)userdata;
	if (!gpu || !g_scene)
		return;
	w = g_scene->get_world();
	if (!w)
		return;

	memcpy(&ubo[0], g_cam.view_proj.m, 16 * sizeof(float));
	gpu->cmd_set_pipeline(cmd, g_pso);

	for (i = 0; i < w->count; i++) {
		struct gpu_draw_indexed_args draw;
		struct mesh_gpu             *m;
		struct mat4                  model;

		if (!w->alive[i] || !(w->mask[i] & COMPONENT_RENDER))
			continue;
		m = find_mesh(w->render_ref[i]);
		if (!m)
			continue;

		mat4_from_transform(&model, &w->world_xform[i]);
		memcpy(&ubo[16], model.m, 16 * sizeof(float));
		gpu->buffer_update(g_ubo, 0, ubo, (uint32_t)sizeof(ubo));

		gpu->cmd_bind_uniform_buffer(cmd, 0, g_ubo, 0,
					     (uint32_t)sizeof(ubo));
		gpu->cmd_bind_vertex_buffer(cmd, 0, m->vbo, 0);
		gpu->cmd_bind_index_buffer(cmd, m->ebo, 0,
					   GPU_INDEX_FORMAT_UINT16);

		memset(&draw, 0, sizeof(draw));
		draw.index_count    = m->index_count;
		draw.instance_count = 1;
		gpu->cmd_draw_indexed(cmd, &draw);
	}
}

static void scene_renderer_tick(void)
{
	static const float CLEAR[4] = { 0.10f, 0.11f, 0.13f, 1.0f };
	struct fg          *fg;
	fg_resource_t       bb;
	fg_pass_t           pass;

	if (!g_ready || !g_fg_api || !g_scene)
		return;

	camera_update(&g_cam);

	fg = g_fg_api->create();
	if (!fg)
		return;
	bb   = g_fg_api->import_backbuffer(fg);
	pass = g_fg_api->pass_declare(fg, "forward", NULL, 0, &bb, 1);
	if (pass) {
		g_fg_api->pass_set_color_clear(pass, 0, CLEAR);
		g_fg_api->pass_set_depth_clear(pass, 1.0f);
		g_fg_api->pass_set_execute(pass, forward_pass, NULL);
		g_fg_api->compile(fg);
		g_fg_api->execute(fg);
	}
	g_fg_api->destroy(fg);
}

static void scene_renderer_shutdown(void)
{
	const struct gpu_api *gpu;
	uint32_t              i;

	gpu = g_mgr ? subsystem_manager_get_api(g_mgr, "renderer") : NULL;
	if (gpu) {
		for (i = 0; i < g_mesh_count; i++) {
			gpu->buffer_destroy(g_meshes[i].vbo);
			gpu->buffer_destroy(g_meshes[i].ebo);
		}
		if (g_ubo)
			gpu->buffer_destroy(g_ubo);
		if (g_pso)
			gpu->pipeline_destroy(g_pso);
	}
	g_mesh_count = 0;
	g_ready      = 0;
	g_log->write(LOG_LEVEL_INFO, "scene_renderer: shutdown");
}

static const struct subsystem desc = {
	.name     = "scene_renderer",
	.init     = scene_renderer_init,
	.tick     = scene_renderer_tick,
	.shutdown = scene_renderer_shutdown,
};

/*
 * The camera is published as its own read-only "camera" subsystem rather than
 * hung off scene_renderer's entry, so consumers resolve it by intent. It has no
 * lifecycle of its own — the renderer owns and updates the camera state.
 */
static const struct subsystem camera_desc = {
	.name = "camera",
	.api  = &g_camera_api,
};

void scene_renderer_plugin_entry(struct subsystem_manager *mgr)
{
	g_mgr = mgr;
#ifdef __EMSCRIPTEN__
	g_log = subsystem_manager_get_api(mgr, "log");
#endif
	g_fg_api = subsystem_manager_get_api(mgr, "frame_graph");
	g_scene  = subsystem_manager_get_api(mgr, "scene");
	g_asset  = subsystem_manager_get_api(mgr, "asset");
	subsystem_manager_register(mgr, &camera_desc);
	subsystem_manager_register(mgr, &desc);
}
