/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "renderer.h"
#include <null/renderer_null.h>
#include <frame_graph/fg.h>
#include <abi/entity_api.h>
#include <abi/camera_api.h>
#include <scene_renderer/scene_view.h>
#include <abi/asset_api.h>
#include <math/math_types.h>
#include <asset/mesh.h>
#include <asset/builtin_mesh_scripts.h>
#include <core/subsystem_manager.h>
#include <memory/memory.h>
#include <core/script.h>
#include <entity/world.h>

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void renderer_null_plugin_entry(struct subsystem_manager *mgr);
void fg_plugin_entry(struct subsystem_manager *mgr);
void scene_renderer_plugin_entry(struct subsystem_manager *mgr);

/* ------------------------------------------------------------------ */
/* A minimal fake asset catalog: the built-in meshes and the one scene */
/* shader (DSL, both stages), addressed by stable id (i + 1).          */
/* ------------------------------------------------------------------ */

static const char *const CAT_PATHS[] = {
	"builtin://mesh/box",
	"builtin://mesh/sphere",
	"builtin://mesh/plane",
	"builtin://mesh/pyramid",
	"builtin://shader/scene-textured",
	"material/red",
	"shader/alt",
	"material/blue",
	"shader/emissive",
	"material/glow",
	"material/dark",
};
#define CAT_COUNT ((uint32_t)(sizeof(CAT_PATHS) / sizeof(CAT_PATHS[0])))

/* Per-index asset type, addressed the same way as the id (id == index + 1). */
static const int32_t CAT_TYPES[CAT_COUNT] = {
	ASSET_TYPE_MESH,     /* id 1 box     */
	ASSET_TYPE_MESH,     /* id 2 sphere  */
	ASSET_TYPE_MESH,     /* id 3 plane   */
	ASSET_TYPE_MESH,     /* id 4 pyramid */
	ASSET_TYPE_SHADER,   /* id 5 scene-textured shader */
	ASSET_TYPE_MATERIAL, /* id 6 material/red (legacy 16-byte, no shader) */
	ASSET_TYPE_SHADER,   /* id 7 shader/alt   */
	ASSET_TYPE_MATERIAL, /* id 8 material/blue (v2: base_color + shader 7) */
	ASSET_TYPE_SHADER,   /* id 9  shader/emissive (has an emissive field) */
	ASSET_TYPE_MATERIAL, /* id 10 material/glow (emissive driven past 1)  */
	ASSET_TYPE_MATERIAL, /* id 11 material/dark (same shader, emissive 0) */
};

/* ids 1-4 serve (mesh ...) source text — upload_mesh compiles it through the
 * real s7 image, exactly as the shipped asset plugin seeds these same four
 * built-ins (see asset_plugin.c). */
static const char *const CAT_MESH_SCRIPTS[4] = {
	BOX_MESH_SCRIPT_SRC, SPHERE_MESH_SCRIPT_SRC,
	PLANE_MESH_SCRIPT_SRC, PYRAMID_MESH_SCRIPT_SRC,
};

/* The null backend records rather than compiles, so any DSL bytes suffice. */
static const char *const SHADER_SRC =
	"(shader scene-textured (vertex (set position (vec4 0.0 0.0 0.0 1.0)))"
	" (fragment (set c (vec4 1.0 1.0 1.0 1.0))))";
static const char *const SHADER_ALT_SRC =
	"(shader alt (vertex (set position (vec4 0.0 0.0 0.0 1.0)))"
	" (fragment (set c (vec4 0.0 0.0 1.0 1.0))))";
/*
 * A v3 material's wire form: a leading shader-ref (asset id) then the shader's
 * std140 Material block — here one vec4 base_color. Red (id 6) names the
 * scene-textured shader (id 5); blue (id 8) names shader/alt (id 7).
 */
static const struct {
	uint32_t shader_ref;
	float    base_color[4];
} MATERIAL_RED  = { 5u, { 1.0f, 0.0f, 0.0f, 1.0f } };
static const struct {
	uint32_t shader_ref;
	float    base_color[4];
} MATERIAL_BLUE = { 7u, { 0.0f, 0.0f, 1.0f, 1.0f } };

/*
 * A shader that declares an `emissive` colour field, the way the shipped pbr
 * shader does — the bloom bright pass finds it by introspecting this Material
 * block, so the test exercises the real lookup rather than a hardcoded offset.
 * std140 puts base_color at 0, metallic at 16, roughness at 20 and the vec3
 * emissive at 32 (its own 16-byte slot), for a 48-byte block.
 */
static const char *const SHADER_EMISSIVE_SRC =
	"(shader emissive-probe\n"
	"  (inputs (a_pos vec3 (location 0)))\n"
	"  (uniforms\n"
	"    (Camera (block 0) (layout std140)\n"
	"      (view_proj mat4)\n"
	"      (model     mat4))\n"
	"    (Material (block 1) (layout std140)\n"
	"      (base_color vec4  (edit color))\n"
	"      (metallic   float (edit range 0.0 1.0))\n"
	"      (roughness  float (edit range 0.0 1.0))\n"
	"      (emissive   vec3  (edit color))))\n"
	"  (targets (frag_color vec4 (location 0)))\n"
	"  (vertex   (set position (* view_proj model (vec4 a_pos 1.0))))\n"
	"  (fragment (set frag_color (vec4 1.0 1.0 1.0 1.0))))\n";

/*
 * Two materials on that shader, differing only in emissive: glow is driven past
 * 1 the way training's grid lines and ducks' crosshair are, dark leaves it
 * at 0. block[] is the std140 Material block laid out as above, so
 * block[8..10] is the emissive vec3 at byte 32.
 */
#define EMISSIVE_BLOCK_FLOATS 12
static const struct {
	uint32_t shader_ref;
	float    block[EMISSIVE_BLOCK_FLOATS];
} MATERIAL_GLOW = { 9u, { 0.10f, 0.01f, 0.01f, 1.0f,   /* base_color      */
			  0.0f, 0.5f, 0.0f, 0.0f,      /* metallic, rough */
			  2.4f, 0.12f, 0.10f, 0.0f } };/* emissive        */
static const struct {
	uint32_t shader_ref;
	float    block[EMISSIVE_BLOCK_FLOATS];
} MATERIAL_DARK = { 9u, { 0.10f, 0.01f, 0.01f, 1.0f,
			  0.0f, 0.5f, 0.0f, 0.0f,
			  0.0f, 0.0f, 0.0f, 0.0f } };

static uint32_t cat_count(void) { return CAT_COUNT; }

static int32_t cat_info(uint32_t i, struct asset_info *out)
{
	if (i >= CAT_COUNT || !out)
		return -1;
	memset(out, 0, sizeof(*out));
	out->path      = CAT_PATHS[i];
	out->id        = i + 1;
	out->type      = CAT_TYPES[i];
	out->state     = 1;
	out->read_only = 1;
	return 0;
}

/* Resolve a stable id (index + 1) to its snapshot; id 0 is "none". */
static int32_t cat_find(uint32_t id, struct asset_info *out)
{
	if (id == 0 || id > CAT_COUNT)
		return -1;
	return cat_info(id - 1, out);
}

static const void *cat_get_data(uint32_t id, uint32_t *out_size)
{
	if (id >= 1 && id <= 4) {
		const char *src = CAT_MESH_SCRIPTS[id - 1];

		if (out_size)
			*out_size = (uint32_t)strlen(src) + 1;
		return src;
	}
	if (id == 5) {
		if (out_size)
			*out_size = (uint32_t)strlen(SHADER_SRC) + 1;
		return SHADER_SRC;
	}
	if (id == 6) {
		if (out_size)
			*out_size = (uint32_t)sizeof(MATERIAL_RED);
		return &MATERIAL_RED;
	}
	if (id == 7) {
		if (out_size)
			*out_size = (uint32_t)strlen(SHADER_ALT_SRC) + 1;
		return SHADER_ALT_SRC;
	}
	if (id == 8) {
		if (out_size)
			*out_size = (uint32_t)sizeof(MATERIAL_BLUE);
		return &MATERIAL_BLUE;
	}
	if (id == 9) {
		if (out_size)
			*out_size = (uint32_t)strlen(SHADER_EMISSIVE_SRC) + 1;
		return SHADER_EMISSIVE_SRC;
	}
	if (id == 10) {
		if (out_size)
			*out_size = (uint32_t)sizeof(MATERIAL_GLOW);
		return &MATERIAL_GLOW;
	}
	if (id == 11) {
		if (out_size)
			*out_size = (uint32_t)sizeof(MATERIAL_DARK);
		return &MATERIAL_DARK;
	}
	return NULL;
}

static const struct asset_api FAKE_ASSET = {
	.count    = cat_count,
	.info     = cat_info,
	.find     = cat_find,
	.get_data = cat_get_data,
};

/* ------------------------------------------------------------------ */
/* A fake "scene" exposing a hand-built world.                         */
/* ------------------------------------------------------------------ */

static struct world       g_world;
static const struct world *fake_get_world(void) { return &g_world; }

/*
 * The scene selection, which is what the outline pass follows on the native
 * harness (OUTLINE_FOLLOWS_SELECTION). -1 — nothing selected — for every test
 * but the one that wants the outline chain declared, so the graphs every other
 * assertion here counts are unaffected by this existing at all.
 */
static int32_t g_selected = -1;
static int32_t fake_get_selected(void) { return g_selected; }

static const struct entity_api FAKE_SCENE = {
	.get_world    = fake_get_world,
	.get_selected = fake_get_selected,
};

static void set_identity_xform(struct transform *t, float x, float y, float z)
{
	memset(t, 0, sizeof(*t));
	t->position[0] = x;
	t->position[1] = y;
	t->position[2] = z;
	t->rotation[3] = 1.0f;
	t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
}

/* Two live box entities, one tombstoned render entity, one non-render. */
static void build_world(uint32_t box_ref)
{
	memset(&g_world, 0, sizeof(g_world));
	g_world.count = 4;

	/* Entity 0 carries a material (base_color red) and draws. Entity 1 has
	 * a mesh but no material — it must be skipped from drawing, though its
	 * mesh stays live (COMPONENT_RENDER) for picking/collision. */
	g_world.alive[0]        = 1;
	g_world.mask[0]         = COMPONENT_RENDER | COMPONENT_MATERIAL;
	g_world.render_ref[0]   = box_ref;
	g_world.material_ref[0] = 6u;
	set_identity_xform(&g_world.world_xform[0], -1.0f, 0.0f, 0.0f);

	g_world.alive[1]      = 1;
	g_world.mask[1]       = COMPONENT_RENDER;
	g_world.render_ref[1] = box_ref;
	set_identity_xform(&g_world.world_xform[1], 1.0f, 0.0f, 0.0f);

	g_world.alive[2]      = 0; /* tombstoned — must be skipped */
	g_world.mask[2]       = COMPONENT_RENDER;
	g_world.render_ref[2] = box_ref;
	set_identity_xform(&g_world.world_xform[2], 0.0f, 0.0f, 0.0f);

	g_world.alive[3]      = 1;
	g_world.mask[3]       = 0; /* no COMPONENT_RENDER — must be skipped */
	set_identity_xform(&g_world.world_xform[3], 0.0f, 2.0f, 0.0f);
}

static uint32_t count_draws(uint32_t *out_index_count)
{
	const struct gpu_call_record *log;
	uint32_t count, i, draws = 0;

	log = renderer_null_get_log(&count);
	for (i = 0; i < count; i++) {
		if (log[i].type != GPU_CALL_CMD_DRAW_INDEXED)
			continue;
		draws++;
		if (out_index_count)
			*out_index_count = log[i].args.cmd_draw_indexed.index_count;
	}
	return draws;
}

/* Every draw binds the material UBO at slot 1 (a param-less material falls
 * back to a white tint; an entity with no material at all doesn't draw, so
 * it never reaches this bind). */
static uint32_t count_material_binds(void)
{
	const struct gpu_call_record *log;
	uint32_t count, i, binds = 0;

	log = renderer_null_get_log(&count);
	for (i = 0; i < count; i++) {
		if (log[i].type == GPU_CALL_CMD_BIND_UNIFORM_BUFFER &&
		    log[i].args.cmd_bind_uniform_buffer.slot == 1 &&
		    log[i].args.cmd_bind_uniform_buffer.size == 4 * sizeof(float))
			binds++;
	}
	return binds;
}

/* The forward pass binds the directional-light Sun UBO once at slot 2 (24 floats
 * — light_dir + light_radiance + light_view_proj, std140), for the pbr shader;
 * count those binds so the light path is exercised even though this world has no
 * pbr material. */
static uint32_t count_light_binds(void)
{
	const struct gpu_call_record *log;
	uint32_t count, i, binds = 0;

	log = renderer_null_get_log(&count);
	for (i = 0; i < count; i++) {
		if (log[i].type == GPU_CALL_CMD_BIND_UNIFORM_BUFFER &&
		    log[i].args.cmd_bind_uniform_buffer.slot == 2 &&
		    log[i].args.cmd_bind_uniform_buffer.size == 24 * sizeof(float))
			binds++;
	}
	return binds;
}

static uint32_t count_calls(enum gpu_call_type type)
{
	const struct gpu_call_record *log;
	uint32_t count, i, hits = 0;

	log = renderer_null_get_log(&count);
	for (i = 0; i < count; i++)
		if (log[i].type == type)
			hits++;
	return hits;
}

/* Transient textures the frame graph created at exactly WxH — the shape of the
 * per-frame targets a path declares, which is how a pass's presence is read
 * here without the graph having to report its own pass list. */
static uint32_t count_texture_creates(uint32_t w, uint32_t h)
{
	const struct gpu_call_record *log;
	uint32_t count, i, hits = 0;

	log = renderer_null_get_log(&count);
	for (i = 0; i < count; i++) {
		if (log[i].type == GPU_CALL_TEXTURE_CREATE &&
		    log[i].args.texture_create.width == w &&
		    log[i].args.texture_create.height == h)
			hits++;
	}
	return hits;
}

/*
 * A frame's declared graph, reduced to the shape two frames can be compared on:
 * every transient the graph created, in order, with its format, size and sample
 * count, and every render pass, in order, with its attachment count and whether
 * colour 0 carried a resolve target. That is exactly "same passes, same
 * attachment/sample state" in a form a test can assert equality of.
 */
#define GRAPH_MAX_TEX    16
#define GRAPH_MAX_PASSES 16

struct graph_shape {
	struct {
		uint32_t format, width, height, samples;
	}        tex[GRAPH_MAX_TEX];
	uint32_t tex_count;
	struct {
		uint32_t color_count;
		int      color0_resolve;
	}        pass[GRAPH_MAX_PASSES];
	uint32_t pass_count;
};

static void capture_graph_shape(struct graph_shape *out)
{
	const struct gpu_call_record *log;
	uint32_t count, i;

	memset(out, 0, sizeof(*out));
	log = renderer_null_get_log(&count);
	for (i = 0; i < count; i++) {
		if (log[i].type == GPU_CALL_TEXTURE_CREATE &&
		    out->tex_count < GRAPH_MAX_TEX) {
			uint32_t t = out->tex_count++;

			out->tex[t].format  = log[i].args.texture_create.format;
			out->tex[t].width   = log[i].args.texture_create.width;
			out->tex[t].height  = log[i].args.texture_create.height;
			out->tex[t].samples =
				log[i].args.texture_create.sample_count;
		} else if (log[i].type == GPU_CALL_CMD_BEGIN_RENDER_PASS &&
			   out->pass_count < GRAPH_MAX_PASSES) {
			uint32_t pi = out->pass_count++;
			const struct gpu_call_record *r = &log[i];

			out->pass[pi].color_count =
				r->args.cmd_begin_render_pass.color_count;
			out->pass[pi].color0_resolve =
				r->args.cmd_begin_render_pass.color0_resolve;
		}
	}
}

/* The sample count the graph declared its first WxH colour target with, or 0
 * when it declared none. */
static uint32_t color_target_samples(const struct graph_shape *g,
				     uint32_t w, uint32_t h)
{
	uint32_t i;

	for (i = 0; i < g->tex_count; i++) {
		if (g->tex[i].width == w && g->tex[i].height == h &&
		    g->tex[i].format != GPU_FORMAT_DEPTH32_FLOAT)
			return g->tex[i].samples;
	}
	return 0;
}

/* Passes that carried a colour-0 resolve target — the multisampled forward
 * pass, and nothing else. Counted rather than indexed because the graph
 * topo-sorts its passes and the bright pass, which reads nothing, is free to
 * land before the forward pass. */
static uint32_t count_resolving_passes(const struct graph_shape *g)
{
	uint32_t i, hits = 0;

	for (i = 0; i < g->pass_count; i++)
		if (g->pass[i].color0_resolve)
			hits++;
	return hits;
}

/* Is every full-res (non-half-res) target in A also in B, same descriptor, same
 * order? HALF_W is the half-res width the bloom chain uses; targets at that
 * width are the chain's own and are skipped, so this compares the scene path
 * alone. */
static int scene_targets_match(const struct graph_shape *a,
			       const struct graph_shape *b, uint32_t half_w)
{
	uint32_t i, j = 0, k;

	for (i = 0; i < a->tex_count; i++) {
		if (a->tex[i].width == half_w)
			continue;
		while (j < b->tex_count && b->tex[j].width == half_w)
			j++;
		if (j >= b->tex_count)
			return 0;
		k = j++;
		if (a->tex[i].format  != b->tex[k].format ||
		    a->tex[i].width   != b->tex[k].width ||
		    a->tex[i].height  != b->tex[k].height ||
		    a->tex[i].samples != b->tex[k].samples)
			return 0;
	}
	while (j < b->tex_count && b->tex[j].width == half_w)
		j++;
	return j == b->tex_count;
}

/*
 * Entity 0 carries the v2 material (id 8) whose shader-ref selects shader/alt;
 * entity 1 carries the v3 material (id 6) whose shader-ref names the
 * scene-textured shader itself, so it reuses the pre-cached default
 * pipeline. So a frame binds two distinct pipelines, one switch between them.
 */
static void build_world_shaders(uint32_t box_ref)
{
	memset(&g_world, 0, sizeof(g_world));
	g_world.count = 2;

	g_world.alive[0]        = 1;
	g_world.mask[0]         = COMPONENT_RENDER | COMPONENT_MATERIAL;
	g_world.render_ref[0]   = box_ref;
	g_world.material_ref[0] = 8u;
	set_identity_xform(&g_world.world_xform[0], -1.0f, 0.0f, 0.0f);

	g_world.alive[1]        = 1;
	g_world.mask[1]         = COMPONENT_RENDER | COMPONENT_MATERIAL;
	g_world.render_ref[1]   = box_ref;
	g_world.material_ref[1] = 6u;
	set_identity_xform(&g_world.world_xform[1], 1.0f, 0.0f, 0.0f);
}

/* Place a transform-only, named entity at slot i — the way a scene's "Camera"
 * entity looks to the renderer: COMPONENT_NAME, no mesh, a bare position. */
static void set_named_entity(uint32_t i, const char *name,
			     float px, float py, float pz)
{
	uint32_t off = g_world.name_bytes;
	uint32_t n   = (uint32_t)strlen(name) + 1;

	g_world.alive[i]    = 1;
	g_world.mask[i]     = COMPONENT_NAME;
	g_world.name_off[i] = off;
	memcpy(g_world.names + off, name, n);
	g_world.name_bytes  = off + n;
	set_identity_xform(&g_world.world_xform[i], px, py, pz);
}

/*
 * The camera eye follows whatever entity a scene names "Camera", resolved by
 * name every frame so it survives a world clear. The second world is the
 * regression the by-name lookup fixes: switching games rebuilds ids from zero,
 * so the slot the old camera held is reused by an ordinary entity and the new
 * camera lands elsewhere — the eye must track the entity actually named
 * "Camera", not the stale slot (which would sink it into a floor-level cell and
 * hide the board).
 */
static void test_camera_follows_named_entity(struct subsystem_manager *mgr)
{
	const struct camera_api *cam = subsystem_manager_get_api(mgr, "camera");
	float eye[3];

	assert(cam);

	memset(&g_world, 0, sizeof(g_world));
	g_world.count = 1;
	set_named_entity(0, "Camera", 3.0f, 4.0f, 5.0f);
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	cam->get_eye(eye);
	assert(eye[0] == 3.0f && eye[1] == 4.0f && eye[2] == 5.0f);

	memset(&g_world, 0, sizeof(g_world));
	g_world.count = 2;
	set_named_entity(0, "cell-4", 0.0f, 0.02f, 0.0f); /* reuses the old slot */
	set_named_entity(1, "Camera", 0.0f, 4.0f, 3.5f);
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	cam->get_eye(eye);
	assert(eye[0] == 0.0f && eye[1] == 4.0f && eye[2] == 3.5f);
}

static float dist_to_origin(const float p[3])
{
	return sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
}

/*
 * Interactive camera override (#697): once the editor navigates the view, the
 * camera detaches from the scripted "Camera" entity and holds the user's pose
 * until reset_view() hands it back. dolly/orbit/pan each move the eye, and the
 * per-frame scripted-camera copy must NOT snap it back while the user owns it.
 */
static void test_camera_user_navigation(struct subsystem_manager *mgr)
{
	const struct camera_api *cam = subsystem_manager_get_api(mgr, "camera");
	float base[3], dolly[3], orbit[3], pan[3];

	assert(cam && cam->dolly && cam->orbit && cam->pan && cam->reset_view);

	/* A camera entity at a known spot; the tick copies it into the eye. The
	 * target is the origin, so distances below are measured from there. */
	memset(&g_world, 0, sizeof(g_world));
	g_world.count = 1;
	set_named_entity(0, "Camera", 0.0f, 0.0f, 4.0f);
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	cam->get_eye(base);
	assert(base[0] == 0.0f && base[1] == 0.0f && base[2] == 4.0f);

	/* Dolly toward the target pulls the eye in along the view ray. */
	cam->dolly(0.25f);
	cam->get_eye(dolly);
	assert(dist_to_origin(dolly) < dist_to_origin(base) - 1e-4f);

	/* The next tick must NOT snap the eye back to the scripted camera. */
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	cam->get_eye(pan);
	assert(pan[0] == dolly[0] && pan[1] == dolly[1] && pan[2] == dolly[2]);

	/* Orbit (yaw about world up) swings the eye through the xz-plane. */
	cam->orbit(0.3f, 0.1f);
	cam->get_eye(orbit);
	assert(orbit[0] != dolly[0] || orbit[2] != dolly[2]);

	/* Pan slides the eye across the view plane. */
	cam->pan(0.2f, 0.0f);
	cam->get_eye(pan);
	assert(pan[0] != orbit[0] || pan[1] != orbit[1] || pan[2] != orbit[2]);

	/* reset_view() hands the camera back: the next tick re-pins the eye to
	 * the "Camera" entity, wherever the user had dragged the view to. */
	cam->reset_view();
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	cam->get_eye(base);
	assert(base[0] == 0.0f && base[1] == 0.0f && base[2] == 4.0f);
}

/* One live box entity carrying MATERIAL_REF. */
static void build_world_material(uint32_t box_ref, uint32_t material_ref)
{
	memset(&g_world, 0, sizeof(g_world));
	g_world.count = 1;

	g_world.alive[0]        = 1;
	g_world.mask[0]         = COMPONENT_RENDER | COMPONENT_MATERIAL;
	g_world.render_ref[0]   = box_ref;
	g_world.material_ref[0] = material_ref;
	set_identity_xform(&g_world.world_xform[0], 0.0f, 0.0f, 0.0f);
}

/*
 * Bloom (#1022), asserted where it is decided: on the graph the tick declares.
 *
 * The two halves of the acceptance criteria are the two halves of this test.
 * A surface driving `emissive` past 1 must bloom, which shows up as four extra
 * passes and the half-res chain's three transients. A surface at 0 must be
 * unchanged — and "unchanged" here is not a pixel tolerance but the stronger
 * structural claim: the non-emissive frame declares the SAME passes on the SAME
 * targets at the SAME sample count as the glowing one, and differs from it by
 * exactly the four bloom passes and their three half-res transients. Nothing
 * about the scene path — offscreen target, depth, MSAA resolve — may move.
 *
 * That last part is the regression this pins. The offscreen scene target is
 * multisampled and resolved, and it is where anti-aliasing comes from; while
 * bloom ran unconditionally its composite was also what carried that target to
 * the backbuffer, so gating bloom on scene content is one short step from
 * quietly taking MSAA away from every scene that authors no emissive material.
 * The present pass exists to keep that from happening and this asserts it did
 * not.
 *
 * Both worlds carry the same mesh and the same shader, differing only in the
 * emissive field of the material, so nothing but the emissive drive can be
 * what moves the graph.
 */
static void test_bloom_follows_emissive(struct subsystem_manager *mgr)
{
	const struct camera_api *cam = subsystem_manager_get_api(mgr, "camera");
	struct graph_shape       dark, glow;

	assert(cam && cam->set_viewport);
	/* Bloom sizes its targets from the reported viewport; without one there
	 * is no offscreen path at all. 64x48 halves clean. */
	cam->set_viewport(64.0f, 48.0f);

	/* Emissive 0: the scene still goes offscreen and is presented, but no
	 * half-res target and no bloom pass is declared. */
	build_world_material(1, 11u);
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	capture_graph_shape(&dark);
	assert(count_texture_creates(32, 24) == 0);
	/* Shadow map, scene colour, scene depth, resolve — and nothing else. */
	assert(dark.tex_count == 4);
	/* Shadow, forward, present. */
	assert(dark.pass_count == 3);
	/*
	 * And it is genuinely multisampled and genuinely resolved — this is
	 * the anti-aliasing, and a scene with nothing to bloom gets it. The
	 * backend advertises GPU_CAP_MSAA_RESOLVE, so the scene colour is
	 * declared above one sample and exactly one pass carries a resolve.
	 */
	assert(color_target_samples(&dark, 64, 48) > 1);
	assert(count_resolving_passes(&dark) == 1);
	/* Shadow draw + forward draw + the present blit. */
	assert(count_draws(NULL) == 3);

	/*
	 * Emissive past 1: the chain joins the graph — the bright pass, two
	 * blurs and the composite — along with the three half-res transients
	 * they hand between them. Three more passes, not four: the composite
	 * terminates the graph, so it takes the present blit's place rather
	 * than adding to it.
	 */
	build_world_material(1, 10u);
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	capture_graph_shape(&glow);
	assert(glow.pass_count == dark.pass_count + 3);
	assert(count_texture_creates(32, 24) == 3);
	assert(glow.tex_count == dark.tex_count + 3);
	/*
	 * And the scene path is untouched: every full-res target matches the
	 * non-emissive frame's, descriptor for descriptor, sample count
	 * included. This is the assertion that fails if the offscreen path is
	 * ever re-tied to whether the scene has an emissive material.
	 */
	assert(scene_targets_match(&dark, &glow, 32));
	assert(count_resolving_passes(&glow) == 1);
	/*
	 * Shadow + forward as before, plus one bright-pass draw for the
	 * emitting mesh and one full-screen triangle each for blur H, blur V
	 * and the composite. The composite has replaced the present blit as
	 * the terminator, so this is three more draws, not four.
	 */
	assert(count_draws(NULL) == 6);

	/*
	 * The bright pass draws the emitting mesh and nothing else: adding a
	 * second, non-emissive entity adds a shadow draw and a forward draw,
	 * but no bright-pass draw. That is what keeps a lit-but-not-emissive
	 * surface out of the glow.
	 */
	g_world.count           = 2;
	g_world.alive[1]        = 1;
	g_world.mask[1]         = COMPONENT_RENDER | COMPONENT_MATERIAL;
	g_world.render_ref[1]   = 1u;
	g_world.material_ref[1] = 11u;
	set_identity_xform(&g_world.world_xform[1], 2.0f, 0.0f, 0.0f);
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	assert(count_draws(NULL) == 8);
}

/*
 * An off-axis (asymmetric) frustum, the glFrustum form: the left/right bounds
 * differ, so column 2 carries an x shear no symmetric mat4_perspective can
 * produce. Stands in for a projection the engine did not build.
 */
static void off_axis_proj(struct mat4 *out, float l, float r, float b, float t,
			  float n, float f)
{
	memset(out->m, 0, sizeof(out->m));
	out->m[0]  = 2.0f * n / (r - l);
	out->m[5]  = 2.0f * n / (t - b);
	out->m[8]  = (r + l) / (r - l);
	out->m[9]  = (t + b) / (t - b);
	out->m[10] = -(f + n) / (f - n);
	out->m[11] = -1.0f;
	out->m[14] = -2.0f * f * n / (f - n);
}

/*
 * The set side (#988): a caller hands the renderer a view, a projection and an
 * eye, and the renderer draws with exactly those — no re-derivation from
 * eye/target/fov. The scene still names a "Camera" entity and the viewport
 * still resizes, both of which drive the authored producer; neither may reach
 * the supplied pair. clear_view_proj hands the camera back.
 *
 * The projection here is off-axis, which the authored path cannot express at
 * all: if anything rebuilt it, the x shear would be gone. (The null backend
 * reports GL clip, so camera_clip_vp is the identity and the matrix the api
 * returns is the one the pass uploads.)
 */
static void test_camera_supplied_view_proj(struct subsystem_manager *mgr)
{
	const struct camera_api *cam = subsystem_manager_get_api(mgr, "camera");
	const float target[3] = { 0.0f, 0.0f, 0.0f };
	const float up[3]     = { 0.0f, 1.0f, 0.0f };
	const float eye[3]    = { 0.032f, 1.6f, -0.5f };
	struct mat4 view, proj, want, got;
	float       got_eye[3];
	uint32_t    authored_draws;

	assert(cam && cam->set_view_proj && cam->clear_view_proj);

	/* A scripted camera the tick copies into the eye every frame, plus a
	 * mesh to draw, so a leaking seam has something to leak. */
	memset(&g_world, 0, sizeof(g_world));
	g_world.count = 2;
	set_named_entity(0, "Camera", 9.0f, 9.0f, 9.0f);
	g_world.alive[1]        = 1;
	g_world.mask[1]         = COMPONENT_RENDER | COMPONENT_MATERIAL;
	g_world.render_ref[1]   = 1u;
	g_world.material_ref[1] = 6u;
	set_identity_xform(&g_world.world_xform[1], 0.0f, 0.0f, 0.0f);
	/* Establish the viewport BEFORE the baseline frame. A positive viewport
	 * is what puts the graph on the post path, so a set_viewport between
	 * the two frames below would change the pass list under the comparison
	 * and the counts would differ for a reason that has nothing to do with
	 * the camera. */
	cam->set_viewport(1920.0f, 1080.0f);
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	/* What the authored path draws for this world, whatever the graph's
	 * pass list happens to be — the supplied path below has to match it,
	 * and asserting a literal count here would instead be asserting how
	 * many passes draw the world, which is not this test's business. */
	authored_draws = count_draws(NULL);
	assert(authored_draws > 0);

	mat4_look_at(&view, eye, target, up);
	off_axis_proj(&proj, -0.09f, 0.11f, -0.10f, 0.10f, 0.1f, 100.0f);
	mat4_mul(&want, &proj, &view);

	cam->set_view_proj(&view, &proj, eye);
	cam->get_view_proj(&got);
	assert(memcmp(got.m, want.m, sizeof(want.m)) == 0);
	cam->get_eye(got_eye);
	assert(got_eye[0] == eye[0] && got_eye[1] == eye[1] &&
	       got_eye[2] == eye[2]);

	/* A frame (whose tick calls camera_update) and an aspect change both
	 * leave the pair exactly as supplied, and the frame draws exactly what
	 * the authored one did. A different aspect from the baseline's, so the
	 * change is real — but still positive, so the pass list is the same. */
	cam->set_viewport(1280.0f, 720.0f);
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	assert(count_draws(NULL) == authored_draws);
	cam->get_view_proj(&got);
	assert(memcmp(got.m, want.m, sizeof(want.m)) == 0);
	cam->get_eye(got_eye);
	assert(got_eye[0] == eye[0] && got_eye[1] == eye[1] &&
	       got_eye[2] == eye[2]);

	/* Handing it back returns the camera to the scripted eye and a
	 * projection the authored parameters produce. */
	cam->clear_view_proj();
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	cam->get_eye(got_eye);
	assert(got_eye[0] == 9.0f && got_eye[1] == 9.0f && got_eye[2] == 9.0f);
	cam->get_view_proj(&got);
	assert(memcmp(got.m, want.m, sizeof(want.m)) != 0);
}

/*
 * Every uniform-ring offset the frame's Camera-block binds named, in order.
 *
 * A draw takes its own slot in the shared ring and binds the Camera block at
 * that slot's byte offset, so this is the frame's ring allocation seen from
 * outside the renderer — the only place it is observable at all. Filtered by
 * the block's size, which is what tells a Camera bind apart from the outline's
 * and the blur's own one-slot UBOs (32 and 16 bytes) that also sit at slot 0.
 */
#define CAMERA_BLOCK_BYTES ((uint32_t)(36u * sizeof(float)))
#define MAX_RING_OFFSETS   64

static uint32_t camera_ring_offsets(uint32_t *out, uint32_t cap)
{
	const struct gpu_call_record *log;
	uint32_t count, i, n = 0;

	log = renderer_null_get_log(&count);
	for (i = 0; i < count && n < cap; i++) {
		if (log[i].type == GPU_CALL_CMD_BIND_UNIFORM_BUFFER &&
		    log[i].args.cmd_bind_uniform_buffer.slot == 0 &&
		    log[i].args.cmd_bind_uniform_buffer.size ==
			    CAMERA_BLOCK_BYTES)
			out[n++] = log[i].args.cmd_bind_uniform_buffer.offset;
	}
	return n;
}

/* Is every offset in OFF[0..N) distinct? Two draws sharing one ring slot is
 * exactly the clobber the ring exists to prevent. */
static int offsets_all_distinct(const uint32_t *off, uint32_t n)
{
	uint32_t i, j;

	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++)
			if (off[i] == off[j])
				return 0;
	return 1;
}

/* Is B exactly A repeated COPIES times — same passes and same transients, in
 * the same order, COPIES times over? That is what "N views each drew the graph
 * one view draws" means in the terms capture_graph_shape records. */
static int graph_shape_repeats(const struct graph_shape *a,
			       const struct graph_shape *b, uint32_t copies)
{
	uint32_t c, i;

	if (b->pass_count != a->pass_count * copies ||
	    b->tex_count != a->tex_count * copies)
		return 0;
	for (c = 0; c < copies; c++) {
		for (i = 0; i < a->pass_count; i++)
			if (memcmp(&b->pass[c * a->pass_count + i], &a->pass[i],
				   sizeof(a->pass[0])) != 0)
				return 0;
		for (i = 0; i < a->tex_count; i++)
			if (memcmp(&b->tex[c * a->tex_count + i], &a->tex[i],
				   sizeof(a->tex[0])) != 0)
				return 0;
	}
	return 1;
}

/* A view drawing at W x H through PROJ, from a fixed eye looking at the
 * origin. The eye is shared by the views below on purpose: a stereo pair
 * differs in its projection, and this test wants nothing else to. */
static void make_view(struct scene_view *v, const struct mat4 *proj,
		      uint32_t w, uint32_t h)
{
	static const float eye[3]    = { 0.0f, 0.0f, 4.0f };
	static const float target[3] = { 0.0f, 0.0f, 0.0f };
	static const float up[3]     = { 0.0f, 1.0f, 0.0f };
	struct mat4        view;

	memset(v, 0, sizeof(*v));
	mat4_look_at(&view, eye, target, up);
	camera_set_view_proj(&v->cam, &view, proj, eye);
	/* The billboard basis draw_particles reads is the authored one, so an
	 * explicitly-built view fills it too — see scene_view.h. */
	memcpy(v->cam.eye, eye, sizeof(eye));
	memcpy(v->cam.target, target, sizeof(target));
	memcpy(v->cam.up, up, sizeof(up));
	v->width  = w;
	v->height = h;
}

/*
 * One view draws the graph the renderer drew before there was a list (#989).
 *
 * The claim the whole change rests on is that a list of length one changes
 * nothing, and this asserts it from both ends. First literally: the frame's
 * passes and transients are counted against the numbers this path has always
 * produced — a shadow pass, a forward pass and the present blit, over the
 * shadow map, the multisampled scene colour, its depth and the resolve target,
 * with exactly one pass resolving. Then structurally: supplying a
 * one-entry list produces a graph identical to the one the frame derives when
 * no list is supplied, so the list machinery itself adds nothing, and clearing
 * the list puts the frame back on the derived path.
 */
static void test_one_view_graph_unchanged(struct subsystem_manager *mgr)
{
	const struct camera_api *cam = subsystem_manager_get_api(mgr, "camera");
	struct graph_shape       derived, supplied, cleared;
	struct scene_view        one;
	struct mat4              proj;
	uint32_t                 derived_draws;

	assert(cam && cam->set_viewport);
	build_world_material(1, 11u);   /* one box, nothing emissive */
	cam->set_viewport(64.0f, 48.0f);
	scene_renderer_clear_views();

	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	capture_graph_shape(&derived);
	derived_draws = count_draws(NULL);

	/* Shadow, forward, present — over the shadow map, the scene colour,
	 * its depth and the resolve target. */
	assert(derived.pass_count == 3);
	assert(derived.tex_count == 4);
	assert(color_target_samples(&derived, 64, 48) > 1);
	assert(count_resolving_passes(&derived) == 1);
	assert(derived_draws == 3);        /* shadow + forward + the blit */

	/* The same frame with the view stated rather than derived. The matrices
	 * differ from the scene camera's and are allowed to: what is asserted
	 * is the graph, a function of the view's size and not of its pose. */
	mat4_perspective(&proj, 0.8f, 64.0f / 48.0f, 0.1f, 100.0f);
	make_view(&one, &proj, 64, 48);
	scene_renderer_set_views(&one, 1);
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	capture_graph_shape(&supplied);
	assert(memcmp(&supplied, &derived, sizeof(derived)) == 0);
	assert(count_draws(NULL) == derived_draws);

	/* And handing the list back returns the frame to the derived view. */
	scene_renderer_clear_views();
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	capture_graph_shape(&cleared);
	assert(memcmp(&cleared, &derived, sizeof(derived)) == 0);
}

/*
 * Two views, drawn one after the other in one frame (#989).
 *
 * They differ only in projection — one symmetric, one off-axis — which is the
 * stereo case exactly: the same head, the same instant, two frustums. What has
 * to hold is that each of them draws the graph a single view draws, and that
 * the second corrupts nothing the first left behind.
 *
 * THE UNIFORM RING IS THE ONE WORTH ASSERTING. Every draw takes its own slot
 * from a ring rewound once per frame, and a second view's draws must take
 * slots of their own rather than reusing the first's — on WebGPU a
 * buffer_update is a queue-timeline write, so re-using view 0's slots would
 * overwrite the uniforms view 0's already-recorded commands are still going to
 * read, and both eyes would come out drawn with the last eye's transforms with
 * nothing logged. Distinct bind offsets across the whole frame is that
 * property, in the only form the call log can show it.
 */
static void test_two_views(struct subsystem_manager *mgr)
{
	const struct camera_api *cam = subsystem_manager_get_api(mgr, "camera");
	struct scene_view        views[2];
	struct graph_shape       one, two;
	struct mat4              sym, off_axis;
	uint32_t                 off[MAX_RING_OFFSETS];
	uint32_t                 one_draws, one_offsets, n;

	assert(cam && cam->set_viewport);
	build_world_material(1, 11u);   /* one box, nothing emissive */
	cam->set_viewport(64.0f, 48.0f);

	mat4_perspective(&sym, 0.8f, 64.0f / 48.0f, 0.1f, 100.0f);
	off_axis_proj(&off_axis, -0.09f, 0.11f, -0.10f, 0.10f, 0.1f, 100.0f);
	make_view(&views[0], &sym, 64, 48);
	make_view(&views[1], &off_axis, 64, 48);

	/* The one-view frame this pair has to reproduce twice. */
	scene_renderer_set_views(views, 1);
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	capture_graph_shape(&one);
	one_draws   = count_draws(NULL);
	one_offsets = camera_ring_offsets(off, MAX_RING_OFFSETS);
	assert(one_offsets > 0);

	scene_renderer_set_views(views, 2);
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	capture_graph_shape(&two);

	/* Both drew, and each drew the same graph one view draws. */
	assert(count_draws(NULL) == 2 * one_draws);
	assert(graph_shape_repeats(&one, &two, 2));
	assert(count_resolving_passes(&two) == 2);

	/* And the second did not take the first's ring slots. */
	n = camera_ring_offsets(off, MAX_RING_OFFSETS);
	assert(n == 2 * one_offsets);
	assert(offsets_all_distinct(off, n));

	/*
	 * Per-view dimensions, not a global one: halving the second view's size
	 * gives it its own transients at its own size, and leaves the first
	 * view's alone. Three targets each — the multisampled scene colour, its
	 * depth and the resolve — since the shadow map is square and fixed.
	 */
	views[1].width  = 32;
	views[1].height = 24;
	scene_renderer_set_views(views, 2);
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	assert(count_texture_creates(64, 48) == 3);
	assert(count_texture_creates(32, 24) == 3);

	scene_renderer_clear_views();
}

/*
 * The two post chains, per view (#989). Both hang off the view's own size, so
 * both had to stop reading a global one; each is asserted by the passes and
 * transients it adds, doubled.
 */
static void test_post_chains_per_view(struct subsystem_manager *mgr)
{
	const struct camera_api *cam = subsystem_manager_get_api(mgr, "camera");
	struct scene_view        views[2];
	struct graph_shape       one, two;
	struct mat4              sym, off_axis;

	assert(cam && cam->set_viewport);
	cam->set_viewport(64.0f, 48.0f);
	mat4_perspective(&sym, 0.8f, 64.0f / 48.0f, 0.1f, 100.0f);
	off_axis_proj(&off_axis, -0.09f, 0.11f, -0.10f, 0.10f, 0.1f, 100.0f);
	make_view(&views[0], &sym, 64, 48);
	make_view(&views[1], &off_axis, 64, 48);

	/*
	 * Bloom: an emissive material, so each view declares the bright pass,
	 * two blurs and the composite over three half-res transients of its
	 * own. Six 32x24 targets across the pair is the whole claim — one chain
	 * per view, sized from that view.
	 */
	build_world_material(1, 10u);
	scene_renderer_set_views(views, 1);
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	capture_graph_shape(&one);
	/* Shadow, forward, bright pass, blur H, blur V, composite. */
	assert(one.pass_count == 6);
	assert(count_texture_creates(32, 24) == 3);

	scene_renderer_set_views(views, 2);
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	capture_graph_shape(&two);
	assert(graph_shape_repeats(&one, &two, 2));
	assert(count_texture_creates(32, 24) == 6);

	/*
	 * Outline: a selected mesh, so each view declares the mask pass and the
	 * composite that terminates it over a mask target of its own. Back to a
	 * non-emissive material so the two chains are counted separately.
	 */
	build_world_material(1, 11u);
	g_selected = 0;
	scene_renderer_set_views(views, 1);
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	capture_graph_shape(&one);
	assert(one.pass_count == 4);   /* shadow, forward, mask, outline */
	assert(one.tex_count == 5);    /* + the silhouette mask */

	scene_renderer_set_views(views, 2);
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	capture_graph_shape(&two);
	assert(graph_shape_repeats(&one, &two, 2));

	g_selected = -1;
	scene_renderer_clear_views();
}

/*
 * A list longer than the renderer can hold is refused whole, not truncated:
 * the frame keeps drawing the list it had. Truncating would silently draw
 * fewer eyes than the caller asked for, which is a wrong frame with nothing to
 * notice it by.
 */
static void test_view_list_overflow_refused(struct subsystem_manager *mgr)
{
	const struct camera_api *cam = subsystem_manager_get_api(mgr, "camera");
	struct scene_view        views[SCENE_MAX_VIEWS + 1];
	struct graph_shape       before, after;
	struct mat4              sym;
	uint32_t                 i;

	assert(cam && cam->set_viewport);
	build_world_material(1, 11u);
	cam->set_viewport(64.0f, 48.0f);
	mat4_perspective(&sym, 0.8f, 64.0f / 48.0f, 0.1f, 100.0f);
	for (i = 0; i < SCENE_MAX_VIEWS + 1; i++)
		make_view(&views[i], &sym, 64, 48);

	scene_renderer_set_views(views, 1);
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	capture_graph_shape(&before);

	scene_renderer_set_views(views, SCENE_MAX_VIEWS + 1);
	renderer_null_reset_log();
	subsystem_manager_tick(mgr);
	capture_graph_shape(&after);
	assert(memcmp(&after, &before, sizeof(before)) == 0);

	scene_renderer_clear_views();
}

int main(void)
{
	static const struct subsystem static_table[] = {
		{ .name = "asset", .api = &FAKE_ASSET },
		{ .name = "scene", .api = &FAKE_SCENE },
		{ NULL },
	};
	struct subsystem_manager mgr;
	uint32_t idx_count = 0;

	mem_init();
	script_init(); /* loads the embedded mesh_script.scm image */

	/* box is catalog id 1. Two live entities reference it. */
	build_world(1);

	subsystem_manager_init(&mgr, static_table);
	renderer_null_plugin_entry(&mgr);   /* "renderer" */
	fg_plugin_entry(&mgr);              /* "frame_graph" (needs renderer) */
	scene_renderer_plugin_entry(&mgr);  /* resolves all, init uploads meshes */

	/*
	 * A shadow pass and a forward pass, each drawing once per live entity
	 * that carries both a mesh and a material — entity 1's mesh is skipped
	 * since it has no material. The shadow pass walks the same drawables as
	 * the forward pass and binds neither material nor light, so it doubles
	 * the draw count and leaves the two bind counts below alone.
	 */
	renderer_null_reset_log();
	subsystem_manager_tick(&mgr);
	assert(count_draws(&idx_count) == 2);
	assert(idx_count == 36);            /* box: 36 indices */
	assert(count_material_binds() == 1); /* one per forward draw */
	assert(count_light_binds() == 1);    /* Sun bound once for the pass */

	/* Degrade safe: an empty world draws nothing and does not crash. */
	g_world.count = 0;
	renderer_null_reset_log();
	subsystem_manager_tick(&mgr);
	assert(count_draws(NULL) == 0);

	/*
	 * Per-material shader: a material that selects shader/alt gets its own
	 * pipeline, compiled once and off-frame (from the tick, not the pass).
	 * Two creates, not one: the backend advertises GPU_CAP_MSAA_RESOLVE, so
	 * every scene shader also gets the multisampled twin the offscreen
	 * forward pass draws with. Every live entity still draws — twice, once
	 * per pass — so two entities are four draws. (The create count, not a
	 * bind, is the observable signal: handles here are opaque.)
	 */
	build_world_shaders(1);
	renderer_null_reset_log();
	subsystem_manager_tick(&mgr);
	assert(count_calls(GPU_CALL_PIPELINE_CREATE) == 2); /* alt, x1 and x4 */
	assert(count_draws(NULL) == 4);

	/* The pipeline is cached by shader id: a later frame compiles nothing. */
	renderer_null_reset_log();
	subsystem_manager_tick(&mgr);
	assert(count_calls(GPU_CALL_PIPELINE_CREATE) == 0);
	assert(count_draws(NULL) == 4);

	test_camera_follows_named_entity(&mgr);
	test_camera_user_navigation(&mgr);
	test_camera_supplied_view_proj(&mgr);
	/* From here on a viewport has been reported, and camera_set_viewport
	 * keeps the last positive one, so every frame takes the post path. */
	test_bloom_follows_emissive(&mgr);
	test_one_view_graph_unchanged(&mgr);
	test_two_views(&mgr);
	test_post_chains_per_view(&mgr);
	test_view_list_overflow_refused(&mgr);

	subsystem_manager_shutdown(&mgr);
	mem_shutdown();

	printf("scene_renderer tests passed\n");
	return 0;
}
