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
 * only outside a frame (init/shutdown, and the tick's pre-pass pipeline warm-up)
 * to create and destroy persistent resources (the pipelines and the primitive
 * mesh buffers) via the seam documented in fg.h. Per-frame GPU access happens
 * exclusively through the command context the graph lends into the forward pass
 * at execute time.
 *
 * A material may select its own shader (the optional trailing shader-ref in its
 * wire form — see resolve_material_shader). Each distinct selected shader gets
 * its own pipeline, compiled once and cached by shader asset id; forward_pass
 * binds the right one per draw. A material with no shader-ref (every legacy
 * 16-byte material, the built-in default) renders with the built-in scene
 * pipeline exactly as before.
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
static gpu_pipeline_t g_default_pso;  /* the built-in scene pipeline; fallback */
static gpu_buffer_t   g_ubo;         /* 2 * mat4: { view_proj, model } */
static gpu_buffer_t   g_material_ubo; /* the active material's std140 params */
static int            g_ready;

#define SCENE_UBO_FLOATS 32          /* view_proj[16] + model[16] */

/*
 * A material's wire form (v3): a uint32 shader-ref (asset id, first-class — a
 * material always names its shader) followed by the shader's Material uniform
 * block, std140-packed. The renderer stays schema-agnostic: it uploads the
 * param bytes to the Material UBO verbatim (the editor packs them to match the
 * shader), so what parameters a material has is entirely the shader's business.
 */
#define MATERIAL_HEADER_BYTES  sizeof(uint32_t)     /* the leading shader-ref */
#define MATERIAL_UBO_MAX       256                  /* std140 param bytes cap  */
#define MATERIAL_FALLBACK_BYTES 16                  /* one white vec4          */

/*
 * One compiled pipeline per shader a material selects, keyed by the shader's
 * asset id. Small and fixed: a session rarely uses more than a couple of custom
 * scene shaders, and an overflow just falls back to the built-in pipeline. The
 * built-in scene pipeline is pre-cached here under its own asset id, so the
 * default material (which names that shader) reuses it rather than recompiling.
 */
struct shader_pso {
	uint32_t       shader_ref;
	gpu_pipeline_t pso;   /* 0 = the shader failed to compile; use default */
};

#define SCENE_MAX_SHADER_PSOS 8
static struct shader_pso g_shader_psos[SCENE_MAX_SHADER_PSOS];
static uint32_t          g_shader_pso_count;

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
 * The world entity that owns the camera's eye (proof of life), or -1
 * when none is bound. A camera "behavior" is just a COMPONENT_SCRIPT entity
 * like any other — scene_renderer_tick copies its animated world_xform
 * position into g_cam.eye each frame, after the scene subsystem has ticked
 * scripts (see the "scene" before "scene_renderer" registration order in
 * engine.c). target/up/fov stay fixed for this proof of life; only eye moves.
 */
static int32_t g_camera_entity_id = -1;

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

/*
 * Resolve a material's shader — the leading uint32 asset id of its wire form.
 * Returns 0 (meaning "use the built-in scene pipeline") for a missing/short
 * material, a shader-ref of 0, or one that no longer resolves to a shader asset
 * (its shader was deleted or retyped) — so a dangling reference degrades to the
 * default rather than feeding non-shader bytes to pipeline_create.
 */
static uint32_t resolve_material_shader(uint32_t material_ref)
{
	const uint8_t    *bytes;
	uint32_t          size = 0;
	uint32_t          shader_ref;
	struct asset_info info;

	if (!material_ref || !g_asset)
		return 0;
	bytes = g_asset->get_data(material_ref, &size);
	if (!bytes || size < MATERIAL_HEADER_BYTES)
		return 0;
	memcpy(&shader_ref, bytes, sizeof(shader_ref));
	if (!shader_ref || !g_asset->find ||
	    g_asset->find(shader_ref, &info) != 0 ||
	    info.type != ASSET_TYPE_SHADER)
		return 0;
	return shader_ref;
}

/*
 * Borrow a material's std140 param bytes — everything after the shader-ref
 * header, i.e. the Material uniform block the editor packed to match the
 * shader. Sets *out_len (0 for a missing or header-only material) and returns
 * the pointer, or NULL when there are no params. The renderer never interprets
 * these bytes; it uploads them to the Material UBO as-is.
 */
static const uint8_t *material_params(uint32_t material_ref, uint32_t *out_len)
{
	const uint8_t *bytes;
	uint32_t       size = 0;

	*out_len = 0;
	if (!material_ref || !g_asset)
		return NULL;
	bytes = g_asset->get_data(material_ref, &size);
	if (!bytes || size <= MATERIAL_HEADER_BYTES)
		return NULL;
	*out_len = size - (uint32_t)MATERIAL_HEADER_BYTES;
	return bytes + MATERIAL_HEADER_BYTES;
}

/* The cache entry for a shader asset id, or NULL if it isn't compiled yet. */
static struct shader_pso *find_shader_pso(uint32_t shader_ref)
{
	uint32_t i;

	for (i = 0; i < g_shader_pso_count; i++) {
		if (g_shader_psos[i].shader_ref == shader_ref)
			return &g_shader_psos[i];
	}
	return NULL;
}

/*
 * Compile a pipeline from one shader asset's DSL source. All scene pipelines
 * share the same vertex layout and Camera/Material uniform blocks, so a custom
 * material shader must speak that same IO contract (a_pos/a_normal/a_uv0 in,
 * Camera at block 0, Material at block 1); only its shading changes.
 */
static gpu_pipeline_t create_pso(const struct gpu_api *gpu, const char *src)
{
	struct gpu_pipeline_desc pd;

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

	/*
	 * One shader asset holds both stages as DSL source; the backend lowers
	 * each stage to GLSL at pipeline-create and errors if a stage is absent.
	 */
	pd.vert.src     = src;
	pd.vert.stage   = GPU_SHADER_STAGE_VERTEX;
	pd.vert.dialect = GPU_SHADER_DIALECT_KRUDD;
	pd.frag.src     = src;
	pd.frag.stage   = GPU_SHADER_STAGE_FRAGMENT;
	pd.frag.dialect = GPU_SHADER_DIALECT_KRUDD;

	return gpu->pipeline_create(&pd);
}

/* Compile the built-in scene pipeline and pre-cache it under its asset id. */
static void build_pipeline(const struct gpu_api *gpu)
{
	const char *src      = shader_src_by_path("builtin://shader/scene");
	uint32_t    scene_id = asset_id_by_path("builtin://shader/scene");

	if (!src) {
		g_log->write(LOG_LEVEL_WARN,
			     "scene_renderer: scene shader asset unavailable");
		return;
	}
	g_default_pso = create_pso(gpu, src);
	/*
	 * Cache the built-in pipeline under the scene shader's id so a material
	 * that names it (the default) reuses this pipeline instead of compiling
	 * a second, identical one.
	 */
	if (g_default_pso && scene_id &&
	    g_shader_pso_count < SCENE_MAX_SHADER_PSOS) {
		g_shader_psos[g_shader_pso_count].shader_ref = scene_id;
		g_shader_psos[g_shader_pso_count].pso        = g_default_pso;
		g_shader_pso_count++;
	}
}

/*
 * Compile and cache the pipeline for a material-selected shader. A failed
 * compile is cached as a null pipeline so the shader is tried only once and
 * quietly falls back to the built-in scene pipeline rather than being retried
 * every tick. The cache is keyed by asset id, so binding a different shader
 * asset to a material switches pipelines live; a later in-place edit of the
 * same shader's source is not hot-reloaded (a follow-up).
 */
static void add_shader_pso(const struct gpu_api *gpu, uint32_t shader_ref)
{
	const char    *src;
	gpu_pipeline_t pso;

	if (g_shader_pso_count >= SCENE_MAX_SHADER_PSOS) {
		g_log->write(LOG_LEVEL_WARN,
			     "scene_renderer: shader pipeline cache full; "
			     "shader %u uses the default", shader_ref);
		return;
	}
	src = (const char *)g_asset->get_data(shader_ref, NULL);
	pso = src ? create_pso(gpu, src) : 0;
	if (!pso)
		g_log->write(LOG_LEVEL_WARN,
			     "scene_renderer: shader %u failed to compile; "
			     "using the default", shader_ref);

	g_shader_psos[g_shader_pso_count].shader_ref = shader_ref;
	g_shader_psos[g_shader_pso_count].pso        = pso;
	g_shader_pso_count++;
}

/* The pipeline a material's shader-ref selects, or the built-in default. */
static gpu_pipeline_t pso_for_shader(uint32_t shader_ref)
{
	struct shader_pso *e;

	if (shader_ref) {
		e = find_shader_pso(shader_ref);
		if (e && e->pso)
			return e->pso;
	}
	return g_default_pso;
}

/*
 * Ensure a compiled pipeline exists for every shader a live material selects,
 * so forward_pass never creates GPU resources mid-frame. Runs from the tick
 * (outside any pass), resolving the device through the same init/shutdown seam
 * without caching it. The per-tick work is bounded: it does at most one
 * pipeline_create per distinct new shader, and the walk is the same one the
 * forward pass already makes.
 */
static void ensure_shader_pipelines(void)
{
	const struct gpu_api *gpu = NULL;
	const struct world   *w;
	uint32_t              i;

	if (!g_scene || !g_asset)
		return;
	w = g_scene->get_world();
	if (!w)
		return;
	for (i = 0; i < w->count; i++) {
		uint32_t shader_ref;

		if (!w->alive[i] || !(w->mask[i] & COMPONENT_RENDER) ||
		    !(w->mask[i] & COMPONENT_MATERIAL))
			continue;
		shader_ref = resolve_material_shader(w->material_ref[i]);
		if (!shader_ref || find_shader_pso(shader_ref))
			continue;
		if (!gpu) {
			gpu = g_mgr ? subsystem_manager_get_api(g_mgr,
								"renderer")
				    : NULL;
			if (!gpu)
				return;
		}
		add_shader_pso(gpu, shader_ref);
	}
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
	static const struct {
		const char *path;
		float       pos[3];
		const char *script; /* behavior script to bind, or NULL */
		const char *name;   /* shown in the entity list */
	} DEMO[] = {
		{ "builtin://cube",    { -1.5f, 0.0f,  0.0f }, "builtin://script/spinner", "Cube"    },
		{ "builtin://sphere",  {  0.0f, 0.0f, -1.0f }, "builtin://script/bounce",  "Sphere"  },
		{ "builtin://pyramid", {  1.5f, 0.0f,  0.5f }, "builtin://script/wobble",  "Pyramid" },
	};
	const struct world *w;
	uint32_t            i;
	uint32_t            material;

	if (!g_scene || !g_scene->create_entity)
		return;
	w = g_scene->get_world();
	if (!w)
		return;
	for (i = 0; i < w->count; i++) {
		if (w->alive[i] && (w->mask[i] & COMPONENT_RENDER))
			return; /* scene already has renderables */
	}

	/*
	 * Every seeded entity carries the built-in default material, so the
	 * world scene never rests in the "no material" state — each renderable
	 * points at a real, inspectable material rather than going undrawn
	 * (forward_pass skips any entity with no COMPONENT_MATERIAL, which is
	 * how an entity keeps its mesh for picking/collision but stops
	 * drawing). The default is opaque white, so the seeded scene looks
	 * identical to before.
	 */
	material = asset_id_by_path("builtin://material/default");

	for (i = 0; i < (uint32_t)(sizeof(DEMO) / sizeof(DEMO[0])); i++) {
		struct transform t;
		int32_t          id;
		uint32_t         ref = asset_id_by_path(DEMO[i].path);

		if (!ref)
			continue;
		memset(&t, 0, sizeof(t));
		t.position[0] = DEMO[i].pos[0];
		t.position[1] = DEMO[i].pos[1];
		t.position[2] = DEMO[i].pos[2];
		t.rotation[3] = 1.0f;
		t.scale[0] = t.scale[1] = t.scale[2] = 1.0f;
		id = g_scene->create_entity(WORLD_NO_PARENT, &t, 0u, ref);
		if (id >= 0 && material && g_scene->set_material_ref)
			g_scene->set_material_ref(id, material);
		if (id >= 0 && g_scene->set_name)
			g_scene->set_name(id, DEMO[i].name);

		/*
		 * Bind a behavior script so the demo scene animates on load — the
		 * cube spins, the sphere bounces, the pyramid wobbles. Each is a
		 * built-in ASSET_TYPE_SCRIPT; skipped cleanly on an engine build
		 * without the script assets or the set_script_ref entry.
		 */
		if (id >= 0 && DEMO[i].script && g_scene->set_script_ref) {
			uint32_t script = asset_id_by_path(DEMO[i].script);

			if (script)
				g_scene->set_script_ref(id, script);
		}
	}

	/*
	 * The camera entity (proof of life): no mesh, no material, just a
	 * COMPONENT_SCRIPT entity bound to orbit-camera. scene_renderer_tick
	 * reads its world_xform position into g_cam.eye every frame.
	 */
	if (g_scene->set_script_ref) {
		uint32_t orbit_script = asset_id_by_path("builtin://script/orbit-camera");

		if (orbit_script) {
			struct transform ct;
			int32_t          cam_id;

			memset(&ct, 0, sizeof(ct));
			ct.rotation[3] = 1.0f;
			ct.scale[0] = ct.scale[1] = ct.scale[2] = 1.0f;
			cam_id = g_scene->create_entity(WORLD_NO_PARENT, &ct, 0u, 0u);
			if (cam_id >= 0) {
				g_scene->set_script_ref(cam_id, orbit_script);
				if (g_scene->set_name)
					g_scene->set_name(cam_id, "Camera");
			}
			g_camera_entity_id = cam_id;
		}
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

		/*
		 * Sized to the largest Material block the renderer will upload;
		 * each draw fills only its material's actual param bytes and
		 * binds exactly that range.
		 */
		bd.size = MATERIAL_UBO_MAX;
		g_material_ubo = gpu->buffer_create(&bd);
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
	gpu_pipeline_t        cur_pso = 0;
	int                   have_pso = 0;

	(void)userdata;
	if (!gpu || !g_scene)
		return;
	w = g_scene->get_world();
	if (!w)
		return;

	memcpy(&ubo[0], g_cam.view_proj.m, 16 * sizeof(float));

	for (i = 0; i < w->count; i++) {
		static const float            WHITE[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		struct gpu_draw_indexed_args  draw;
		struct mesh_gpu              *m;
		struct mat4                   model;
		unsigned char                 params[MATERIAL_UBO_MAX];
		const uint8_t                *pbytes;
		uint32_t                      plen;
		uint32_t                      mat_ref;
		gpu_pipeline_t                pso;

		if (!w->alive[i] || !(w->mask[i] & COMPONENT_RENDER))
			continue;
		if (!(w->mask[i] & COMPONENT_MATERIAL))
			continue; /* mesh stays pickable/collidable; just not drawn */
		m = find_mesh(w->render_ref[i]);
		if (!m)
			continue;

		mat_ref = w->material_ref[i];

		/*
		 * Select this material's pipeline (its shader, or the built-in
		 * default) and bind it only when it changes from the last draw,
		 * so a run of same-shader entities costs one set_pipeline. The
		 * have_pso flag forces the first bind even when the backend's
		 * pipeline handle is 0 (a valid value for the recording backend).
		 */
		pso = pso_for_shader(resolve_material_shader(mat_ref));
		if (!have_pso || pso != cur_pso) {
			gpu->cmd_set_pipeline(cmd, pso);
			cur_pso  = pso;
			have_pso = 1;
		}

		mat4_from_transform(&model, &w->world_xform[i]);
		memcpy(&ubo[16], model.m, 16 * sizeof(float));
		gpu->buffer_update(g_ubo, 0, ubo, (uint32_t)sizeof(ubo));

		gpu->cmd_bind_uniform_buffer(cmd, 0, g_ubo, 0,
					     (uint32_t)sizeof(ubo));

		/*
		 * Upload this material's std140 params verbatim (the shader's
		 * Material block, packed by the editor), binding exactly their
		 * length. A per-entity material-param override wins when present,
		 * so two entities sharing one material asset can draw with
		 * different colors; otherwise the shared material asset's own
		 * params are used. A param-less material (e.g. a legacy 16-byte
		 * material with nothing to report) falls back to a single white
		 * vec4 — the identity tint the scene shader expects.
		 */
		if (w->material_param_len[i] > 0) {
			pbytes = w->material_params[i];
			plen   = w->material_param_len[i];
		} else {
			pbytes = material_params(mat_ref, &plen);
		}
		if (plen > MATERIAL_UBO_MAX)
			plen = MATERIAL_UBO_MAX;
		if (!pbytes || plen == 0) {
			memcpy(params, WHITE, sizeof(WHITE));
			plen = MATERIAL_FALLBACK_BYTES;
		} else {
			memcpy(params, pbytes, plen);
		}
		gpu->buffer_update(g_material_ubo, 0, params, plen);
		gpu->cmd_bind_uniform_buffer(cmd, 1, g_material_ubo, 0, plen);

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

	/* Warm pipelines for any newly material-selected shader, off-frame. */
	ensure_shader_pipelines();

	if (g_camera_entity_id >= 0) {
		const struct world *w = g_scene->get_world();

		if (w && (uint32_t)g_camera_entity_id < w->count &&
		    w->alive[g_camera_entity_id]) {
			const struct transform *x =
				&w->world_xform[g_camera_entity_id];

			g_cam.eye[0] = x->position[0];
			g_cam.eye[1] = x->position[1];
			g_cam.eye[2] = x->position[2];
		}
	}

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
		if (g_material_ubo)
			gpu->buffer_destroy(g_material_ubo);
		/*
		 * The default pipeline is also cached (under the scene shader's
		 * id), so destroy the distinct cache entries first, then it once.
		 */
		for (i = 0; i < g_shader_pso_count; i++) {
			if (g_shader_psos[i].pso &&
			    g_shader_psos[i].pso != g_default_pso)
				gpu->pipeline_destroy(g_shader_psos[i].pso);
		}
		if (g_default_pso)
			gpu->pipeline_destroy(g_default_pso);
	}
	g_default_pso      = 0;
	g_shader_pso_count = 0;
	g_mesh_count       = 0;
	g_ready            = 0;
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
