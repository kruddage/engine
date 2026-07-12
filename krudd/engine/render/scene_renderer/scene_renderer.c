/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "fg.h"
#include "renderer.h"
#include "entity_api.h"
#include "camera.h"
#include "camera_api.h"
#include "preview_api.h"
#include "math_types.h"

#include <math.h>
#include "mesh.h"
#include "mesh_script.h"
#include "texture.h"
#include "texture_script.h"
#include "script.h"
#include "asset_api.h"
#include "memory_api.h"
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef __EMSCRIPTEN__
#include "log.h"
#include "memory.h"
static const struct log_api    native_log = { log_write };
static const struct memory_api native_mem = {
	mem_alloc, mem_alloc_zero, mem_free,
	mem_pool_create, mem_pool_alloc, mem_pool_free, mem_pool_destroy,
};
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
static const struct log_api    *g_log;
static const struct memory_api *g_mem;
#else
static const struct log_api    *g_log = &native_log;
static const struct memory_api *g_mem = &native_mem;
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
	uint32_t       mat_block_size; /* std140 bytes of the shader's Material block */
};

#define SCENE_MAX_SHADER_PSOS 8
static struct shader_pso g_shader_psos[SCENE_MAX_SHADER_PSOS];
static uint32_t          g_shader_pso_count;

/*
 * One uploaded mesh, keyed by (render_ref, param bytes). A param-less mesh keys
 * on its asset id alone (param_len 0), exactly as before; a parameterized mesh
 * keys on its asset id AND the entity's override bytes, so two entities sharing
 * one mesh asset but overriding it differently each get their own geometry — the
 * geometry analogue of the per-draw material UBO. `used` is the per-frame
 * mark-and-sweep flag ensure_meshes drives to evict combos no live entity needs
 * anymore (a slider settling on a new size frees the old one's buffers).
 */
struct mesh_gpu {
	gpu_buffer_t vbo;
	gpu_buffer_t ebo;
	uint32_t     render_ref;
	uint32_t     index_count;
	uint32_t     param_len;               /* 0 = param-less (asset-id key only) */
	uint8_t      params[WORLD_MESH_PARAM_CAP];
	int          used;                    /* mark/sweep: needed this frame */
};

/*
 * Cache capacity across all live (render_ref, params) combos, not just distinct
 * mesh assets — a handful of parameterized meshes at a few sizes each still fits.
 * ensure_meshes evicts combos no live entity references, so the cache tracks what
 * the scene actually draws rather than growing without bound; an overflow just
 * leaves the newest combo un-cached (it falls back to not drawing that entity)
 * rather than corrupting anything.
 */
#define SCENE_MAX_MESHES 32
static struct mesh_gpu g_meshes[SCENE_MAX_MESHES];
static uint32_t        g_mesh_count;

/*
 * One uploaded, baked procedural texture, keyed by (texture_ref, width, height,
 * gen-param bytes). Mirrors mesh_gpu: two materials that bake the same texture at
 * the same size and params share one GPU texture; a different size or param set
 * gets its own. `used` drives ensure_textures' mark/sweep, freeing combos no live
 * material references anymore (a resolution change frees the old one's texture).
 */
#define SCENE_TEX_PARAM_CAP 128
struct texture_gpu {
	gpu_texture_t tex;
	uint32_t      texture_ref;
	uint32_t      width;
	uint32_t      height;
	uint32_t      param_len;
	uint8_t       params[SCENE_TEX_PARAM_CAP];
	int           used;
};

#define SCENE_MAX_TEXTURES 16
static struct texture_gpu g_textures[SCENE_MAX_TEXTURES];
static uint32_t           g_texture_count;

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

/*
 * Meshes are uploaded lazily, not from a fixed built-in list: ensure_meshes()
 * (run each tick, off-frame) resolves whatever (render_ref, params) combos the
 * live world actually references and evicts the rest. Every ASSET_TYPE_MESH
 * asset's bytes are (mesh NAME [(params ...)] (generate () ...)) Scheme source —
 * there is no hardcoded C mesh generator and no separate "compiled blob" asset
 * shape — so upload_mesh() below always compiles through mesh_script_generate()
 * before GPU-uploading the result.
 */

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

/*
 * The cached mesh for (render_ref, params), or NULL when that exact combo is not
 * resident. A match needs the same asset id and the same override bytes, so a
 * shared mesh at two different param sets resolves to two distinct entries.
 */
static struct mesh_gpu *find_mesh(uint32_t render_ref,
				  const uint8_t *params, uint32_t plen)
{
	uint32_t i;

	if (plen > WORLD_MESH_PARAM_CAP)
		plen = WORLD_MESH_PARAM_CAP;
	for (i = 0; i < g_mesh_count; i++) {
		struct mesh_gpu *m = &g_meshes[i];

		if (m->render_ref != render_ref || m->param_len != plen)
			continue;
		if (plen == 0 || memcmp(m->params, params, plen) == 0)
			return m;
	}
	return NULL;
}

/* The entity's mesh-param override bytes for the cache key, or NULL/0 when it
 * has none (the mesh then generates on its declared defaults). */
static const uint8_t *entity_mesh_params(const struct world *w, uint32_t i,
					 uint32_t *plen)
{
	if (w->mesh_param_len[i] > 0) {
		*plen = w->mesh_param_len[i];
		return w->mesh_params[i];
	}
	*plen = 0;
	return NULL;
}

/* The entity's texture-param override bytes for the bake, or NULL/0 when it has
 * none (the texture then bakes on its declared defaults). Two entities sharing
 * one material bake its texture with different params purely from this. */
static const uint8_t *entity_texture_params(const struct world *w, uint32_t i,
					    uint32_t *plen)
{
	if (w->texture_param_len[i] > 0) {
		*plen = w->texture_param_len[i];
		return w->texture_params[i];
	}
	*plen = 0;
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
 * The std140 byte size of a shader's Material block, from the same introspection
 * the editor packs against. Cached per shader in the pso entry so the material's
 * UBO/texture-trailer split (see material_texture) costs no per-draw s7 call;
 * 0 for a shader with no Material block or an unparseable source.
 */
static uint32_t shader_material_block_size(const char *src)
{
	uint32_t total = 0;

	if (src)
		script_shader_material_params(src, NULL, 0, &total);
	return total;
}

/* The cached Material-block size for a shader asset id, or 0 if not compiled. */
static uint32_t material_block_size_of(uint32_t shader_ref)
{
	struct shader_pso *e = find_shader_pso(shader_ref);

	return e ? e->mat_block_size : 0;
}

/*
 * Parse a material's optional texture slot — the trailer after the shader-ref
 * and the shader's std140 Material block:
 *   [shader-ref u32][block: block_size][tex-ref u32][width u32][height u32][params...]
 * Returns 1 and fills the out params when a texture slot is present, 0 otherwise.
 * The block size comes from the material's shader (cached in the pso), so a
 * material with no trailer (size == header + block) reports no texture, exactly
 * as before this trailer existed — the backward-compatible split. A shader whose
 * block size isn't known yet (0) is treated as "no texture" so ambiguous bytes
 * are never misread as a slot.
 */
static int material_texture(uint32_t material_ref, uint32_t shader_ref,
			    uint32_t *out_tex, uint32_t *out_w, uint32_t *out_h,
			    const uint8_t **out_params, uint32_t *out_plen)
{
	const uint8_t *bytes;
	uint32_t       size = 0;
	uint32_t       block, off;

	if (!material_ref || !g_asset)
		return 0;
	block = material_block_size_of(shader_ref);
	if (block == 0)
		return 0;
	bytes = g_asset->get_data(material_ref, &size);
	if (!bytes)
		return 0;
	off = (uint32_t)MATERIAL_HEADER_BYTES + block;
	if (size < off + 3u * sizeof(uint32_t))
		return 0; /* no trailer */
	memcpy(out_tex, bytes + off,      sizeof(uint32_t));
	memcpy(out_w,   bytes + off + 4u, sizeof(uint32_t));
	memcpy(out_h,   bytes + off + 8u, sizeof(uint32_t));
	if (*out_tex == 0)
		return 0;
	*out_params = bytes + off + 12u;
	*out_plen   = size - (off + 12u);
	return 1;
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
		g_shader_psos[g_shader_pso_count].mat_block_size =
			shader_material_block_size(src);
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
	g_shader_psos[g_shader_pso_count].mat_block_size =
		shader_material_block_size(src);
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

/*
 * Upload the (render_ref, params) combo's vertex/index buffers into a fresh
 * cache slot and return it (NULL when the cache is full, the asset has no
 * source, or the generator yields nothing). Every ASSET_TYPE_MESH asset's bytes
 * are (mesh NAME [(params ...)] (generate () ...)) Scheme source, so this always
 * compiles through mesh_script_generate() — now with the entity's param override
 * — into a transient local blob before GPU-uploading it. There is no stored
 * "compiled blob" shape to borrow instead.
 */
static struct mesh_gpu *upload_mesh(const struct gpu_api *gpu,
				    uint32_t render_ref,
				    const uint8_t *params, uint32_t plen)
{
	const struct mesh_blob *blob;
	struct gpu_buffer_desc  bd;
	struct mesh_gpu        *m;
	const char             *src;

	if (g_mesh_count >= SCENE_MAX_MESHES)
		return NULL;
	src = (const char *)g_asset->get_data(render_ref, NULL);
	if (!src)
		return NULL;
	blob = mesh_script_generate(src, params, plen, g_mem, NULL);
	if (!blob)
		return NULL;

	if (plen > WORLD_MESH_PARAM_CAP)
		plen = WORLD_MESH_PARAM_CAP;
	m = &g_meshes[g_mesh_count];
	m->render_ref  = render_ref;
	m->index_count = blob->index_count;
	m->param_len   = plen;
	if (plen)
		memcpy(m->params, params, plen);

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

	g_mem->free((void *)blob);
	return m;
}

/*
 * Reconcile the mesh cache with the world, off-frame (from the tick, like
 * ensure_shader_pipelines) so forward_pass never creates or destroys GPU
 * resources mid-pass. Mark every cached combo unused, then walk each renderable
 * entity: ensure its (render_ref, params) combo is resident (uploading on a
 * miss) and mark it used. Finally sweep — destroy and compact out every combo no
 * live entity touched this frame, so a param edit that lands on a new size frees
 * the buffers the old size held. Bounded work: at most one upload per new combo,
 * over the same walk the forward pass already makes.
 */
static void ensure_meshes(void)
{
	const struct gpu_api *gpu = NULL;
	const struct world   *w;
	uint32_t              i;

	if (!g_scene || !g_asset)
		return;
	w = g_scene->get_world();
	if (!w)
		return;

	for (i = 0; i < g_mesh_count; i++)
		g_meshes[i].used = 0;

	for (i = 0; i < w->count; i++) {
		const uint8_t   *pbytes;
		uint32_t         plen;
		struct mesh_gpu *m;

		if (!w->alive[i] || !(w->mask[i] & COMPONENT_RENDER)
		    || !w->render_ref[i])
			continue;
		pbytes = entity_mesh_params(w, i, &plen);
		m = find_mesh(w->render_ref[i], pbytes, plen);
		if (!m) {
			if (!gpu) {
				gpu = g_mgr ? subsystem_manager_get_api(g_mgr,
									"renderer")
					    : NULL;
				if (!gpu)
					return;
			}
			m = upload_mesh(gpu, w->render_ref[i], pbytes, plen);
		}
		if (m)
			m->used = 1;
	}

	for (i = 0; i < g_mesh_count;) {
		if (g_meshes[i].used) {
			i++;
			continue;
		}
		if (!gpu)
			gpu = g_mgr ? subsystem_manager_get_api(g_mgr,
								"renderer")
				    : NULL;
		if (gpu) {
			gpu->buffer_destroy(g_meshes[i].vbo);
			gpu->buffer_destroy(g_meshes[i].ebo);
		}
		g_meshes[i] = g_meshes[--g_mesh_count];
	}
}

/*
 * The cached texture for (texture_ref, width, height, params), or NULL when that
 * exact combo is not resident. A match needs the same asset id, the same bake
 * size, and the same gen-param bytes, so a shared texture at two resolutions (or
 * two param sets) resolves to two distinct entries — the pixel analogue of the
 * (render_ref, params) mesh key.
 */
static struct texture_gpu *find_texture(uint32_t texture_ref, uint32_t width,
					uint32_t height, const uint8_t *params,
					uint32_t plen)
{
	uint32_t i;

	if (plen > SCENE_TEX_PARAM_CAP)
		plen = SCENE_TEX_PARAM_CAP;
	for (i = 0; i < g_texture_count; i++) {
		struct texture_gpu *t = &g_textures[i];

		if (t->texture_ref != texture_ref || t->width != width ||
		    t->height != height || t->param_len != plen)
			continue;
		if (plen == 0 || memcmp(t->params, params, plen) == 0)
			return t;
	}
	return NULL;
}

/*
 * Bake (texture_ref, params) at width x height via texture_script_generate and
 * upload the RGBA8 result as a sampled GPU texture with a full mip chain,
 * returning the new cache slot (NULL when the cache is full, the asset has no
 * source, or the generator yields nothing). Every ASSET_TYPE_TEXTURE asset's
 * bytes are (texture NAME (shade (u v) ...)) Scheme source, so this compiles the
 * pixels on demand — there is no stored image to borrow instead.
 */
static struct texture_gpu *upload_texture(const struct gpu_api *gpu,
					  uint32_t texture_ref, uint32_t width,
					  uint32_t height, const uint8_t *params,
					  uint32_t plen)
{
	const struct texture_blob *blob;
	struct gpu_texture_desc    td;
	struct texture_gpu        *t;
	const char                *src;

	if (g_texture_count >= SCENE_MAX_TEXTURES)
		return NULL;
	src = (const char *)g_asset->get_data(texture_ref, NULL);
	if (!src)
		return NULL;
	blob = texture_script_generate(src, params, plen, width, height,
				       g_mem, NULL);
	if (!blob)
		return NULL;

	memset(&td, 0, sizeof(td));
	td.format        = GPU_FORMAT_RGBA8_UNORM;
	td.width         = blob->width;
	td.height        = blob->height;
	td.mip_levels    = 1;
	td.sample_count  = 1;
	td.initial_data  = texture_blob_pixels(blob);
	td.generate_mips = 1;

	t = &g_textures[g_texture_count];
	t->tex         = gpu->texture_create(&td);
	t->texture_ref = texture_ref;
	t->width       = width;
	t->height      = height;
	if (plen > SCENE_TEX_PARAM_CAP)
		plen = SCENE_TEX_PARAM_CAP;
	t->param_len = plen;
	if (plen)
		memcpy(t->params, params, plen);
	g_texture_count++;

	g_mem->free((void *)blob);
	return t;
}

/*
 * Reconcile the texture cache with the world, off-frame (from the tick, after
 * ensure_shader_pipelines so a material's shader block size is known) so
 * forward_pass never bakes or uploads a texture mid-pass. Mark/sweep like
 * ensure_meshes: mark every cached combo unused, ensure each live material's
 * texture slot is resident (baking on a miss) and mark it used, then destroy the
 * combos no live material references — so a resolution change frees the old
 * texture. Materials with no texture slot are simply skipped.
 */
static void ensure_textures(void)
{
	const struct gpu_api *gpu = NULL;
	const struct world   *w;
	uint32_t              i;

	if (!g_scene || !g_asset)
		return;
	w = g_scene->get_world();
	if (!w)
		return;

	for (i = 0; i < g_texture_count; i++)
		g_textures[i].used = 0;

	for (i = 0; i < w->count; i++) {
		uint32_t       tex_ref, tw, th, plen = 0, shader_ref;
		const uint8_t *params = NULL;
		struct texture_gpu *t;

		if (!w->alive[i] || !(w->mask[i] & COMPONENT_RENDER) ||
		    !(w->mask[i] & COMPONENT_MATERIAL))
			continue;
		shader_ref = resolve_material_shader(w->material_ref[i]);
		if (!material_texture(w->material_ref[i], shader_ref,
				      &tex_ref, &tw, &th, &params, &plen))
			continue;
		/*
		 * A per-entity texture-param override wins over the material's own
		 * trailer params, so two entities sharing one material bake the
		 * texture with different generation params (a checker at scale 8
		 * vs 16) — the pixel twin of the per-entity mesh-param override.
		 */
		{
			uint32_t       eplen = 0;
			const uint8_t *ep    = entity_texture_params(w, i, &eplen);

			if (ep) {
				params = ep;
				plen   = eplen;
			}
		}
		t = find_texture(tex_ref, tw, th, params, plen);
		if (!t) {
			if (!gpu) {
				gpu = g_mgr ? subsystem_manager_get_api(g_mgr,
									"renderer")
					    : NULL;
				if (!gpu)
					return;
			}
			t = upload_texture(gpu, tex_ref, tw, th, params, plen);
		}
		if (t)
			t->used = 1;
	}

	for (i = 0; i < g_texture_count;) {
		if (g_textures[i].used) {
			i++;
			continue;
		}
		if (!gpu)
			gpu = g_mgr ? subsystem_manager_get_api(g_mgr,
								"renderer")
				    : NULL;
		if (gpu)
			gpu->texture_destroy(g_textures[i].tex);
		g_textures[i] = g_textures[--g_texture_count];
	}
}

/* ------------------------------------------------------------------ */
/* Mesh preview — render one mesh, shaded, into an offscreen target     */
/* ------------------------------------------------------------------ */
/*
 * The editor's mesh inspector wants a lit thumbnail of an authored mesh. Rather
 * than approximate it, render the real geometry through the real scene pipeline
 * into an offscreen color+depth target and hand back the color texture's native
 * handle for ImGui to composite (see preview_api.h). This drives the gpu device
 * directly — a legitimate off-frame render like init's resource creation, not a
 * frame-graph pass — so it never touches the per-frame forward pass.
 *
 * State is a single reusable slot: one pair of render targets sized to the last
 * requested edge, and one uploaded mesh keyed by asset id. Both are kept apart
 * from the world caches (g_meshes/g_textures) so ensure_meshes' mark/sweep never
 * evicts the preview geometry (no live entity references it).
 */
#define PREVIEW_MAX_RES 512

static gpu_texture_t g_prev_color;      /* RGBA8 target; 0 until first preview */
static gpu_texture_t g_prev_depth;      /* depth target, same edge as color    */
static uint32_t      g_prev_res;        /* edge of the current targets         */
static gpu_buffer_t  g_prev_vbo;
static gpu_buffer_t  g_prev_ebo;
static uint32_t      g_prev_mesh_ref;   /* asset id the buffers hold (0 = none) */
static uint32_t      g_prev_index_count;
static float         g_prev_center[3];  /* AABB center of the cached mesh       */
static float         g_prev_radius;     /* bounding radius, for camera framing  */

/*
 * (Re)allocate the color+depth preview targets at `res` when the requested edge
 * changes. Returns 0 on allocation failure. The old pair is freed first so a
 * resolution change doesn't leak.
 */
static int preview_ensure_targets(const struct gpu_api *gpu, uint32_t res)
{
	struct gpu_texture_desc td;

	if (g_prev_color && g_prev_res == res)
		return 1;
	if (g_prev_color) {
		gpu->texture_destroy(g_prev_color);
		g_prev_color = 0;
	}
	if (g_prev_depth) {
		gpu->texture_destroy(g_prev_depth);
		g_prev_depth = 0;
	}

	memset(&td, 0, sizeof(td));
	td.format       = GPU_FORMAT_RGBA8_UNORM;
	td.width        = res;
	td.height       = res;
	td.mip_levels   = 1;
	td.sample_count = 1;
	g_prev_color = gpu->texture_create(&td);

	td.format    = GPU_FORMAT_DEPTH32_FLOAT;
	g_prev_depth = gpu->texture_create(&td);

	if (!g_prev_color || !g_prev_depth) {
		if (g_prev_color) gpu->texture_destroy(g_prev_color);
		if (g_prev_depth) gpu->texture_destroy(g_prev_depth);
		g_prev_color = g_prev_depth = 0;
		g_prev_res   = 0;
		return 0;
	}
	g_prev_res = res;
	return 1;
}

/*
 * (Re)generate and upload the preview mesh when the inspected asset changes,
 * caching its geometry and the AABB (center + radius) the camera frames against.
 * Param-less generation (the mesh's declared defaults) is what the inspector
 * shows. Returns 0 when the asset has no source or the generator yields nothing.
 */
static int preview_ensure_mesh(const struct gpu_api *gpu, uint32_t mesh_ref)
{
	const struct mesh_blob   *blob;
	const struct mesh_vertex *vtx;
	struct gpu_buffer_desc    bd;
	const char               *src;
	float                     lo[3], hi[3];
	uint32_t                  i;

	if (g_prev_mesh_ref == mesh_ref && g_prev_vbo)
		return 1;

	if (g_prev_vbo) { gpu->buffer_destroy(g_prev_vbo); g_prev_vbo = 0; }
	if (g_prev_ebo) { gpu->buffer_destroy(g_prev_ebo); g_prev_ebo = 0; }
	g_prev_mesh_ref = 0;

	src = (const char *)g_asset->get_data(mesh_ref, NULL);
	if (!src)
		return 0;
	blob = mesh_script_generate(src, NULL, 0, g_mem, NULL);
	if (!blob)
		return 0;

	vtx = mesh_blob_vertices(blob);
	if (blob->vertex_count == 0) {
		g_mem->free((void *)blob);
		return 0;
	}
	lo[0] = hi[0] = vtx[0].position[0];
	lo[1] = hi[1] = vtx[0].position[1];
	lo[2] = hi[2] = vtx[0].position[2];
	for (i = 1; i < blob->vertex_count; i++) {
		uint32_t k;

		for (k = 0; k < 3; k++) {
			float v = vtx[i].position[k];

			if (v < lo[k]) lo[k] = v;
			if (v > hi[k]) hi[k] = v;
		}
	}
	{
		float r2 = 0.0f;
		float d[3];

		for (i = 0; i < 3; i++)
			g_prev_center[i] = 0.5f * (lo[i] + hi[i]);
		d[0] = 0.5f * (hi[0] - lo[0]);
		d[1] = 0.5f * (hi[1] - lo[1]);
		d[2] = 0.5f * (hi[2] - lo[2]);
		r2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
		g_prev_radius = r2 > 0.0f ? sqrtf(r2) : 1.0f;
	}

	memset(&bd, 0, sizeof(bd));
	bd.usage        = GPU_BUFFER_USAGE_VERTEX;
	bd.size         = (size_t)blob->vertex_count * sizeof(struct mesh_vertex);
	bd.initial_data = vtx;
	g_prev_vbo = gpu->buffer_create(&bd);

	bd.usage        = GPU_BUFFER_USAGE_INDEX;
	bd.size         = (size_t)blob->index_count * sizeof(uint16_t);
	bd.initial_data = mesh_blob_indices(blob);
	g_prev_ebo = gpu->buffer_create(&bd);

	g_prev_index_count = blob->index_count;
	g_prev_mesh_ref    = mesh_ref;
	g_mem->free((void *)blob);
	return (g_prev_vbo && g_prev_ebo) ? 1 : 0;
}

/*
 * Render mesh_ref shaded with material_ref into the preview target and return
 * the color texture's native handle (see preview_api.h). Drives the device
 * directly for a single off-frame pass, reusing the scene pipeline, the shared
 * Camera/Material UBOs and the mesh upload path so the thumbnail matches how the
 * mesh draws in-scene. Returns 0 on any failure.
 */
static uint32_t scene_preview_render_mesh(uint32_t mesh_ref,
					  uint32_t material_ref,
					  uint32_t res, float yaw)
{
	static const float          BG[4] = { 0.10f, 0.11f, 0.13f, 1.0f };
	static const float          WHITE[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	const struct gpu_api        *gpu;
	struct gpu_render_pass_desc  rp;
	struct gpu_draw_indexed_args draw;
	struct camera                cam;
	struct transform             mt;
	struct mat4                  model;
	gpu_cmd_buf_t                cmd;
	gpu_pipeline_t               pso;
	uint32_t                     shader_ref;
	float                        ubo[SCENE_UBO_FLOATS];
	unsigned char                params[MATERIAL_UBO_MAX];
	const uint8_t               *pbytes;
	uint32_t                     plen = 0;
	float                        dist;

	if (!g_ready || !g_scene || !g_asset || !mesh_ref || res == 0)
		return 0;
	if (res > PREVIEW_MAX_RES)
		res = PREVIEW_MAX_RES;

	gpu = g_mgr ? subsystem_manager_get_api(g_mgr, "renderer") : NULL;
	if (!gpu)
		return 0;

	if (!preview_ensure_targets(gpu, res))
		return 0;
	if (!preview_ensure_mesh(gpu, mesh_ref))
		return 0;

	/*
	 * A material_ref of 0 means "the built-in default", so a pure-geometry
	 * preview needs no material picked. Compile its shader's pipeline on
	 * first sight, off-frame, exactly as ensure_shader_pipelines does.
	 */
	if (material_ref == 0)
		material_ref = asset_id_by_path("builtin://material/default");
	shader_ref = resolve_material_shader(material_ref);
	if (shader_ref && !find_shader_pso(shader_ref))
		add_shader_pso(gpu, shader_ref);
	pso = pso_for_shader(shader_ref);

	/* Frame the mesh's bounds from a fixed three-quarter view. */
	cam.fov_y  = 0.6f;
	cam.aspect = 1.0f;
	cam.near   = 0.05f;
	cam.far    = 100.0f;
	cam.up[0]  = 0.0f; cam.up[1] = 1.0f; cam.up[2] = 0.0f;
	cam.target[0] = g_prev_center[0];
	cam.target[1] = g_prev_center[1];
	cam.target[2] = g_prev_center[2];
	dist = (g_prev_radius / sinf(cam.fov_y * 0.5f)) * 1.25f;
	/* Direction (0.5, 0.45, 1.0), normalized, scaled to the framing distance. */
	{
		float dir[3] = { 0.5f, 0.45f, 1.0f };
		float len = sqrtf(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);

		cam.eye[0] = cam.target[0] + dir[0] / len * dist;
		cam.eye[1] = cam.target[1] + dir[1] / len * dist;
		cam.eye[2] = cam.target[2] + dir[2] / len * dist;
	}
	camera_update(&cam);

	/* model: a yaw about +Y so the caller can spin the preview. */
	memset(&mt, 0, sizeof(mt));
	mt.position[0] = mt.position[1] = mt.position[2] = 0.0f;
	mt.rotation[0] = 0.0f;
	mt.rotation[1] = sinf(yaw * 0.5f);
	mt.rotation[2] = 0.0f;
	mt.rotation[3] = cosf(yaw * 0.5f);
	mt.scale[0] = mt.scale[1] = mt.scale[2] = 1.0f;
	mat4_from_transform(&model, &mt);

	memcpy(&ubo[0],  cam.view_proj.m, 16 * sizeof(float));
	memcpy(&ubo[16], model.m,         16 * sizeof(float));

	/* Material params: the shader's Material block, or a white tint fallback. */
	pbytes = material_params(material_ref, &plen);
	if (pbytes && shader_ref) {
		uint32_t block = material_block_size_of(shader_ref);

		if (block && plen > block)
			plen = block;
	}
	if (plen > MATERIAL_UBO_MAX)
		plen = MATERIAL_UBO_MAX;
	if (!pbytes || plen == 0) {
		memcpy(params, WHITE, sizeof(WHITE));
		plen = MATERIAL_FALLBACK_BYTES;
	} else {
		memcpy(params, pbytes, plen);
	}

	cmd = gpu->cmd_buf_begin();

	memset(&rp, 0, sizeof(rp));
	rp.color_count       = 1;
	rp.color[0].texture  = g_prev_color;
	rp.color[0].load_op  = GPU_LOAD_OP_CLEAR;
	rp.color[0].store_op = GPU_STORE_OP_STORE;
	rp.color[0].clear[0] = BG[0];
	rp.color[0].clear[1] = BG[1];
	rp.color[0].clear[2] = BG[2];
	rp.color[0].clear[3] = BG[3];
	rp.depth             = g_prev_depth;
	rp.depth_load_op     = GPU_LOAD_OP_CLEAR;
	rp.depth_store_op    = GPU_STORE_OP_STORE;
	rp.clear_depth       = 1.0f;
	gpu->cmd_begin_render_pass(cmd, &rp);

	gpu->cmd_set_pipeline(cmd, pso);
	gpu->buffer_update(g_ubo, 0, ubo, (uint32_t)sizeof(ubo));
	gpu->cmd_bind_uniform_buffer(cmd, 0, g_ubo, 0, (uint32_t)sizeof(ubo));
	gpu->buffer_update(g_material_ubo, 0, params, plen);
	gpu->cmd_bind_uniform_buffer(cmd, 1, g_material_ubo, 0, plen);
	gpu->cmd_bind_vertex_buffer(cmd, 0, g_prev_vbo, 0);
	gpu->cmd_bind_index_buffer(cmd, g_prev_ebo, 0, GPU_INDEX_FORMAT_UINT16);

	memset(&draw, 0, sizeof(draw));
	draw.index_count    = g_prev_index_count;
	draw.instance_count = 1;
	gpu->cmd_draw_indexed(cmd, &draw);

	gpu->cmd_end_render_pass(cmd);
	gpu->cmd_buf_submit(cmd);

	return gpu->texture_native_handle(g_prev_color);
}

/* Free the preview's targets and mesh buffers — called from shutdown. */
static void preview_release(const struct gpu_api *gpu)
{
	if (!gpu)
		return;
	if (g_prev_color) gpu->texture_destroy(g_prev_color);
	if (g_prev_depth) gpu->texture_destroy(g_prev_depth);
	if (g_prev_vbo)   gpu->buffer_destroy(g_prev_vbo);
	if (g_prev_ebo)   gpu->buffer_destroy(g_prev_ebo);
	g_prev_color = g_prev_depth = 0;
	g_prev_vbo = g_prev_ebo = 0;
	g_prev_res = g_prev_mesh_ref = g_prev_index_count = 0;
}

static const struct preview_api g_preview_api = {
	scene_preview_render_mesh,
};

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
		int         has_mesh_params; /* seed a mesh-param override? */
		float       whd[3];          /* box width/height/depth when it does */
	} DEMO[] = {
		{ "builtin://mesh/box",     { -1.5f, 0.0f,  0.0f }, "builtin://script/spinner", "Box",     0, { 0.0f, 0.0f, 0.0f } },
		{ "builtin://mesh/sphere",  {  0.0f, 0.0f, -1.0f }, "builtin://script/bounce",  "Sphere",  0, { 0.0f, 0.0f, 0.0f } },
		{ "builtin://mesh/pyramid", {  1.5f, 0.0f,  0.5f }, "builtin://script/wobble",  "Pyramid", 0, { 0.0f, 0.0f, 0.0f } },
		{ "builtin://mesh/grid",    { 0.0f, -0.5f, 1.5f }, NULL, "Grid",                0, { 0.0f, 0.0f, 0.0f } },
		/*
		 * Two entities on ONE mesh asset (builtin://mesh/box), drawing at
		 * different sizes purely from their per-entity mesh-param overrides —
		 * the geometry twin of the two same-material entities that draw in
		 * different colors. This is the whole point made visible on load.
		 */
		{ "builtin://mesh/box",     { -3.2f, 0.0f,  0.0f }, NULL, "Tall Box",            1, { 0.6f, 2.0f, 0.6f } },
		{ "builtin://mesh/box",     {  3.2f, 0.0f,  0.0f }, NULL, "Wide Box",            1, { 2.2f, 0.5f, 1.0f } },
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
		{
			/*
			 * The grid floor wears the built-in checker material — a
			 * procedural texture baked and sampled on load, the proof
			 * this whole path renders. Every other entity keeps the
			 * opaque-white default, unchanged.
			 */
			uint32_t emat = material;

			if (strcmp(DEMO[i].path, "builtin://mesh/grid") == 0) {
				uint32_t c =
					asset_id_by_path("builtin://material/checker");

				if (c)
					emat = c;
			}
			if (id >= 0 && emat && g_scene->set_material_ref)
				g_scene->set_material_ref(id, emat);
		}
		if (id >= 0 && g_scene->set_name)
			g_scene->set_name(id, DEMO[i].name);

		/*
		 * Seed a mesh-param override so the two box entities, sharing one
		 * mesh asset, generate distinct geometry (a tall box and a wide
		 * one). Skipped cleanly on a build without the set_mesh_params entry.
		 */
		if (id >= 0 && DEMO[i].has_mesh_params && g_scene->set_mesh_params)
			g_scene->set_mesh_params(id, (const uint8_t *)DEMO[i].whd,
						 sizeof(DEMO[i].whd));

		/*
		 * Bind a behavior script so the demo scene animates on load — the
		 * box spins, the sphere bounces, the pyramid wobbles. Each is a
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
	 * Two boxes sharing ONE material (the built-in checker), each baking its
	 * texture at a different scale purely from a per-entity texture-param
	 * override — the pixel twin of the two boxes on one mesh above, and the
	 * whole point of per-entity texture params made visible on load. The
	 * override is one tight-packed float (the checker's leading `scale` param);
	 * skipped cleanly on a build without the checker material or the
	 * set_texture_params entry.
	 */
	if (g_scene->set_texture_params) {
		static const struct {
			float       pos[3];
			float       scale;
			const char *name;
		} TEX[] = {
			{ { -1.6f, 1.9f, 0.0f },  3.0f, "Checker x3"  },
			{ {  1.6f, 1.9f, 0.0f }, 12.0f, "Checker x12" },
		};
		uint32_t box     = asset_id_by_path("builtin://mesh/box");
		uint32_t checker = asset_id_by_path("builtin://material/checker");
		uint32_t k;

		for (k = 0; box && checker &&
		     k < (uint32_t)(sizeof(TEX) / sizeof(TEX[0])); k++) {
			struct transform t;
			int32_t          id;

			memset(&t, 0, sizeof(t));
			t.position[0] = TEX[k].pos[0];
			t.position[1] = TEX[k].pos[1];
			t.position[2] = TEX[k].pos[2];
			t.rotation[3] = 1.0f;
			t.scale[0] = t.scale[1] = t.scale[2] = 0.8f;
			id = g_scene->create_entity(WORLD_NO_PARENT, &t, 0u, box);
			if (id < 0)
				continue;
			g_scene->set_material_ref(id, checker);
			g_scene->set_texture_params(id, (const uint8_t *)&TEX[k].scale,
						    sizeof(TEX[k].scale));
			if (g_scene->set_name)
				g_scene->set_name(id, TEX[k].name);
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

	/* Persistent-creation seam: resolve the device directly, outside a frame. */
	gpu = g_mgr ? subsystem_manager_get_api(g_mgr, "renderer") : NULL;
	if (!gpu) {
		g_log->write(LOG_LEVEL_WARN,
			     "scene_renderer: no renderer device; disabled");
		return;
	}

	build_pipeline(gpu);
	/* Mesh buffers are created lazily by ensure_meshes() on the first tick,
	 * once the seeded/loaded world exists — not from a fixed list here. */

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
		     "scene_renderer: ready (meshes uploaded lazily per tick)");
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
		uint32_t                      shader_ref;
		gpu_pipeline_t                pso;

		if (!w->alive[i] || !(w->mask[i] & COMPONENT_RENDER))
			continue;
		if (!(w->mask[i] & COMPONENT_MATERIAL))
			continue; /* mesh stays pickable/collidable; just not drawn */
		{
			const uint8_t *mp;
			uint32_t       mplen;

			mp = entity_mesh_params(w, i, &mplen);
			m  = find_mesh(w->render_ref[i], mp, mplen);
		}
		if (!m)
			continue;

		mat_ref = w->material_ref[i];
		shader_ref = resolve_material_shader(mat_ref);

		/*
		 * Select this material's pipeline (its shader, or the built-in
		 * default) and bind it only when it changes from the last draw,
		 * so a run of same-shader entities costs one set_pipeline. The
		 * have_pso flag forces the first bind even when the backend's
		 * pipeline handle is 0 (a valid value for the recording backend).
		 */
		pso = pso_for_shader(shader_ref);
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
			uint32_t block;

			pbytes = material_params(mat_ref, &plen);
			/*
			 * A textured material's bytes are the Material block
			 * followed by the texture trailer; upload only the block
			 * to the UBO (the trailer is bound as a texture below),
			 * so the sampler slot never leaks into the uniform data.
			 */
			block = material_block_size_of(shader_ref);
			if (block && plen > block)
				plen = block;
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

		/*
		 * Bind this material's baked procedural texture, if it names one,
		 * to the albedo unit the scene-textured shader samples. The combo
		 * is already resident (ensure_textures ran off-frame); a miss just
		 * skips the bind and the sampler reads whatever unit 0 holds.
		 */
		{
			uint32_t       tex_ref, tw, th, tplen = 0;
			const uint8_t *tparams = NULL;

			if (material_texture(mat_ref, shader_ref, &tex_ref, &tw,
					     &th, &tparams, &tplen)) {
				uint32_t       eplen = 0;
				const uint8_t *ep    =
					entity_texture_params(w, i, &eplen);
				struct texture_gpu *t;

				if (ep) {
					tparams = ep;
					tplen   = eplen;
				}
				t = find_texture(tex_ref, tw, th, tparams, tplen);
				if (t)
					gpu->cmd_bind_texture(cmd, 0, t->tex);
			}
		}

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

	/* Warm pipelines and mesh buffers for the live world, off-frame, so the
	 * forward pass never creates or destroys a GPU resource mid-pass. */
	ensure_shader_pipelines();
	ensure_textures();
	ensure_meshes();

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
		preview_release(gpu);
		for (i = 0; i < g_mesh_count; i++) {
			gpu->buffer_destroy(g_meshes[i].vbo);
			gpu->buffer_destroy(g_meshes[i].ebo);
		}
		for (i = 0; i < g_texture_count; i++)
			gpu->texture_destroy(g_textures[i].tex);
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
	g_texture_count    = 0;
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

/*
 * The mesh-preview service (see preview_api.h) — published like the camera as
 * its own read-only subsystem so the editor resolves it by intent. No lifecycle
 * of its own; the renderer owns the preview targets and frees them at shutdown.
 */
static const struct subsystem preview_desc = {
	.name = "mesh_preview",
	.api  = &g_preview_api,
};

void scene_renderer_plugin_entry(struct subsystem_manager *mgr)
{
	g_mgr = mgr;
#ifdef __EMSCRIPTEN__
	g_log = subsystem_manager_get_api(mgr, "log");
	g_mem = subsystem_manager_get_api(mgr, "memory");
#endif
	g_fg_api = subsystem_manager_get_api(mgr, "frame_graph");
	g_scene  = subsystem_manager_get_api(mgr, "scene");
	g_asset  = subsystem_manager_get_api(mgr, "asset");
	subsystem_manager_register(mgr, &camera_desc);
	subsystem_manager_register(mgr, &preview_desc);
	subsystem_manager_register(mgr, &desc);
}
