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
 * block); the fragment stage shades from the world normal and the sun's
 * direction, multiplies in the material tint, then multiplies in the sampled
 * albedo — a material naming this shader is what makes a procedural texture
 * actually appear on a mesh.
 *
 * Like the pbr shader it reads the sun from the Sun block (slot 2) and casts
 * the sun's shadows: it declares the same std140 Sun block (so it reads the
 * same uploaded bytes), projects each vertex into light space (v_lightpos), and
 * does the same 3x3 PCF depth compare against the shadow_map, attenuating only
 * the directional half of its lighting. Two samplers now — albedo and
 * shadow_map — which the backend's alphabetical rule lands on units 0 and 1;
 * the scene renderer binds them to match.
 */
/*
 * Shared shader-DSL helpers, spliced into each built-in shader's (functions ...)
 * section at seed time (see the builtins-seed block) so the 3x3 PCF sun-shadow
 * lookup and the Reinhard+gamma tonemap curve are authored once here instead of
 * copied verbatim into every shader that shades the sun or writes a display
 * colour. The krudd shader DSL grew reusable helpers for exactly this; a helper
 * reads the shared samplers/blocks (here shadow_map) as globals but takes its
 * per-pixel inputs — the light-space position and the N·L term — as parameters,
 * so it lowers identically to GLSL and WGSL. The transpiler emits a helper only
 * in the stage that reaches it and still binds shadow_map there through its
 * transitive reference analysis, so the vertex stage and the sampler slots are
 * unchanged from the hand-inlined shaders these replace.
 *
 * sun_shadow(lp, ndl): projects lp into the shadow map, PCF-compares a 3x3
 * neighbourhood biased by (1 - ndl), and returns the [0,1] visibility (1.0 for
 * a fragment outside the light frustum). ndl is the surface's N·L — the pbr
 * shaders pass their ndl, scene-textured its diff, both the same quantity.
 */
static const char *SUN_SHADOW_FN =
	"    (sun_shadow ((lp vec4) (ndl float)) float\n"
	"      (let* ((proj  (/ (swizzle lp xyz) (swizzle lp w)))\n"
	"             (uvw   (+ (* proj 0.5) 0.5))\n"
	"             (su    (swizzle uvw x))\n"
	"             (sv    (swizzle uvw y))\n"
	"             (fragd (swizzle uvw z))\n"
	"             (bias  (max (* 0.0025 (- 1.0 ndl)) 0.0006))\n"
	"             (edge  (- fragd bias))\n"
	"             (tx    0.00048828125)\n"
	"             (b     (vec2 su sv))\n"
	"             (s0 (step edge (swizzle (sample shadow_map (+ b (vec2 (- 0.0 tx) (- 0.0 tx)))) r)))\n"
	"             (s1 (step edge (swizzle (sample shadow_map (+ b (vec2 0.0 (- 0.0 tx)))) r)))\n"
	"             (s2 (step edge (swizzle (sample shadow_map (+ b (vec2 tx (- 0.0 tx)))) r)))\n"
	"             (s3 (step edge (swizzle (sample shadow_map (+ b (vec2 (- 0.0 tx) 0.0))) r)))\n"
	"             (s4 (step edge (swizzle (sample shadow_map b) r)))\n"
	"             (s5 (step edge (swizzle (sample shadow_map (+ b (vec2 tx 0.0))) r)))\n"
	"             (s6 (step edge (swizzle (sample shadow_map (+ b (vec2 (- 0.0 tx) tx))) r)))\n"
	"             (s7 (step edge (swizzle (sample shadow_map (+ b (vec2 0.0 tx))) r)))\n"
	"             (s8 (step edge (swizzle (sample shadow_map (+ b (vec2 tx tx))) r)))\n"
	"             (pcf   (* (+ s0 s1 s2 s3 s4 s5 s6 s7 s8) 0.1111111))\n"
	"             (inrng (* (step 0.0 su) (step su 1.0) (step 0.0 sv)\n"
	"                       (step sv 1.0) (step fragd 1.0))))\n"
	"        (return (mix 1.0 pcf inrng))))\n";

/*
 * tonemap(color): the ACES filmic curve (Narkowicz's fitted approximation)
 * maps the HDR radiance to [0,1], then a gamma 1/2.2 encodes it for display.
 * ACES replaces the plainer Reinhard curve this used to run: it lifts the mid-
 * tones, rolls bright highlights toward white instead of desaturating them to
 * grey, and adds the gentle S-shaped contrast that reads as "filmic" — the
 * single cheapest step toward a cinematic look. Fitted num/den are evaluated
 * per channel, clamped to [0,1] (ACES can overshoot slightly), then encoded.
 * Shared by the pbr shaders, which shade in linear HDR; scene-textured writes
 * an already-[0,1] colour and needs none.
 */
static const char *TONEMAP_FN =
	"    (tonemap ((color vec3)) vec3\n"
	"      (let* ((num    (* color (+ (* 2.51 color) 0.03)))\n"
	"             (den    (+ (* color (+ (* 2.43 color) 0.59)) 0.14))\n"
	"             (mapped (clamp (/ num den)\n"
	"                            (vec3 0.0 0.0 0.0) (vec3 1.0 1.0 1.0))))\n"
	"        (return (pow mapped (vec3 0.4545 0.4545 0.4545)))))\n";

/*
 * Analytic image-based lighting, shared by the pbr shaders. Instead of a
 * captured HDRI cubemap (which needs cube-sampler support the DSL and both
 * backends still lack, plus prefiltering the WebGPU backend has no compute to
 * bake), the environment is a function of direction, evaluated per fragment —
 * the "mobile IBL" approach: it gives metals a real directional reflection and
 * dielectrics an environment-tinted fill for pure ALU, no textures.
 *
 *   env_radiance(dir): the environment seen along dir — a studio-ish gradient,
 *     cool sky at the zenith, a bright warm horizon band, dark ground. This is
 *     what a mirror-smooth surface reflects; sampling it in the reflection
 *     direction is the specular probe.
 *
 *   env_irradiance(n): the diffuse (cosine-integrated) version — a soft
 *     hemisphere with no horizon spike, standing in for the irradiance map. It
 *     doubles as the fully-rough specular, so a rough surface blurs toward it.
 *
 *   env_brdf_approx(rough, ndv): Karis's analytic fit of the split-sum BRDF
 *     integral (from "Physically Based Shading on Mobile") — returns the (scale,
 *     bias) that would otherwise come from a precomputed BRDF LUT, so specular
 *     IBL is F0*scale + bias. exp2 is folded into exp (exp2(x) = exp(x*ln2)),
 *     the DSL knowing exp but not exp2.
 */
static const char *ENV_IBL_FN =
	"    (env_radiance ((dir vec3)) vec3\n"
	"      (let* ((y       (swizzle dir y))\n"
	"             (sky     (vec3 0.55 0.68 0.92))\n"
	"             (ground  (vec3 0.16 0.15 0.14))\n"
	"             (horizon (vec3 1.0 0.96 0.88))\n"
	"             (base    (mix ground sky (+ 0.5 (* 0.5 y))))\n"
	"             (hb      (pow (- 1.0 (abs y)) 3.0)))\n"
	"        (return (mix base horizon (* 0.5 hb)))))\n"
	"    (env_irradiance ((n vec3)) vec3\n"
	"      (let* ((y      (swizzle n y))\n"
	"             (sky    (vec3 0.42 0.50 0.62))\n"
	"             (ground (vec3 0.20 0.19 0.17)))\n"
	"        (return (mix ground sky (+ 0.5 (* 0.5 y))))))\n"
	"    (env_brdf_approx ((rough float) (ndv float)) vec2\n"
	"      (let* ((c0 (vec4 -1.0 -0.0275 -0.572 0.022))\n"
	"             (c1 (vec4 1.0 0.0425 1.04 -0.04))\n"
	"             (r  (+ (* rough c0) c1))\n"
	"             (a004 (+ (* (min (* (swizzle r x) (swizzle r x))\n"
	"                              (exp (* -6.4324 ndv)))\n"
	"                         (swizzle r x))\n"
	"                      (swizzle r y))))\n"
	"        (return (+ (* (vec2 -1.04 1.04) a004) (swizzle r zw)))))\n";

static const char *SCENE_TEXTURED_HEAD =
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
	"    (Sun (block 2) (layout std140)\n"
	"      (light_dir       vec3)\n"
	"      (light_radiance  vec3)\n"
	"      (light_view_proj mat4))\n"
	"    (albedo     sampler2D)\n"
	"    (shadow_map depth2D))\n"
	"  (varyings\n"
	"    (v_normal   vec3)\n"
	"    (v_uv       vec2)\n"
	"    (v_lightpos vec4))\n"
	"  (targets\n"
	"    (frag_color vec4 (location 0)))\n"
	"  (functions\n";

static const char *SCENE_TEXTURED_TAIL =
	"  )\n"
	"  (vertex\n"
	"    (set v_normal (* (mat3 model) a_normal))\n"
	"    (set v_uv a_uv0)\n"
	"    (set v_lightpos (* light_view_proj model (vec4 a_pos 1.0)))\n"
	"    (set position (* view_proj model (vec4 a_pos 1.0))))\n"
	"  (fragment\n"
	"    (let* ((n    (normalize v_normal))\n"
	"           (base (+ 0.5 (* 0.5 n)))\n"
	"           (diff (max (dot n (normalize light_dir)) 0.0))\n"
	"           (shadow (sun_shadow v_lightpos diff))\n"
	"           (lit  (* base (+ 0.35 (* 0.65 diff shadow))))\n"
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
 * or a default sun when the scene has none.  Ambient is analytic image-based
 * lighting (see ENV_IBL_FN): the environment reflected in the view's mirror
 * direction feeds a split-sum specular term, and a soft irradiance hemisphere
 * feeds diffuse, so metals reflect a lit environment instead of going black.
 * The view direction is the real one — cam_pos (in the Camera block) minus the
 * interpolated world position — so both the specular highlight and the
 * environment reflection track the camera.
 *
 * The sun casts shadows: the Sun block also carries light_view_proj, the
 * light's world→clip matrix scene_renderer builds each frame, and the shader
 * samples a shadow_map (a depth texture the sun-shadow pass renders the scene
 * into from the light's viewpoint) at its own sampler unit.  The vertex stage
 * projects each vertex into light clip space (v_lightpos); the fragment does a
 * 3x3 PCF depth compare there and attenuates only the direct term, so a
 * surface the sun cannot see keeps its ambient fill but loses its key light.
 * A fragment outside the light frustum is treated as fully lit.  It samples no
 * albedo texture, so a material naming it carries no texture slot — the only
 * sampler it declares is the shadow map, which lands on unit 0.
 *
 * Block naming matters: the webgl backend assigns uniform-block binding slots
 * by sorting block NAMES alphabetically (GLSL ES 300 has no layout(binding=N)),
 * so the names must sort Camera < Material < Sun to land on slots 0/1/2 — the
 * slots scene_renderer binds them to.  "Sun" is chosen to sort after "Material"
 * for exactly that reason; renaming it below "Material" would silently swap the
 * Material and light bindings.
 */
static const char *PBR_HEAD =
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
	"      (roughness  float (edit range 0.0 1.0) (default 0.5))\n"
	"      (emissive   vec3  (edit color) (default 0.0 0.0 0.0)))\n"
	"    (Sun (block 2) (layout std140)\n"
	"      (light_dir       vec3)\n"
	"      (light_radiance  vec3)\n"
	"      (light_view_proj mat4))\n"
	"    (shadow_map depth2D))\n"
	"  (varyings\n"
	"    (v_normal   vec3)\n"
	"    (v_worldpos vec3)\n"
	"    (v_lightpos vec4))\n"
	"  (targets\n"
	"    (frag_color vec4 (location 0)))\n"
	"  (functions\n";

static const char *PBR_TAIL =
	"  )\n"
	"  (vertex\n"
	"    (set v_normal (* (mat3 model) a_normal))\n"
	"    (set v_worldpos (swizzle (* model (vec4 a_pos 1.0)) xyz))\n"
	"    (set v_lightpos (* light_view_proj model (vec4 a_pos 1.0)))\n"
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
	"           (shadow (sun_shadow v_lightpos ndl))\n"
	"           (lo     (* (+ diff spec) light_radiance ndl shadow))\n"
	"           (refl   (reflect (- v) n))\n"
	"           (pref   (mix (env_radiance refl) (env_irradiance n) roughness))\n"
	"           (eab    (env_brdf_approx roughness ndv))\n"
	"           (spec_i (* pref (+ (* f0 (swizzle eab x)) (swizzle eab y))))\n"
	"           (diff_i (* (env_irradiance n) albedo (- 1.0 metallic)))\n"
	"           (amb    (+ (* diff_i 0.7) spec_i))\n"
	"           (color  (+ amb lo emissive))\n"
	"           (gamma  (tonemap color)))\n"
	"      (set frag_color (vec4 gamma 1.0)))))\n";

/*
 * pbr-textured — the pbr shader plus one albedo sampler, for a material that
 * wants both a procedural texture and the metallic-roughness BRDF (checker
 * and scene-textured predate the pbr shader and never grew that combination;
 * this is the one built-in that has both). Same IO contract, same Cook-
 * Torrance fragment as the pbr shader, with two additions:
 *
 *   - v_uv rides along (a_uv0 was already a pbr input, just unused) and
 *     albedo tints base_color the way scene-textured's albedo tints its tint.
 *
 *   - the albedo texture's alpha channel doubles as a height field (see
 *     GRASS_TEXTURE_SCRIPT_SRC), and four extra taps around v_uv recover its
 *     screen-space slope (dh/du, dh/dv) — a cheap normal-mapping trick that
 *     needs no tangent-space basis because it perturbs the *world-space*
 *     normal directly in X/Z, which is exactly right for a mesh whose surface
 *     is close to the XZ plane (a flat or gently-waved ground/heightfield).
 *     It would misbehave on a steeply tilted or rotated mesh, but nothing
 *     built-in binds this shader to one. bump-strength (a Material param) is
 *     how far that gradient pushes the normal; 0 recovers the un-bumped pbr
 *     look exactly.
 */
/*
 * pbr-textured is HEAD + shared helpers + TAIL like the others. Folding the PCF
 * block into SUN_SHADOW_FN shrank the fragment enough that it no longer needs
 * the two-literal split the inlined version did to stay under the ISO C99 4095-
 * char concatenated-literal cap (-Wpedantic -Werror enforces it).
 */
static const char *PBR_TEXTURED_HEAD =
	"(shader pbr-textured\n"
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
	"      (base_color    vec4  (edit color) (default 1.0 1.0 1.0 1.0))\n"
	"      (metallic      float (edit range 0.0 1.0) (default 0.0))\n"
	"      (roughness     float (edit range 0.0 1.0) (default 0.8))\n"
	"      (bump_strength float (edit range 0.0 2.0) (default 0.5)))\n"
	"    (Sun (block 2) (layout std140)\n"
	"      (light_dir       vec3)\n"
	"      (light_radiance  vec3)\n"
	"      (light_view_proj mat4))\n"
	"    (albedo     sampler2D)\n"
	"    (shadow_map depth2D))\n"
	"  (varyings\n"
	"    (v_normal   vec3)\n"
	"    (v_worldpos vec3)\n"
	"    (v_uv       vec2)\n"
	"    (v_lightpos vec4))\n"
	"  (targets\n"
	"    (frag_color vec4 (location 0)))\n"
	"  (functions\n";

static const char *PBR_TEXTURED_TAIL =
	"  )\n"
	"  (vertex\n"
	"    (set v_normal (* (mat3 model) a_normal))\n"
	"    (set v_worldpos (swizzle (* model (vec4 a_pos 1.0)) xyz))\n"
	"    (set v_uv a_uv0)\n"
	"    (set v_lightpos (* light_view_proj model (vec4 a_pos 1.0)))\n"
	"    (set position (* view_proj model (vec4 a_pos 1.0))))\n"
	"  (fragment\n"
	"    (let* ((eps  0.012)\n"
	"           (hpx (swizzle (sample albedo (+ v_uv (vec2 eps 0.0))) a))\n"
	"           (hmx (swizzle (sample albedo (- v_uv (vec2 eps 0.0))) a))\n"
	"           (hpz (swizzle (sample albedo (+ v_uv (vec2 0.0 eps))) a))\n"
	"           (hmz (swizzle (sample albedo (- v_uv (vec2 0.0 eps))) a))\n"
	"           (dhdu (/ (- hpx hmx) (* 2.0 eps)))\n"
	"           (dhdv (/ (- hpz hmz) (* 2.0 eps)))\n"
	"           (bumped (vec3 (* (- 0.0 dhdu) bump_strength) 0.0\n"
	"                         (* (- 0.0 dhdv) bump_strength)))\n"
	"           (n      (normalize (+ (normalize v_normal) bumped)))\n"
	"           (l      (normalize light_dir))\n"
	"           (v      (normalize (- cam_pos v_worldpos)))\n"
	"           (h      (normalize (+ l v)))\n"
	"           (ndl    (max (dot n l) 0.0))\n"
	"           (ndv    (max (dot n v) 0.0001))\n"
	"           (ndh    (max (dot n h) 0.0))\n"
	"           (vdh    (max (dot v h) 0.0))\n"
	"           (tex_c  (swizzle (sample albedo v_uv) rgb))\n"
	"           (albedo (* (swizzle base_color rgb) tex_c))\n"
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
	"           (shadow (sun_shadow v_lightpos ndl))\n"
	"           (lo     (* (+ diff spec) light_radiance ndl shadow))\n"
	"           (refl   (reflect (- v) n))\n"
	"           (pref   (mix (env_radiance refl) (env_irradiance n) roughness))\n"
	"           (eab    (env_brdf_approx roughness ndv))\n"
	"           (spec_i (* pref (+ (* f0 (swizzle eab x)) (swizzle eab y))))\n"
	"           (diff_i (* (env_irradiance n) albedo (- 1.0 metallic)))\n"
	"           (amb    (+ (* diff_i 0.7) spec_i))\n"
	"           (color  (+ amb lo))\n"
	"           (gamma  (tonemap color)))\n"
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
 * Concatenate PARTS (a shader's HEAD, its shared helper fragments, and its TAIL)
 * into caller storage BUF, then seed the result. Splitting a built-in this way
 * keeps the shared helpers (SUN_SHADOW_FN, TONEMAP_FN) authored once and lets
 * each source stay a set of small literals well under the ISO C99 4095-char
 * concatenated-literal cap. Returns 0 if BUF is too small (a build-time sizing
 * error, not a runtime condition — the sources are fixed).
 */
static uint32_t seed_shader_parts(const char *path, char *buf, size_t cap,
				  const char *const *parts, size_t nparts)
{
	size_t off = 0;
	size_t i;

	for (i = 0; i < nparts; i++) {
		size_t l = strlen(parts[i]);

		if (off + l + 1 > cap)
			return 0;
		memcpy(buf + off, parts[i], l);
		off += l;
	}
	buf[off] = '\0';
	return seed_shader(path, buf);
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
 * { base_color vec4; metallic float; roughness float; emissive vec3; }, which
 * std140-packs to 48 bytes (base_color @0, metallic @16, roughness @20,
 * emissive @32, the block rounded up to 48), so the material is 4 + 48 = 52
 * bytes.  This seeder writes no emissive, leaving it zero (see the memset), so
 * these built-ins are non-emissive; the field exists for materials that author
 * a glow.  There is no texture slot: material_texture reads none because the
 * bytes end exactly at header + block, so the shader renders its pure
 * parametric shading.  A zero shader_ref makes this a no-op.
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
	n = (uint32_t)(sizeof(uint32_t)       /* shader-ref                 */
		       + 12 * sizeof(float)); /* std140 Material block (48B) */
	e->data = g_mem->alloc(n);
	if (!e->data) {
		e->state = ASSET_ERROR;
		return;
	}
	/*
	 * The zero fill is load-bearing: it leaves the block's pad AND the
	 * emissive vec3 (@32 in the block, so @36 here) at zero, so a material
	 * seeded through this path is non-emissive without naming emissive.
	 */
	memset(e->data, 0, n);
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
 * Seed a built-in pbr-textured material: the v3 wire form, [shader-ref u32]
 * followed by pbr-textured's { base_color vec4; metallic float; roughness
 * float; bump_strength float } Material block (std140-packs to 32 bytes:
 * base_color @0, metallic @16, roughness @20, bump_strength @24, the block
 * rounded up to 32 like seed_pbr_material's smaller block), then the same
 * [tex-ref u32][width u32][height u32] trailer seed_textured_material
 * appends after scene-textured's 16-byte block. The trailer lands at a
 * different absolute offset here (36, not 20) only because this block is
 * bigger; the renderer never hardcodes either offset — material_texture
 * (scene_renderer.c) locates the trailer generically from the shader's own
 * declared Material block size. A zero shader_ref or tex_ref makes this a
 * no-op.
 */
static void seed_pbr_textured_material(const char *path, uint32_t shader_ref,
					const float rgba[4], float metallic,
					float roughness, float bump_strength,
					uint32_t tex_ref, uint32_t width,
					uint32_t height)
{
	struct asset_entry *e;
	uint32_t            n;
	unsigned char      *p;

	if (!shader_ref || !tex_ref)
		return;
	e = alloc_entry(path);
	if (!e)
		return;
	n = (uint32_t)(sizeof(uint32_t)         /* shader-ref                */
		       + 8 * sizeof(float)      /* std140 Material block (32B) */
		       + 3 * sizeof(uint32_t)); /* tex-ref, width, height    */
	e->data = g_mem->alloc(n);
	if (!e->data) {
		e->state = ASSET_ERROR;
		return;
	}
	memset(e->data, 0, n);            /* leaves the block's tail pad zero */
	p = (unsigned char *)e->data;
	memcpy(p,      &shader_ref, sizeof(shader_ref));       /* @0  shader-ref    */
	memcpy(p + 4,  rgba, 4 * sizeof(float));               /* @4  base_color    */
	memcpy(p + 20, &metallic,      sizeof(metallic));      /* @20 metallic      */
	memcpy(p + 24, &roughness,     sizeof(roughness));     /* @24 roughness     */
	memcpy(p + 28, &bump_strength, sizeof(bump_strength)); /* @28 bump_strength */
	memcpy(p + 36, &tex_ref, sizeof(tex_ref));             /* @36 tex-ref       */
	memcpy(p + 40, &width,   sizeof(width));               /* @40 width         */
	memcpy(p + 44, &height,  sizeof(height));              /* @44 height        */
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

	/*
	 * The chess set — five lathed pieces plus the blocky knight (see
	 * builtin_mesh_scripts.h). games/chess binds these by path; kept here
	 * beside the other lathe built-ins since they showcase the same engine.
	 */
	seed_mesh("builtin://mesh/chess-pawn",   CHESS_PAWN_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/chess-rook",   CHESS_ROOK_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/chess-bishop", CHESS_BISHOP_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/chess-queen",  CHESS_QUEEN_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/chess-king",   CHESS_KING_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/chess-knight", CHESS_KNIGHT_MESH_SCRIPT_SRC);

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
			char        buf[8192];
			const char *parts[] = { SCENE_TEXTURED_HEAD,
						SUN_SHADOW_FN,
						SCENE_TEXTURED_TAIL };
			uint32_t    tshader = seed_shader_parts(
				"builtin://shader/scene-textured",
				buf, sizeof(buf), parts,
				sizeof(parts) / sizeof(parts[0]));

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
		char        pbuf[8192];
		const char *pparts[] = { PBR_HEAD, SUN_SHADOW_FN, TONEMAP_FN,
					 ENV_IBL_FN, PBR_TAIL };
		uint32_t pshader = seed_shader_parts(
			"builtin://shader/pbr", pbuf, sizeof(pbuf), pparts,
			sizeof(pparts) / sizeof(pparts[0]));

		seed_pbr_material("builtin://material/pbr-metal", pshader,
				  GOLD, 1.0f, 0.30f);
		seed_pbr_material("builtin://material/pbr-plastic", pshader,
				  PLASTIC, 0.0f, 0.45f);

		/*
		 * Gem-toned dielectrics off the same pbr shader: low metallic
		 * (a gem is not a metal — its colour comes from the dielectric
		 * base, not F0-tinted reflectance) and low roughness for a
		 * glassy, polished highlight. The pbr shader has no
		 * transmission/refraction term, so this reads as a deep,
		 * glossy stone rather than a faceted, light-bending gem — the
		 * honest ceiling of a metallic-roughness BRDF — but the
		 * saturated colour plus tight specular sells "ruby"/"sapphire"
		 * well enough for a game piece. tictactoe's marks and its win
		 * strike (games/tictactoe/rules.scm) bind these by winner.
		 */
		{
			static const float RUBY[4]     = { 0.55f, 0.02f, 0.06f, 1.0f };
			static const float SAPPHIRE[4] = { 0.03f, 0.10f, 0.55f, 1.0f };
			static const float STONE[4]    = { 0.78f, 0.74f, 0.64f, 1.0f };

			seed_pbr_material("builtin://material/pbr-ruby", pshader,
					  RUBY, 0.05f, 0.12f);
			seed_pbr_material("builtin://material/pbr-sapphire", pshader,
					  SAPPHIRE, 0.05f, 0.12f);
			/*
			 * A calm, neutral dielectric for a game board's playing
			 * squares — the "just one heightfield of texture, not
			 * nine checkered pads" replacement (games/tictactoe/
			 * scene.scm): matte sandstone rather than a busy pattern.
			 */
			seed_pbr_material("builtin://material/pbr-stone", pshader,
					  STONE, 0.0f, 0.75f);
		}

		/*
		 * The chess set's four materials, all off the same pbr shader
		 * (games/chess). The two piece materials are matte dielectrics
		 * (metallic 0): a warm near-white "ivory" for the white army and
		 * a near-black "ebony" for the black, both at a low-ish roughness
		 * so the analytic highlight gives turned pieces a soft sheen
		 * rather than a plastic glare. The two board materials are the
		 * light and dark squares — a warm maple and a deep walnut, matte
		 * (higher roughness) so the wood reads as a surface the polished
		 * pieces sit on, not a second set of mirrors.
		 */
		{
			static const float IVORY[4]  = { 0.90f, 0.85f, 0.74f, 1.0f };
			static const float EBONY[4]  = { 0.06f, 0.055f, 0.05f, 1.0f };
			static const float LIGHTSQ[4] = { 0.80f, 0.66f, 0.46f, 1.0f };
			static const float DARKSQ[4]  = { 0.28f, 0.17f, 0.10f, 1.0f };

			seed_pbr_material("builtin://material/chess-ivory", pshader,
					  IVORY, 0.0f, 0.32f);
			seed_pbr_material("builtin://material/chess-ebony", pshader,
					  EBONY, 0.0f, 0.28f);
			seed_pbr_material("builtin://material/board-light", pshader,
					  LIGHTSQ, 0.0f, 0.55f);
			seed_pbr_material("builtin://material/board-dark", pshader,
					  DARKSQ, 0.0f, 0.50f);
		}
	}

	/*
	 * Grass — the pbr-textured shader (metallic-roughness plus one albedo
	 * sampler and a height-from-alpha normal bump; see PBR_TEXTURED_HEAD/_TAIL)
	 * paired with the grass texture (GRASS_TEXTURE_SCRIPT_SRC), whose alpha
	 * channel IS the bump's height field — the "substance-designer-style"
	 * ground material games/tictactoe/scene.scm grounds its heightfield in.
	 * Baked at 512x512 (double the checker's 256) since grass tiles at a much
	 * higher spatial frequency and reads muddy if undersampled. The shader is
	 * assembled from its HEAD/TAIL and the shared helpers into a stack buffer
	 * before seeding — seed_shader copies it immediately, so it need not
	 * outlive this block.
	 */
	{
		char        shader_src[8192];
		const char *parts[] = { PBR_TEXTURED_HEAD, SUN_SHADOW_FN,
					TONEMAP_FN, ENV_IBL_FN,
					PBR_TEXTURED_TAIL };
		uint32_t    grass_tex =
			seed_texture("builtin://texture/grass",
				     GRASS_TEXTURE_SCRIPT_SRC);
		uint32_t    tshader2 = seed_shader_parts(
			"builtin://shader/pbr-textured", shader_src,
			sizeof(shader_src), parts,
			sizeof(parts) / sizeof(parts[0]));

		seed_pbr_textured_material("builtin://material/pbr-grass",
					   tshader2, DEFAULT_MATERIAL_COLOR,
					   0.0f, 0.9f, 0.6f,
					   grass_tex, 512, 512);
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
 * The chess pieces advertise their sweep the way the other lathe built-ins do:
 * a surface of revolution over a silhouette (the knight is the exception — a
 * lathed pedestal fused with box blocks, so it lists no single profile). The
 * vertex/index counts are the geometry's fingerprint, moving only when a piece's
 * silhouette in builtin_mesh_scripts.h does.
 */
static const struct asset_decl_field chess_pawn_mesh_decl[] = {
	{ "format",     "krudd-mesh"                },
	{ "topology",   "triangles"                 },
	{ "surface",    "revolution (mesh-lathe)"   },
	{ "segments",   "sectors 24, profile 15"    },
	{ "vertices",   "375"                       },
	{ "indices",    "2016"                      },
	{ "attributes", "position, normal, uv0"     },
};

static const struct asset_decl_field chess_rook_mesh_decl[] = {
	{ "format",     "krudd-mesh"                },
	{ "topology",   "triangles"                 },
	{ "surface",    "revolution (mesh-lathe)"   },
	{ "segments",   "sectors 24, profile 15"    },
	{ "vertices",   "375"                       },
	{ "indices",    "2016"                      },
	{ "attributes", "position, normal, uv0"     },
};

static const struct asset_decl_field chess_bishop_mesh_decl[] = {
	{ "format",     "krudd-mesh"                },
	{ "topology",   "triangles"                 },
	{ "surface",    "revolution (mesh-lathe)"   },
	{ "segments",   "sectors 24, profile 17"    },
	{ "vertices",   "425"                       },
	{ "indices",    "2304"                      },
	{ "attributes", "position, normal, uv0"     },
};

static const struct asset_decl_field chess_queen_mesh_decl[] = {
	{ "format",     "krudd-mesh"                },
	{ "topology",   "triangles"                 },
	{ "surface",    "revolution (mesh-lathe)"   },
	{ "segments",   "sectors 24, profile 19"    },
	{ "vertices",   "475"                       },
	{ "indices",    "2592"                      },
	{ "attributes", "position, normal, uv0"     },
};

static const struct asset_decl_field chess_king_mesh_decl[] = {
	{ "format",     "krudd-mesh"                },
	{ "topology",   "triangles"                 },
	{ "surface",    "revolution (mesh-lathe)"   },
	{ "segments",   "sectors 24, profile 19"    },
	{ "vertices",   "475"                       },
	{ "indices",    "2592"                      },
	{ "attributes", "position, normal, uv0"     },
};

static const struct asset_decl_field chess_knight_mesh_decl[] = {
	{ "format",     "krudd-mesh"                },
	{ "topology",   "triangles"                 },
	{ "surface",    "lathe + boxes (approx)"    },
	{ "segments",   "sectors 24, profile 9"     },
	{ "vertices",   "273"                       },
	{ "indices",    "1224"                      },
	{ "attributes", "position, normal, uv0"     },
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
	{ "builtin://mesh/chess-pawn", chess_pawn_mesh_decl,
	  ARRAY_SIZE(chess_pawn_mesh_decl) },
	{ "builtin://mesh/chess-rook", chess_rook_mesh_decl,
	  ARRAY_SIZE(chess_rook_mesh_decl) },
	{ "builtin://mesh/chess-bishop", chess_bishop_mesh_decl,
	  ARRAY_SIZE(chess_bishop_mesh_decl) },
	{ "builtin://mesh/chess-queen", chess_queen_mesh_decl,
	  ARRAY_SIZE(chess_queen_mesh_decl) },
	{ "builtin://mesh/chess-king", chess_king_mesh_decl,
	  ARRAY_SIZE(chess_king_mesh_decl) },
	{ "builtin://mesh/chess-knight", chess_knight_mesh_decl,
	  ARRAY_SIZE(chess_knight_mesh_decl) },
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
