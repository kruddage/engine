/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "asset.h"
#include "asset_api.h"
#include "asset_codec_api.h"
#include "builtin_scripts.h"
#include "builtin_mesh_scripts.h"
#include "builtin_texture_scripts.h"
#include "builtin_sound_scripts.h"
#include "asset_edit.h"
#include "edit_api.h"
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"
#include "memory_api.h"

#include <string.h>
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/fetch.h>
#else
#include <stdio.h>
#include "log.h"
#include "memory.h"
static const struct log_api    native_log = { log_write };
static const struct memory_api native_mem = {
	mem_alloc, mem_alloc_zero, mem_free,
	mem_pool_create, mem_pool_alloc, mem_pool_free, mem_pool_destroy,
};
#endif

#define ASSET_CACHE_MAX  64
#define CODEC_TABLE_MAX  16

/*
 * Authored assets may carry a small declaration set (e.g. a shader's
 * stage/dialect).  Stored inline so describe() can hand out pointers into
 * the entry's own storage, valid until the entry is evicted or re-declared.
 */
#define ASSET_DECL_MAX     8
#define ASSET_DECL_KEY_MAX 24
#define ASSET_DECL_VAL_MAX 48

struct asset_decl_store {
	char key[ASSET_DECL_KEY_MAX];
	char val[ASSET_DECL_VAL_MAX];
};

struct asset_entry {
	char     path[ASSET_PATH_MAX];
	uint8_t *data;
	uint32_t size;
	int32_t  refs;
	int32_t  state;    /* asset_state */
	int32_t  kind;     /* ASSET_KIND_* */
	int32_t  read_only;
	int32_t  type;     /* ASSET_TYPE_* */
	uint32_t id;       /* stable identity; never 0, never reused */
	int32_t  origin;   /* ASSET_ORIGIN_* */
	struct asset_decl_store decl[ASSET_DECL_MAX];
	uint32_t ndecl;    /* authored declaration field count; 0 = none */
};

struct codec_entry {
	char  ext[16];
	void *(*decode)(const void *bytes, uint32_t size);
	void *(*encode)(const void *typed, uint32_t *out_size);
};

static struct asset_entry cache[ASSET_CACHE_MAX];
static int32_t            cache_count;

static struct codec_entry codec_table[CODEC_TABLE_MAX];
static int32_t            codec_count;

static uint32_t next_asset_id = 1; /* 0 is reserved for "none" */

#ifdef __EMSCRIPTEN__
static const struct log_api    *g_log;
static const struct memory_api *g_mem;
#else
static const struct log_api    *g_log = &native_log;
static const struct memory_api *g_mem = &native_mem;
#endif

/*
 * The "edit" undo/redo service, resolved lazily. The asset plugin loads before
 * edit_plugin, so "edit" is not registered yet at our plugin_entry — we stash
 * the manager and look it up on the first authored mutation instead. g_edit
 * stays NULL when the service is absent (e.g. native unit tests), in which case
 * the mutation still happens and simply isn't recorded (no hard dependency).
 */
static struct subsystem_manager *g_mgr;
static const struct edit_api    *g_edit;

static const struct edit_api *resolve_edit(void)
{
	if (!g_edit && g_mgr)
		g_edit = subsystem_manager_get_api(g_mgr, "edit");
	return g_edit;
}

static struct asset_entry *find_entry(const char *path)
{
	int32_t i;

	for (i = 0; i < cache_count; i++) {
		if (strncmp(cache[i].path, path, ASSET_PATH_MAX) == 0)
			return &cache[i];
	}
	return NULL;
}

static struct asset_entry *find_entry_by_id(uint32_t id)
{
	int32_t i;

	for (i = 0; i < cache_count; i++) {
		if (cache[i].id == id)
			return &cache[i];
	}
	return NULL;
}

static struct asset_entry *alloc_entry(const char *path)
{
	struct asset_entry *e;

	if (cache_count >= ASSET_CACHE_MAX)
		return NULL;
	e = &cache[cache_count++];
	strncpy(e->path, path, ASSET_PATH_MAX - 1);
	e->path[ASSET_PATH_MAX - 1] = '\0';
	e->data      = NULL;
	e->size      = 0;
	e->refs      = 0;
	e->state     = ASSET_PENDING;
	e->kind      = ASSET_KIND_NORMAL;
	e->read_only = 0;
	e->type      = ASSET_TYPE_UNKNOWN;
	e->id        = next_asset_id++;
	e->origin    = ASSET_ORIGIN_FETCHED;
	e->ndecl     = 0;
	return e;
}

static void evict_entry(struct asset_entry *e)
{
	if (e->data)
		g_mem->free(e->data);
	*e = cache[--cache_count];
}

/* ------------------------------------------------------------------ */
/* Built-in mesh script asset library                                  */
/* ------------------------------------------------------------------ */

/*
 * Built-in shaders, authored in the krudd shader DSL (a Scheme S-expression
 * carrying both stages and a shared IO model).  This source — not GLSL — is
 * what the asset stores, so the editor shows the DSL when you click a built-in
 * shader and the same bytes feed native and WebGPU backends later; the WebGL
 * renderer lowers the DSL to GLSL ES 3.00 at bind time (see script.h's
 * shader_transpile).  describe()'s decl-fields below advertise the source
 * format and which stages are present.
 *
 * Scene-textured shader — the entity-driven pipeline the scene renderer
 * (#172) binds, and the engine's only built-in scene shader. Consumes the
 * mesh_vertex layout (position/normal/uv0), a std140 Camera block carrying
 * view_proj and per-draw model, and a std140 Material block carrying a
 * single per-draw base_color tint (#materials v0 — the smallest possible
 * material parameter, with no fixed-function state beyond it). Each block
 * binds at the GL index matching its declaration order among the blocks a
 * stage actually uses (see scene_renderer.c), so Camera (vertex-only) lands
 * at slot 0 and Material (fragment-only) at slot 1. It also adds a v_uv
 * varying and an `albedo` sampler2D (its own texture unit, not the Material
 * block); the fragment stage shades from the world normal (readable 3D, no
 * lighting), multiplies in the material tint, then multiplies in the sampled
 * albedo — a material naming this shader is what makes a procedural texture
 * actually appear on a mesh.
 */
static const char *SCENE_TEXTURED_SHADER_SRC =
	"(shader scene-textured\n"
	"  (inputs\n"
	"    (a_pos    vec3 (location 0))\n"
	"    (a_normal vec3 (location 1))\n"
	"    (a_uv0    vec2 (location 2)))\n"
	"  (uniforms\n"
	"    (Camera (block 0) (layout std140)\n"
	"      (view_proj mat4)\n"
	"      (model     mat4))\n"
	"    (Material (block 1) (layout std140)\n"
	"      (base_color vec4 (edit color)))\n"
	"    (albedo sampler2D))\n"
	"  (varyings\n"
	"    (v_normal vec3)\n"
	"    (v_uv     vec2))\n"
	"  (targets\n"
	"    (frag_color vec4 (location 0)))\n"
	"  (vertex\n"
	"    (set v_normal (* (mat3 model) a_normal))\n"
	"    (set v_uv a_uv0)\n"
	"    (set position (* view_proj model (vec4 a_pos 1.0))))\n"
	"  (fragment\n"
	"    (let* ((n    (normalize v_normal))\n"
	"           (base (+ 0.5 (* 0.5 n)))\n"
	"           (diff (max (dot n (normalize (vec3 0.5 0.8 0.4))) 0.0))\n"
	"           (lit  (* base (+ 0.35 (* 0.65 diff))))\n"
	"           (tex  (sample albedo v_uv)))\n"
	"      (set frag_color\n"
	"           (vec4 (* lit (swizzle base_color rgb) (swizzle tex rgb)) 1.0)))))\n";

/*
 * Opaque white — the multiplicative identity the scene-textured shader's
 * base_color expects, so a textured material's tint defaults to "show the
 * texture unmodified" rather than tinting it.
 */
static const float DEFAULT_MATERIAL_COLOR[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

/*
 * A physically based (metallic-roughness) scene shader.  It speaks the same
 * vertex/Camera IO contract as scene-textured (a_pos/a_normal/a_uv0 in, Camera
 * at block 0, Material at block 1), so it drops into the scene pipeline exactly
 * like a custom material shader — only its shading differs.  The Material block
 * is { base_color vec4; metallic float; roughness float; }, which the editor
 * turns into a colour swatch and two 0..1 sliders straight from the (edit ...)
 * hints; the renderer packs those bytes std140 with no schema of its own.
 *
 * The fragment is a Cook-Torrance BRDF: a GGX (Trowbridge-Reitz) normal
 * distribution, a Smith/Schlick-GGX geometry term, and a Schlick Fresnel with
 * F0 = mix(0.04, base_color, metallic).  Direct light is one directional key
 * read from the Sun block — its world-space direction and radiance are uploaded
 * by scene_renderer from the scene's light entity (a COMPONENT_LIGHT entity),
 * or a default sun when the scene has none.  A cheap sky/ground hemisphere
 * stands in for image-based lighting so metals still read instead of going
 * black.  The view direction is the real one — cam_pos (in the Camera block)
 * minus the interpolated world position — so specular highlights track the
 * camera.  It samples no albedo texture, so a material naming it carries no
 * texture slot.
 *
 * Block naming matters: the webgl backend assigns uniform-block binding slots
 * by sorting block NAMES alphabetically (GLSL ES 300 has no layout(binding=N)),
 * so the names must sort Camera < Material < Sun to land on slots 0/1/2 — the
 * slots scene_renderer binds them to.  "Sun" is chosen to sort after "Material"
 * for exactly that reason; renaming it below "Material" would silently swap the
 * Material and light bindings.
 */
static const char *PBR_SHADER_SRC =
	"(shader pbr\n"
	"  (inputs\n"
	"    (a_pos    vec3 (location 0))\n"
	"    (a_normal vec3 (location 1))\n"
	"    (a_uv0    vec2 (location 2)))\n"
	"  (uniforms\n"
	"    (Camera (block 0) (layout std140)\n"
	"      (view_proj mat4)\n"
	"      (model     mat4)\n"
	"      (cam_pos   vec3))\n"
	"    (Material (block 1) (layout std140)\n"
	"      (base_color vec4  (edit color) (default 0.82 0.82 0.85 1.0))\n"
	"      (metallic   float (edit range 0.0 1.0) (default 0.1))\n"
	"      (roughness  float (edit range 0.0 1.0) (default 0.5)))\n"
	"    (Sun (block 2) (layout std140)\n"
	"      (light_dir      vec3)\n"
	"      (light_radiance vec3)))\n"
	"  (varyings\n"
	"    (v_normal   vec3)\n"
	"    (v_worldpos vec3))\n"
	"  (targets\n"
	"    (frag_color vec4 (location 0)))\n"
	"  (vertex\n"
	"    (set v_normal (* (mat3 model) a_normal))\n"
	"    (set v_worldpos (swizzle (* model (vec4 a_pos 1.0)) xyz))\n"
	"    (set position (* view_proj model (vec4 a_pos 1.0))))\n"
	"  (fragment\n"
	"    (let* ((n      (normalize v_normal))\n"
	"           (l      (normalize light_dir))\n"
	"           (v      (normalize (- cam_pos v_worldpos)))\n"
	"           (h      (normalize (+ l v)))\n"
	"           (ndl    (max (dot n l) 0.0))\n"
	"           (ndv    (max (dot n v) 0.0001))\n"
	"           (ndh    (max (dot n h) 0.0))\n"
	"           (vdh    (max (dot v h) 0.0))\n"
	"           (albedo (swizzle base_color rgb))\n"
	"           (f0     (mix (vec3 0.04 0.04 0.04) albedo metallic))\n"
	"           (a      (* roughness roughness))\n"
	"           (a2     (* a a))\n"
	"           (dnm    (+ (* ndh ndh (- a2 1.0)) 1.0))\n"
	"           (ndf    (/ a2 (* 3.14159265 dnm dnm)))\n"
	"           (k      (/ (* (+ roughness 1.0) (+ roughness 1.0)) 8.0))\n"
	"           (gv     (/ ndv (+ (* ndv (- 1.0 k)) k)))\n"
	"           (gl     (/ ndl (+ (* ndl (- 1.0 k)) k)))\n"
	"           (g      (* gv gl))\n"
	"           (fres   (+ f0 (* (- (vec3 1.0 1.0 1.0) f0) (pow (- 1.0 vdh) 5.0))))\n"
	"           (spec   (/ (* ndf g fres) (+ (* 4.0 ndv ndl) 0.0001)))\n"
	"           (kd     (* (- (vec3 1.0 1.0 1.0) fres) (- 1.0 metallic)))\n"
	"           (diff   (* kd albedo 0.31831))\n"
	"           (lo     (* (+ diff spec) light_radiance ndl))\n"
	"           (sky    (vec3 0.55 0.62 0.75))\n"
	"           (ground (vec3 0.20 0.19 0.17))\n"
	"           (hemi   (mix ground sky (+ 0.5 (* 0.5 (swizzle n y)))))\n"
	"           (amb    (* 0.45 (+ (* hemi albedo (- 1.0 metallic)) (* hemi f0))))\n"
	"           (color  (+ amb lo))\n"
	"           (mapped (/ color (+ color (vec3 1.0 1.0 1.0))))\n"
	"           (gamma  (pow mapped (vec3 0.4545 0.4545 0.4545))))\n"
	"      (set frag_color (vec4 gamma 1.0)))))\n";

static int builtins_seeded;

/*
 * Seed one built-in shader asset from NUL-terminated DSL source.  A heap
 * copy of the source (including its trailing NUL) becomes the asset's bytes,
 * so consumers can use get_data() directly as a C string and shutdown frees
 * it uniformly with fetched assets.  size counts the stored bytes (NUL
 * included).
 */
static uint32_t seed_shader(const char *path, const char *src)
{
	struct asset_entry *e;
	uint32_t            n;

	e = alloc_entry(path);
	if (!e)
		return 0;
	n = (uint32_t)strlen(src) + 1;
	e->data = g_mem->alloc(n);
	if (!e->data) {
		e->state = ASSET_ERROR;
		return 0;
	}
	memcpy(e->data, src, n);
	e->size      = n;
	e->state     = ASSET_LOADED;
	e->kind      = ASSET_KIND_PRIMITIVE;
	e->read_only = 1;
	e->type      = ASSET_TYPE_SHADER;
	return e->id;
}

/*
 * Seed a built-in textured material: the v3 wire form (shader-ref + the shader's
 * std140 Material block) followed by an optional texture slot the renderer reads
 * as a trailer — [texture-ref u32][width u32][height u32][texture gen-params...].
 * Here the slot names a built-in texture at a fixed bake size and no param
 * override (the texture uses its declared defaults), so the material renders that
 * procedural texture on whatever mesh it is bound to. shader_ref must name the
 * scene-textured shader (whose Material block is one vec4 base_color, 16 bytes),
 * so the trailer starts at offset 4 + 16; the renderer locates it from that
 * block size. A zero shader_ref or tex_ref makes this a no-op.
 */
static void seed_textured_material(const char *path, uint32_t shader_ref,
				   const float rgba[4], uint32_t tex_ref,
				   uint32_t width, uint32_t height)
{
	struct asset_entry *e;
	uint32_t            n;
	unsigned char      *p;

	if (!shader_ref || !tex_ref)
		return;
	e = alloc_entry(path);
	if (!e)
		return;
	n = (uint32_t)(sizeof(uint32_t)      /* shader-ref            */
		       + 4 * sizeof(float)   /* base_color block      */
		       + 3 * sizeof(uint32_t)); /* tex-ref, width, height */
	e->data = g_mem->alloc(n);
	if (!e->data) {
		e->state = ASSET_ERROR;
		return;
	}
	p = (unsigned char *)e->data;
	memcpy(p, &shader_ref, sizeof(shader_ref));                     p += 4;
	memcpy(p, rgba, 4 * sizeof(float));                             p += 16;
	memcpy(p, &tex_ref, sizeof(tex_ref));                          p += 4;
	memcpy(p, &width, sizeof(width));                              p += 4;
	memcpy(p, &height, sizeof(height));
	e->size      = n;
	e->state     = ASSET_LOADED;
	e->kind      = ASSET_KIND_PRIMITIVE;
	e->read_only = 1;
	e->type      = ASSET_TYPE_MATERIAL;
}

/*
 * Seed a built-in PBR material: the v3 wire form with no texture trailer, just
 * [shader-ref u32][Material block std140].  The pbr shader's Material block is
 * { base_color vec4; metallic float; roughness float; }, which std140-packs to
 * 32 bytes (base_color @0, metallic @16, roughness @20, the block rounded up to
 * 32), so the material is 4 + 32 = 36 bytes with the trailing pad left zero.
 * There is no texture slot: material_texture reads none because the bytes end
 * exactly at header + block, so the shader renders its pure parametric shading.
 * A zero shader_ref makes this a no-op.
 */
static void seed_pbr_material(const char *path, uint32_t shader_ref,
			      const float rgba[4], float metallic,
			      float roughness)
{
	struct asset_entry *e;
	uint32_t            n;
	unsigned char      *p;

	if (!shader_ref)
		return;
	e = alloc_entry(path);
	if (!e)
		return;
	n = (uint32_t)(sizeof(uint32_t)      /* shader-ref                 */
		       + 8 * sizeof(float)); /* std140 Material block (32B) */
	e->data = g_mem->alloc(n);
	if (!e->data) {
		e->state = ASSET_ERROR;
		return;
	}
	memset(e->data, 0, n);            /* leaves the block's tail pad zero */
	p = (unsigned char *)e->data;
	memcpy(p,      &shader_ref, sizeof(shader_ref)); /* @0  shader-ref  */
	memcpy(p + 4,  rgba, 4 * sizeof(float));         /* @4  base_color  */
	memcpy(p + 20, &metallic,  sizeof(metallic));    /* @20 metallic    */
	memcpy(p + 24, &roughness, sizeof(roughness));   /* @24 roughness   */
	e->size      = n;
	e->state     = ASSET_LOADED;
	e->kind      = ASSET_KIND_PRIMITIVE;
	e->read_only = 1;
	e->type      = ASSET_TYPE_MATERIAL;
}

/*
 * Seed one built-in entity script from NUL-terminated Scheme source, the same
 * shape as seed_shader: the (script NAME ...) text becomes the asset's bytes so
 * the entity-script driver can read it back as a C string via get_data().  The
 * asset is what an entity's script_ref points at; ASSET_TYPE_SCRIPT tags it for
 * the script-only picker.
 */
static void seed_script(const char *path, const char *src)
{
	struct asset_entry *e;
	uint32_t            n;

	e = alloc_entry(path);
	if (!e)
		return;
	n = (uint32_t)strlen(src) + 1;
	e->data = g_mem->alloc(n);
	if (!e->data) {
		e->state = ASSET_ERROR;
		return;
	}
	memcpy(e->data, src, n);
	e->size      = n;
	e->state     = ASSET_LOADED;
	e->kind      = ASSET_KIND_PRIMITIVE;
	e->read_only = 1;
	e->type      = ASSET_TYPE_SCRIPT;
}

/*
 * Seed one built-in mesh from NUL-terminated Scheme source, the same shape as
 * seed_script: the (mesh NAME (generate () ...)) text becomes the asset's
 * bytes, so a consumer resolves it to a real mesh_blob on demand via
 * mesh_script_generate() (asset/mesh_script.c) rather than at seed time —
 * mirroring how a shader asset stores DSL source, not compiled GLSL. There is
 * no separate "mesh script" type: every ASSET_TYPE_MESH asset is one of
 * these, full stop.
 */
static void seed_mesh(const char *path, const char *src)
{
	struct asset_entry *e;
	uint32_t            n;

	e = alloc_entry(path);
	if (!e)
		return;
	n = (uint32_t)strlen(src) + 1;
	e->data = g_mem->alloc(n);
	if (!e->data) {
		e->state = ASSET_ERROR;
		return;
	}
	memcpy(e->data, src, n);
	e->size      = n;
	e->state     = ASSET_LOADED;
	e->kind      = ASSET_KIND_PRIMITIVE;
	e->read_only = 1;
	e->type      = ASSET_TYPE_MESH;
}

/*
 * Seed one built-in texture from NUL-terminated Scheme source, the same shape as
 * seed_mesh: the (texture NAME (shade (u v) ...)) text becomes the asset's bytes,
 * so a consumer bakes it to a real texture_blob on demand via
 * texture_script_generate() (asset/texture_script.c) at whatever resolution it
 * asks for, rather than at seed time. There is no separate "texture script"
 * type: every ASSET_TYPE_TEXTURE asset is one of these.
 */
static uint32_t seed_texture(const char *path, const char *src)
{
	struct asset_entry *e;
	uint32_t            n;

	e = alloc_entry(path);
	if (!e)
		return 0;
	n = (uint32_t)strlen(src) + 1;
	e->data = g_mem->alloc(n);
	if (!e->data) {
		e->state = ASSET_ERROR;
		return 0;
	}
	memcpy(e->data, src, n);
	e->size      = n;
	e->state     = ASSET_LOADED;
	e->kind      = ASSET_KIND_PRIMITIVE;
	e->read_only = 1;
	e->type      = ASSET_TYPE_TEXTURE;
	return e->id;
}

/*
 * Seed one built-in sound from NUL-terminated Scheme source, the same shape as
 * seed_texture: the (sound NAME (sample (t) ...)) text becomes the asset's
 * bytes, so a consumer bakes it to a real sound_blob on demand via
 * sound_script_generate() (asset/sound_script.c) at whatever sample rate its
 * audio context runs, rather than at seed time. There is no separate "sound
 * script" type: every ASSET_TYPE_SOUND asset is one of these.
 */
static void seed_sound(const char *path, const char *src)
{
	struct asset_entry *e;
	uint32_t            n;

	e = alloc_entry(path);
	if (!e)
		return;
	n = (uint32_t)strlen(src) + 1;
	e->data = g_mem->alloc(n);
	if (!e->data) {
		e->state = ASSET_ERROR;
		return;
	}
	memcpy(e->data, src, n);
	e->size      = n;
	e->state     = ASSET_LOADED;
	e->kind      = ASSET_KIND_PRIMITIVE;
	e->read_only = 1;
	e->type      = ASSET_TYPE_SOUND;
}

static void seed_builtins(void)
{
	if (builtins_seeded)
		return;
	builtins_seeded = 1;

	/*
	 * Every built-in mesh is authored the same way — there is no
	 * hardcoded C mesh generator. seed_mesh stores each
	 * (mesh NAME (generate () ...)) source verbatim; a consumer resolves
	 * it to a real mesh_blob on demand via mesh_script_generate().
	 */
	seed_mesh("builtin://mesh/box",     BOX_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/sphere",  SPHERE_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/plane",   PLANE_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/pyramid", PYRAMID_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/grid",    GRID_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/cylinder",     CYLINDER_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/cone",         CONE_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/disc",         DISC_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/capsule",      CAPSULE_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/torus",        TORUS_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/superquadric", SUPERQUADRIC_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/heightfield",  HEIGHTFIELD_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/sdf-rook",     SDF_ROOK_MESH_SCRIPT_SRC);

	seed_script("builtin://script/spinner", SPINNER_SCRIPT_SRC);
	seed_script("builtin://script/bounce",  BOUNCE_SCRIPT_SRC);
	seed_script("builtin://script/wobble",  WOBBLE_SCRIPT_SRC);
	seed_script("builtin://script/pulse",   PULSE_SCRIPT_SRC);
	seed_script("builtin://script/orbit-camera", ORBIT_CAMERA_SCRIPT_SRC);

	/*
	 * Built-in textures, authored the same way as the meshes: each is a
	 * (texture NAME (shade (u v) ...)) source, baked to a texture_blob on
	 * demand at whatever resolution the material asks for.
	 */
	{
		uint32_t checker =
			seed_texture("builtin://texture/checker",
				     CHECKER_TEXTURE_SCRIPT_SRC);

		seed_texture("builtin://texture/gradient",
			     GRADIENT_TEXTURE_SCRIPT_SRC);
		seed_texture("builtin://texture/noise",
			     NOISE_TEXTURE_SCRIPT_SRC);

		/*
		 * The scene-textured shader and a material that binds the checker
		 * through it — the built-ins that make a procedural texture render
		 * on a mesh. Seeded after the checker so its id is known for the
		 * material's texture slot; baked at 256x256 with the checker's own
		 * default params.
		 */
		{
			uint32_t tshader =
				seed_shader("builtin://shader/scene-textured",
					    SCENE_TEXTURED_SHADER_SRC);

			seed_textured_material("builtin://material/checker",
					       tshader, DEFAULT_MATERIAL_COLOR,
					       checker, 256, 256);
		}
	}

	/*
	 * The physically based shader and two materials that exercise it — a
	 * metal and a dielectric — so the metallic/roughness workflow ships with
	 * ready-to-bind examples the way the checker ships for the textured
	 * shader.  Neither carries a texture slot: the pbr shader is pure
	 * parametric (base_color/metallic/roughness), so seed_pbr_material writes
	 * only the shader-ref and the std140 Material block.
	 */
	{
		static const float GOLD[4]   = { 1.00f, 0.78f, 0.34f, 1.0f };
		static const float PLASTIC[4] = { 0.85f, 0.13f, 0.16f, 1.0f };
		uint32_t pshader = seed_shader("builtin://shader/pbr",
					       PBR_SHADER_SRC);

		seed_pbr_material("builtin://material/pbr-metal", pshader,
				  GOLD, 1.0f, 0.30f);
		seed_pbr_material("builtin://material/pbr-plastic", pshader,
				  PLASTIC, 0.0f, 0.45f);
	}

	/*
	 * Built-in sounds, authored the same way as the textures: each is a
	 * (sound NAME (sample (t) ...)) source, baked to a sound_blob on demand
	 * at whatever sample rate the audio context runs.
	 */
	seed_sound("builtin://sound/beep",        BEEP_SOUND_SCRIPT_SRC);
	seed_sound("builtin://sound/blip",        BLIP_SOUND_SCRIPT_SRC);
	seed_sound("builtin://sound/noise-burst", NOISE_BURST_SOUND_SCRIPT_SRC);
}

#ifdef __EMSCRIPTEN__

static void on_fetch_success(emscripten_fetch_t *fetch)
{
	struct asset_entry *e = (struct asset_entry *)fetch->userData;

	e->data = g_mem->alloc((size_t)fetch->numBytes);
	if (e->data) {
		memcpy(e->data, fetch->data, (size_t)fetch->numBytes);
		e->size  = (uint32_t)fetch->numBytes;
		e->state = ASSET_LOADED;
		g_log->write(LOG_LEVEL_INFO, "asset: loaded %s (%u bytes)",
			     e->path, e->size);
	} else {
		e->state = ASSET_ERROR;
		g_log->write(LOG_LEVEL_INFO,
			     "asset: out of memory loading %s", e->path);
	}
	emscripten_fetch_close(fetch);
}

static void on_fetch_error(emscripten_fetch_t *fetch)
{
	struct asset_entry *e = (struct asset_entry *)fetch->userData;

	e->state = ASSET_ERROR;
	g_log->write(LOG_LEVEL_INFO, "asset: error loading %s (HTTP %d)",
		     e->path, fetch->status);
	emscripten_fetch_close(fetch);
}

static void start_fetch(struct asset_entry *e)
{
	emscripten_fetch_attr_t attr;

	emscripten_fetch_attr_init(&attr);
	strncpy(attr.requestMethod, "GET", sizeof(attr.requestMethod) - 1);
	attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
	attr.userData   = e;
	attr.onsuccess  = on_fetch_success;
	attr.onerror    = on_fetch_error;
	emscripten_fetch(&attr, e->path);
}

#else /* native: synchronous filesystem read */

static void start_fetch(struct asset_entry *e)
{
	FILE    *f;
	long     len;
	uint8_t *buf;
	size_t   n;

	f = fopen(e->path, "rb");
	if (!f) {
		e->state = ASSET_ERROR;
		return;
	}

	fseek(f, 0, SEEK_END);
	len = ftell(f);
	rewind(f);

	if (len < 0) {
		fclose(f);
		e->state = ASSET_ERROR;
		return;
	}

	buf = g_mem->alloc((size_t)len);
	if (!buf) {
		fclose(f);
		e->state = ASSET_ERROR;
		return;
	}

	n = fread(buf, 1, (size_t)len, f);
	fclose(f);

	if (n != (size_t)len) {
		g_mem->free(buf);
		e->state = ASSET_ERROR;
		return;
	}

	e->data  = buf;
	e->size  = (uint32_t)len;
	e->state = ASSET_LOADED;
}

#endif /* __EMSCRIPTEN__ */

void asset_request(const char *path)
{
	struct asset_entry *e = find_entry(path);

	if (e) {
		e->refs++;
		return;
	}
	e = alloc_entry(path);
	if (!e) {
		g_log->write(LOG_LEVEL_INFO, "asset: cache full, dropping %s",
			     path);
		return;
	}
	e->refs = 1;
	start_fetch(e);
}

asset_state asset_state_of(const char *path)
{
	struct asset_entry *e = find_entry(path);

	return e ? (asset_state)e->state : ASSET_ERROR;
}

const void *asset_get(const char *path, uint32_t *out_size)
{
	struct asset_entry *e = find_entry(path);

	if (!e || e->state != ASSET_LOADED)
		return NULL;
	if (out_size)
		*out_size = e->size;
	return e->data;
}

void asset_release(const char *path)
{
	struct asset_entry *e = find_entry(path);

	if (!e || e->refs <= 0)
		return;
	if (e->read_only)
		return;
	e->refs--;
	/* Don't evict while a fetch is in flight — callback still holds *e. */
	if (e->refs == 0 && e->state != ASSET_PENDING)
		evict_entry(e);
}

/* Find the codec slot for ext, or NULL. */
static struct codec_entry *find_codec(const char *ext)
{
	int32_t i;

	for (i = 0; i < codec_count; i++) {
		if (strncmp(codec_table[i].ext, ext,
			    sizeof(codec_table[0].ext) - 1) == 0)
			return &codec_table[i];
	}
	return NULL;
}

/* Find the codec slot for ext, appending a fresh one if absent (or NULL if
 * the table is full).  Both register_codec and register_encoder attach to the
 * same slot, so one codec can hold both directions. */
static struct codec_entry *find_or_add_codec(const char *ext)
{
	struct codec_entry *e = find_codec(ext);

	if (e)
		return e;
	if (codec_count >= CODEC_TABLE_MAX)
		return NULL;
	e = &codec_table[codec_count++];
	strncpy(e->ext, ext, sizeof(e->ext) - 1);
	e->ext[sizeof(e->ext) - 1] = '\0';
	e->decode = NULL;
	e->encode = NULL;
	return e;
}

void asset_codec_register(const char *ext,
			  void *(*decode)(const void *bytes, uint32_t size))
{
	struct codec_entry *e = find_or_add_codec(ext);

	if (e)
		e->decode = decode;
}

void asset_codec_register_encoder(const char *ext,
				  void *(*encode)(const void *typed,
						  uint32_t *out_size))
{
	struct codec_entry *e = find_or_add_codec(ext);

	if (e)
		e->encode = encode;
}

void *asset_codec_decode_bytes(const char *ext, const void *bytes,
			       uint32_t size)
{
	struct codec_entry *e = find_codec(ext);

	if (!e || !e->decode)
		return NULL;
	return e->decode(bytes, size);
}

void *asset_codec_encode(const char *ext, const void *typed,
			 uint32_t *out_size)
{
	struct codec_entry *e = find_codec(ext);

	if (!e || !e->encode)
		return NULL;
	return e->encode(typed, out_size);
}

void *asset_codec_get_typed(const char *path)
{
	const char *dot;
	const char *ext;
	const void *bytes;
	uint32_t    size;
	int32_t     i;

	dot = strrchr(path, '.');
	if (!dot)
		return NULL;
	ext = dot + 1;

	bytes = asset_get(path, &size);
	if (!bytes)
		return NULL;

	for (i = 0; i < codec_count; i++) {
		if (strncmp(codec_table[i].ext, ext,
			    sizeof(codec_table[0].ext) - 1) == 0)
			return codec_table[i].decode(bytes, size);
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Catalog enumeration API                                             */
/* ------------------------------------------------------------------ */

uint32_t asset_catalog_count(void)
{
	return (uint32_t)cache_count;
}

static void fill_info(struct asset_info *out, const struct asset_entry *e)
{
	out->path      = e->path;
	out->state     = e->state;
	out->size      = e->size;
	out->refs      = e->refs;
	out->kind      = e->kind;
	out->read_only = e->read_only;
	out->type      = e->type;
	out->id        = e->id;
	out->origin    = e->origin;
}

int32_t asset_catalog_info(uint32_t i, struct asset_info *out)
{
	if ((int32_t)i >= cache_count || !out)
		return -1;
	fill_info(out, &cache[i]);
	return 0;
}

int32_t asset_catalog_find(uint32_t id, struct asset_info *out)
{
	struct asset_entry *e;

	if (!out)
		return -1;
	e = find_entry_by_id(id);
	if (!e)
		return -1;
	fill_info(out, e);
	return 0;
}

const void *asset_catalog_get_data(uint32_t id, uint32_t *out_size)
{
	struct asset_entry *e;

	e = find_entry_by_id(id);
	if (!e || e->state != ASSET_LOADED)
		return NULL;
	if (out_size)
		*out_size = e->size;
	return e->data;
}

/* ------------------------------------------------------------------ */
/* Built-in declaration descriptors                                    */
/* ------------------------------------------------------------------ */

/*
 * The box mesh is the first built-in to carry a (params ...) clause, so it
 * advertises those parameters the way a script advertises its params — a mesh's
 * authored inputs are first-class, like a shader's uniforms. Its geometry (and
 * so its bounds) depends on those params, so it advertises the default extents.
 */
static const struct asset_decl_field box_mesh_decl[] = {
	{ "format",     "krudd-mesh"            },
	{ "topology",   "triangles"             },
	{ "vertices",   "24"                    },
	{ "indices",    "36"                    },
	{ "attributes", "position, normal, uv0" },
	{ "params",     "width, height, depth"  },
};

static const struct asset_decl_field sphere_decl[] = {
	{ "format",        "krudd-mesh"            },
	{ "topology",      "triangles"             },
	{ "segments",      "rings 16, sectors 32"  },
	{ "vertices",      "561"                   },
	{ "indices",       "2880"                  },
	{ "attributes",    "position, normal, uv0" },
	{ "bounds.radius", "0.5"                   },
};

static const struct asset_decl_field plane_decl[] = {
	{ "format",     "krudd-mesh"            },
	{ "topology",   "triangles"             },
	{ "vertices",   "4"                     },
	{ "indices",    "6"                     },
	{ "attributes", "position, normal, uv0" },
	{ "normal",     "{ 0, 1, 0 }"           },
};

static const struct asset_decl_field pyramid_decl[] = {
	{ "format",     "krudd-mesh"            },
	{ "topology",   "triangles"             },
	{ "vertices",   "16"                    },
	{ "indices",    "18"                    },
	{ "attributes", "position, normal, uv0" },
};

/*
 * A shader asset is one DSL source holding every stage, so it advertises its
 * source format and the stages it defines (not a single stage per asset).  The
 * renderer lowers the DSL to whatever its backend speaks; a WebGPU/WGSL backend
 * slots in without the asset — or this metadata — changing. The scene-textured
 * shader is the engine's only built-in scene shader, so it also advertises the
 * albedo sampler it adds.
 */
static const struct asset_decl_field scene_textured_shader_decl[] = {
	{ "format",   "krudd-shader"     },
	{ "stages",   "vertex, fragment" },
	{ "samplers", "albedo"           },
};

/*
 * The built-in checker material advertises its shader and the texture it binds
 * — the way the shader built-ins advertise their format/stages — so the
 * inspector shows what a textured material is made of.
 */
static const struct asset_decl_field checker_material_decl[] = {
	{ "format",  "krudd-material"                  },
	{ "shader",  "builtin://shader/scene-textured" },
	{ "texture", "builtin://texture/checker @ 256" },
};

/*
 * The physically based shader advertises its format and stages like the scene
 * shader, plus the Material params it exposes — base_color/metallic/roughness —
 * the way a script decl lists its params, so the inspector shows what a pbr
 * material is made of.  It adds no sampler (pure parametric shading).
 */
static const struct asset_decl_field pbr_shader_decl[] = {
	{ "format", "krudd-shader"                      },
	{ "stages", "vertex, fragment"                  },
	{ "params", "base_color, metallic, roughness"   },
};

/* The two built-in pbr materials advertise their shader and the metallic/
 * roughness point each one picks, the way the checker material advertises its
 * shader and texture. */
static const struct asset_decl_field pbr_metal_material_decl[] = {
	{ "format", "krudd-material"        },
	{ "shader", "builtin://shader/pbr"  },
	{ "params", "metallic 1.0, roughness 0.30" },
};

static const struct asset_decl_field pbr_plastic_material_decl[] = {
	{ "format", "krudd-material"        },
	{ "shader", "builtin://shader/pbr"  },
	{ "params", "metallic 0.0, roughness 0.45" },
};

/*
 * A script asset is one (script NAME ...) Scheme form.  It advertises its
 * source format and the lifecycle hooks the built-in defines, the way a shader
 * advertises its stages.
 */
static const struct asset_decl_field spinner_script_decl[] = {
	{ "format", "krudd-script"      },
	{ "hooks",  "on-begin, on-tick" },
	{ "params", "speed"             },
};

static const struct asset_decl_field bounce_script_decl[] = {
	{ "format", "krudd-script" },
	{ "hooks",  "on-tick"      },
	{ "params", "height, rate" },
};

static const struct asset_decl_field wobble_script_decl[] = {
	{ "format", "krudd-script" },
	{ "hooks",  "on-tick"      },
	{ "params", "amp, rate"    },
};

/* pulse also advertises its authored parameters, the way the material decl
 * advertises base_color — a script's params are first-class, like a shader's. */
static const struct asset_decl_field pulse_script_decl[] = {
	{ "format", "krudd-script"    },
	{ "hooks",  "on-tick"         },
	{ "params", "amp, rate"       },
};

static const struct asset_decl_field orbit_camera_script_decl[] = {
	{ "format", "krudd-script"          },
	{ "hooks",  "on-tick"               },
	{ "params", "radius, height, speed, angle-offset" },
};

/*
 * A mesh asset is one (mesh NAME (generate () ...)) Scheme form. It
 * advertises its source format, the way a shader/script advertises theirs;
 * unlike a script there is no hook set to enumerate — a mesh always carries
 * exactly one generate clause.
 */
static const struct asset_decl_field grid_mesh_decl[] = {
	{ "format",   "krudd-mesh" },
	{ "topology", "triangles"  },
	{ "vertices", "25"         },
	{ "indices",  "96"         },
};

/*
 * The revolved primitives — cylinder/cone/disc/capsule/torus — advertise the
 * meridian profile they sweep and the sector count, the way sphere advertises
 * its rings/sectors: metadata an inspector shows, not something the generator
 * reads back.
 */
static const struct asset_decl_field cylinder_mesh_decl[] = {
	{ "format",     "krudd-mesh"            },
	{ "topology",   "triangles"             },
	{ "segments",   "sectors 24"            },
	{ "vertices",   "150"                   },
	{ "indices",    "432"                   },
	{ "attributes", "position, normal, uv0" },
};

static const struct asset_decl_field cone_mesh_decl[] = {
	{ "format",     "krudd-mesh"            },
	{ "topology",   "triangles"             },
	{ "segments",   "sectors 24"            },
	{ "vertices",   "100"                   },
	{ "indices",    "288"                   },
	{ "attributes", "position, normal, uv0" },
};

static const struct asset_decl_field disc_mesh_decl[] = {
	{ "format",     "krudd-mesh"            },
	{ "topology",   "triangles"             },
	{ "segments",   "sectors 24"            },
	{ "vertices",   "50"                    },
	{ "indices",    "144"                   },
	{ "attributes", "position, normal, uv0" },
};

static const struct asset_decl_field capsule_mesh_decl[] = {
	{ "format",     "krudd-mesh"            },
	{ "topology",   "triangles"             },
	{ "segments",   "sectors 24, cap 8"     },
	{ "vertices",   "450"                   },
	{ "indices",    "2448"                  },
	{ "attributes", "position, normal, uv0" },
};

static const struct asset_decl_field torus_mesh_decl[] = {
	{ "format",     "krudd-mesh"                },
	{ "topology",   "triangles"                 },
	{ "segments",   "sectors 24, tube 16"       },
	{ "vertices",   "425"                       },
	{ "indices",    "2304"                      },
	{ "attributes", "position, normal, uv0"     },
	{ "radius",     "major 0.35, minor 0.15"    },
};

/*
 * The parametric surfaces advertise their grid resolution and the params that
 * reshape them, the way the box advertises width/height/depth — the geometry
 * twin of a shader's uniforms.
 */
static const struct asset_decl_field superquadric_mesh_decl[] = {
	{ "format",     "krudd-mesh"            },
	{ "topology",   "triangles"             },
	{ "segments",   "u 32, v 24"            },
	{ "vertices",   "825"                   },
	{ "indices",    "4608"                  },
	{ "attributes", "position, normal, uv0" },
	{ "params",     "e1, e2"                },
};

static const struct asset_decl_field heightfield_mesh_decl[] = {
	{ "format",     "krudd-mesh"            },
	{ "topology",   "triangles"             },
	{ "segments",   "u 24, v 24"            },
	{ "vertices",   "625"                   },
	{ "indices",    "3456"                  },
	{ "attributes", "position, normal, uv0" },
	{ "params",     "amp, freq"             },
};

/*
 * The implicit surface advertises the field it marches and the grid it marches
 * it over, the way the parametric surfaces advertise their (u,v) resolution —
 * the constructive-solid-geometry twin of a swept profile.
 */
static const struct asset_decl_field sdf_rook_mesh_decl[] = {
	{ "format",     "krudd-mesh"                     },
	{ "topology",   "triangles"                      },
	{ "surface",    "signed distance field"          },
	{ "segments",   "marching cubes 40^3"            },
	{ "vertices",   "5962"                           },
	{ "indices",    "35760"                          },
	{ "attributes", "position, normal, uv0"          },
	{ "csg",        "smooth-union, subtract"         },
};

/*
 * A texture asset is one (texture NAME (shade (u v) ...)) Scheme form. It
 * advertises its source format and its authored params — resolution-independent,
 * so it reports no fixed dimensions (the material picks the bake size), the way a
 * mesh reports no fixed transform.
 */
static const struct asset_decl_field checker_texture_decl[] = {
	{ "format", "krudd-texture"          },
	{ "params", "scale, color-a, color-b" },
};

static const struct asset_decl_field gradient_texture_decl[] = {
	{ "format", "krudd-texture" },
	{ "params", "top, bottom"   },
};

static const struct asset_decl_field noise_texture_decl[] = {
	{ "format", "krudd-texture" },
	{ "params", "scale, seed"   },
};

struct builtin_desc {
	const char                   *path;
	const struct asset_decl_field *fields;
	uint32_t                       count;
};

#define ARRAY_SIZE(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

static const struct builtin_desc builtin_descs[] = {
	{ "builtin://mesh/box",     box_mesh_decl, ARRAY_SIZE(box_mesh_decl) },
	{ "builtin://mesh/sphere",  sphere_decl,  ARRAY_SIZE(sphere_decl)  },
	{ "builtin://mesh/plane",   plane_decl,   ARRAY_SIZE(plane_decl)   },
	{ "builtin://mesh/pyramid", pyramid_decl, ARRAY_SIZE(pyramid_decl) },
	{ "builtin://shader/scene-textured", scene_textured_shader_decl,
	  ARRAY_SIZE(scene_textured_shader_decl) },
	{ "builtin://material/checker", checker_material_decl,
	  ARRAY_SIZE(checker_material_decl) },
	{ "builtin://shader/pbr", pbr_shader_decl,
	  ARRAY_SIZE(pbr_shader_decl) },
	{ "builtin://material/pbr-metal", pbr_metal_material_decl,
	  ARRAY_SIZE(pbr_metal_material_decl) },
	{ "builtin://material/pbr-plastic", pbr_plastic_material_decl,
	  ARRAY_SIZE(pbr_plastic_material_decl) },
	{ "builtin://script/spinner", spinner_script_decl,
	  ARRAY_SIZE(spinner_script_decl) },
	{ "builtin://script/bounce", bounce_script_decl,
	  ARRAY_SIZE(bounce_script_decl) },
	{ "builtin://script/wobble", wobble_script_decl,
	  ARRAY_SIZE(wobble_script_decl) },
	{ "builtin://script/pulse", pulse_script_decl,
	  ARRAY_SIZE(pulse_script_decl) },
	{ "builtin://script/orbit-camera", orbit_camera_script_decl,
	  ARRAY_SIZE(orbit_camera_script_decl) },
	{ "builtin://mesh/grid", grid_mesh_decl,
	  ARRAY_SIZE(grid_mesh_decl) },
	{ "builtin://mesh/cylinder", cylinder_mesh_decl,
	  ARRAY_SIZE(cylinder_mesh_decl) },
	{ "builtin://mesh/cone", cone_mesh_decl,
	  ARRAY_SIZE(cone_mesh_decl) },
	{ "builtin://mesh/disc", disc_mesh_decl,
	  ARRAY_SIZE(disc_mesh_decl) },
	{ "builtin://mesh/capsule", capsule_mesh_decl,
	  ARRAY_SIZE(capsule_mesh_decl) },
	{ "builtin://mesh/torus", torus_mesh_decl,
	  ARRAY_SIZE(torus_mesh_decl) },
	{ "builtin://mesh/superquadric", superquadric_mesh_decl,
	  ARRAY_SIZE(superquadric_mesh_decl) },
	{ "builtin://mesh/heightfield", heightfield_mesh_decl,
	  ARRAY_SIZE(heightfield_mesh_decl) },
	{ "builtin://mesh/sdf-rook", sdf_rook_mesh_decl,
	  ARRAY_SIZE(sdf_rook_mesh_decl) },
	{ "builtin://texture/checker", checker_texture_decl,
	  ARRAY_SIZE(checker_texture_decl) },
	{ "builtin://texture/gradient", gradient_texture_decl,
	  ARRAY_SIZE(gradient_texture_decl) },
	{ "builtin://texture/noise", noise_texture_decl,
	  ARRAY_SIZE(noise_texture_decl) },
};

#define BUILTIN_DESC_COUNT ARRAY_SIZE(builtin_descs)

uint32_t asset_catalog_describe(uint32_t i,
				struct asset_decl_field *out,
				uint32_t max)
{
	const struct asset_entry *e;
	uint32_t    d;
	uint32_t    n;

	if ((int32_t)i >= cache_count || !out || max == 0)
		return 0;

	e = &cache[i];

	/* Authored assets carry their own (editor-set) declaration. */
	if (e->ndecl > 0) {
		n = (e->ndecl > max) ? max : e->ndecl;
		for (d = 0; d < n; d++) {
			out[d].key   = e->decl[d].key;
			out[d].value = e->decl[d].val;
		}
		return n;
	}

	/* Built-in primitives carry static descriptors keyed by path. */
	for (d = 0; d < BUILTIN_DESC_COUNT; d++) {
		if (strncmp(builtin_descs[d].path, e->path,
			    ASSET_PATH_MAX) != 0)
			continue;
		n = builtin_descs[d].count;
		if (n > max)
			n = max;
		memcpy(out, builtin_descs[d].fields,
		       n * sizeof(struct asset_decl_field));
		return n;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Mutation API — authored project assets                              */
/* ------------------------------------------------------------------ */

uint32_t asset_mut_create(const char *path, int32_t type,
			  const void *bytes, uint32_t size)
{
	struct asset_entry *e;
	uint8_t            *buf;

	if (!path || (!bytes && size > 0))
		return 0;
	if (find_entry(path))
		return 0;
	e = alloc_entry(path);
	if (!e)
		return 0;

	if (size > 0) {
		buf = g_mem->alloc((size_t)size);
		if (!buf) {
			/* Undo the alloc_entry; swap-remove ourselves. */
			cache_count--;
			return 0;
		}
		memcpy(buf, bytes, (size_t)size);
		e->data = buf;
	}

	e->size      = size;
	e->state     = ASSET_LOADED;
	e->kind      = ASSET_KIND_NORMAL;
	e->read_only = 0;
	e->origin    = ASSET_ORIGIN_AUTHORED;
	e->type      = type;
	e->refs      = 1;
	return e->id;
}

int32_t asset_mut_inject(uint32_t id, const char *path, int32_t type,
			 const void *bytes, uint32_t size)
{
	struct asset_entry *e;
	uint8_t            *buf;

	if (id == 0 || !path || (!bytes && size > 0))
		return -1;
	if (find_entry(path) || find_entry_by_id(id))
		return -1;
	e = alloc_entry(path);
	if (!e)
		return -1;

	if (size > 0) {
		buf = g_mem->alloc((size_t)size);
		if (!buf) {
			cache_count--;   /* undo the alloc_entry */
			return -1;
		}
		memcpy(buf, bytes, (size_t)size);
		e->data = buf;
	}

	e->id        = id;   /* restore the persisted identity, not a fresh one */
	e->size      = size;
	e->state     = ASSET_LOADED;
	e->kind      = ASSET_KIND_NORMAL;
	e->read_only = 0;
	e->origin    = ASSET_ORIGIN_AUTHORED;
	e->type      = type;
	e->refs      = 1;

	/* Keep the allocator monotonic and collision-free with the restored id. */
	if (id >= next_asset_id)
		next_asset_id = id + 1u;
	return 0;
}

int32_t asset_mut_set_data(uint32_t id, const void *bytes, uint32_t size)
{
	struct asset_entry *e;
	uint8_t            *buf;

	e = find_entry_by_id(id);
	if (!e || e->origin != ASSET_ORIGIN_AUTHORED)
		return -1;
	if (size > 0 && !bytes)
		return -1;

	if (size > 0) {
		buf = g_mem->alloc((size_t)size);
		if (!buf)
			return -1;
		memcpy(buf, bytes, (size_t)size);
	} else {
		buf = NULL;
	}

	if (e->data)
		g_mem->free(e->data);
	e->data  = buf;
	e->size  = size;
	e->state = ASSET_LOADED;
	return 0;
}

int32_t asset_mut_destroy(uint32_t id)
{
	struct asset_entry *e;

	e = find_entry_by_id(id);
	if (!e || e->origin != ASSET_ORIGIN_AUTHORED)
		return -1;
	evict_entry(e);
	return 0;
}

int32_t asset_mut_set_decl(uint32_t id, const struct asset_decl_field *fields,
			   uint32_t n)
{
	struct asset_entry *e;
	uint32_t            k;

	e = find_entry_by_id(id);
	if (!e || e->origin != ASSET_ORIGIN_AUTHORED)
		return -1;
	if (n > ASSET_DECL_MAX)
		return -1;
	if (n > 0 && !fields)
		return -1;

	/* Validate up front so a bad field leaves the existing decl intact. */
	for (k = 0; k < n; k++) {
		if (!fields[k].key || !fields[k].value)
			return -1;
	}

	for (k = 0; k < n; k++) {
		strncpy(e->decl[k].key, fields[k].key, ASSET_DECL_KEY_MAX - 1);
		e->decl[k].key[ASSET_DECL_KEY_MAX - 1] = '\0';
		strncpy(e->decl[k].val, fields[k].value, ASSET_DECL_VAL_MAX - 1);
		e->decl[k].val[ASSET_DECL_VAL_MAX - 1] = '\0';
	}
	e->ndecl = n;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Undo/redo recording — thin wrappers over the raw mutation ops        */
/* ------------------------------------------------------------------ */

/*
 * The "asset_mut" vtable points at these wrappers, not the raw ops, so every
 * caller of the mutable API is recorded. Each captures the asset's pre-edit
 * state, performs the raw mutation, then hands the before-state to
 * asset_edit_record(), which captures the after-state and pushes one reversible
 * command. Only a mutation that actually took effect is recorded. The raw ops
 * stay recording-free so the command's own apply/revert (which call them) never
 * re-enter the history — the same separation the scene adapter keeps between its
 * wrappers and the world ops.
 */
static uint32_t rec_create(const char *path, int32_t type,
			   const void *bytes, uint32_t size)
{
	const struct edit_api *edit   = resolve_edit();
	struct asset_snapshot *before = edit ? asset_snapshot_absent(g_mem)
					     : NULL;
	uint32_t               id;

	id = asset_mut_create(path, type, bytes, size);
	if (id != 0)
		asset_edit_record(edit, g_mem, id, before, "Create Asset", 0);
	else
		asset_snapshot_free(before, g_mem);
	return id;
}

static int32_t rec_set_data(uint32_t id, const void *bytes, uint32_t size)
{
	const struct edit_api *edit   = resolve_edit();
	struct asset_snapshot *before = edit ? asset_snapshot_capture(id, g_mem)
					     : NULL;
	int32_t                rc;

	rc = asset_mut_set_data(id, bytes, size);
	if (rc == 0)
		asset_edit_record(edit, g_mem, id, before, "Edit Asset",
				  asset_edit_key(id));
	else
		asset_snapshot_free(before, g_mem);
	return rc;
}

static int32_t rec_destroy(uint32_t id)
{
	const struct edit_api *edit   = resolve_edit();
	struct asset_snapshot *before = edit ? asset_snapshot_capture(id, g_mem)
					     : NULL;
	int32_t                rc;

	rc = asset_mut_destroy(id);
	if (rc == 0)
		asset_edit_record(edit, g_mem, id, before, "Delete Asset", 0);
	else
		asset_snapshot_free(before, g_mem);
	return rc;
}

static const struct asset_api catalog_api = {
	.count    = asset_catalog_count,
	.info     = asset_catalog_info,
	.describe = asset_catalog_describe,
	.find     = asset_catalog_find,
	.get_data = asset_catalog_get_data,
};

static const struct asset_codec_api codec_api = {
	.register_codec   = asset_codec_register,
	.get_typed        = asset_codec_get_typed,
	.register_encoder = asset_codec_register_encoder,
	.decode_bytes     = asset_codec_decode_bytes,
	.encode           = asset_codec_encode,
};

/*
 * create / set_data / destroy route through the recording wrappers so authored
 * edits land on the undo timeline. set_decl (metadata) and inject (id-preserving
 * rehydration from persistence, before the user authors anything) are not user
 * gestures and stay on the raw ops — inject in particular is what undo itself
 * calls to bring a destroyed asset back, so recording it would be circular.
 */
static const struct asset_mut_api mut_api = {
	.create   = rec_create,
	.set_data = rec_set_data,
	.destroy  = rec_destroy,
	.set_decl = asset_mut_set_decl,
	.inject   = asset_mut_inject,
};

static const struct subsystem codec_desc = {
	.name = "asset_codec",
	.api  = &codec_api,
};

static const struct subsystem mut_desc = {
	.name = "asset_mut",
	.api  = &mut_api,
};

void asset_init(void)
{
	seed_builtins();
	g_log->write(LOG_LEVEL_INFO, "asset: init");
}

static void asset_tick(void)
{
	/* WASM fetch callbacks update state directly; nothing to drain. */
}

static void asset_shutdown(void)
{
	int32_t i;

	for (i = 0; i < cache_count; i++) {
		if (cache[i].data)
			g_mem->free(cache[i].data);
	}
	cache_count = 0;
	g_log->write(LOG_LEVEL_INFO, "asset: shutdown");
}

static const struct subsystem desc = {
	.name     = "asset",
	.api      = &catalog_api,
	.init     = asset_init,
	.tick     = asset_tick,
	.shutdown = asset_shutdown,
};

void asset_plugin_entry(struct subsystem_manager *mgr)
{
#ifdef __EMSCRIPTEN__
	g_log = subsystem_manager_get_api(mgr, "log");
	g_mem = subsystem_manager_get_api(mgr, "memory");
#endif
	/* Stash for the lazy "edit" lookup: it registers after us. */
	g_mgr = mgr;
	subsystem_manager_register(mgr, &desc);
	subsystem_manager_register(mgr, &codec_desc);
	subsystem_manager_register(mgr, &mut_desc);
}
