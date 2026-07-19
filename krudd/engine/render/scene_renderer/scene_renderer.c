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
#include "particles.h"
#include "asset_api.h"
#include "memory_api.h"
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"

#include "s7.h"

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
 * Editor-chrome toggle: there is no editor mode left (games/demo and the
 * editor-chrome plumbing in plugin_abi.c/game.c were removed), so a wasm
 * build always takes the in-game outline path below — the picked-piece
 * outline (entity_api's get_outline), not the editor selection. Native
 * builds host no games and no chrome switch, so they always outline via
 * the editor selection path.
 */
#ifdef __EMSCRIPTEN__
#define EDITOR_CHROME() 0
#else
#define EDITOR_CHROME() 1
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
 * 16-byte material) renders with the built-in scene-textured pipeline exactly
 * as before.
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
/*
 * The multisampled twin of g_default_pso, used by the forward pass when it
 * renders into the multisampled offscreen scene target (the bloom/outline
 * paths on a backend that advertises GPU_CAP_MSAA_RESOLVE). When MSAA is off
 * it aliases g_default_pso, so nothing is compiled or destroyed twice.
 */
static gpu_pipeline_t g_default_pso_ms;
/*
 * Sample count for the scene's multisampled colour/depth targets and the
 * geometry pipelines that draw into them: 4 when the device can resolve MSAA,
 * else 1 (every target single-sample, no resolve — the pre-MSAA behaviour).
 * Fixed at init from the renderer caps.
 */
static uint32_t       g_msaa_samples = 1;
/*
 * Set for the duration of a forward pass that targets the multisampled
 * offscreen colour, so forward_pass selects the multisampled pipeline variant.
 * Cleared for the direct-to-backbuffer fallback (a single-sample target), which
 * must keep using the single-sample pipelines.
 */
static int            g_forward_msaa;
static gpu_buffer_t   g_ubo_ring;      /* Camera ring: one slot per draw */
static gpu_buffer_t   g_material_ring; /* Material ring: one slot per draw */
static uint32_t       g_ring_cursor;   /* bump allocator, shared by both rings */
static int            g_ring_overflowed; /* latches so the log fires once */
static gpu_buffer_t   g_light_ubo;    /* Sun: { light_dir, light_radiance } std140 */
static int            g_ready;

/*
 * Selection-outline resources (#selection-outline). When an entity is selected
 * the tick runs a three-pass graph instead of the plain forward pass: the scene
 * into an offscreen colour+depth target, the selected mesh alone into a 1-bit
 * silhouette mask, then a full-screen pass that samples both and paints a red
 * border where the mask's edge is. All are created once, off-frame, and stay
 * null (so the outline is silently skipped) if their pipelines fail to compile.
 */
static gpu_pipeline_t g_mask_pso;    /* selected mesh -> solid-white silhouette */
static gpu_pipeline_t g_outline_pso; /* full-screen edge-detect + composite     */
static gpu_buffer_t   g_fs_vbo;      /* full-screen triangle clip-space corners  */
static gpu_buffer_t   g_fs_ebo;
static gpu_buffer_t   g_outline_ubo; /* std140 { vec2 texel; vec4 color; }       */

/*
 * Bloom post resources (#bloom). The plain forward path renders the scene into
 * an offscreen colour, then extract -> blur H -> blur V -> composite adds a glow
 * around the bright part. The three pipelines share the full-screen quad above;
 * g_blur_ubo carries the separable blur's per-tap step. Null pipelines make the
 * tick fall back to a direct forward-to-backbuffer pass with no bloom, so bloom
 * is a pure add-on that never breaks ordinary rendering.
 */
static gpu_pipeline_t g_bloom_extract_pso;   /* threshold the bright part       */
static gpu_pipeline_t g_bloom_blur_pso;      /* separable 9-tap, run H then V    */
static gpu_pipeline_t g_bloom_composite_pso; /* add blurred bloom onto the scene */
/*
 * The composite targets the backbuffer when the outline is off, and an
 * offscreen "outline_lit" when it is on. Those are two different attachment
 * states — the backbuffer carries the backend's emulated depth, the offscreen
 * target carries none — and WebGPU validates a pipeline against the pass it
 * runs in, so one pipeline cannot serve both. Same reason the scene geometry
 * pipelines carry a multisampled variant; g_bloom_composite_to_bb selects.
 */
static gpu_pipeline_t g_bloom_composite_bb_pso;
static int            g_bloom_composite_to_bb;
static gpu_buffer_t   g_blur_ubo;            /* std140 { vec2 dir }              */
#define BLUR_UBO_FLOATS 4                    /* vec2 dir + 2 pad (std140 -> 16B) */

/*
 * Per-frame bloom transients + half-res size the tick publishes for the passes.
 * Three distinct blur targets (not a two-buffer ping-pong) so every hazard is a
 * plain read-after-write — extract->a, blur H a->b, blur V b->c, composite reads
 * c — which is the only ordering the frame graph the outline path uses tracks.
 */
static struct {
	fg_resource_t scene_color;
	fg_resource_t bloom_a;    /* extract out / blur-H in  */
	fg_resource_t bloom_b;    /* blur-H out / blur-V in   */
	fg_resource_t bloom_c;    /* blur-V out / composite in */
	uint32_t      half_w, half_h;
} g_bloom_frame;

/*
 * Sun shadow-map resources (#sun-shadows). Before the forward pass the tick
 * renders the scene's depth from the light's viewpoint into a square depth
 * target, and the pbr shader compares against it to shade the sun's shadows.
 * g_shadow_pso is the depth-only pipeline (a mask-style position-only shader),
 * g_shadow_res is the per-frame depth transient the forward pass samples, and
 * g_shadow_dummy is a 1x1 depth texture cleared to the far plane (1.0), bound
 * wherever no real shadow map is available (the off-frame mesh preview, or a
 * frame that skips the pass) so the shadow sample reads "unoccluded" rather
 * than garbage. It is a depth texture, not a colour one, because the shaders
 * declare shadow_map as depth2D -> texture_depth_2d and WebGPU rejects a colour
 * texture in a depth slot (WebGL was untyped here and did not care).
 * g_color_dummy is its colour counterpart, for an albedo slot with no texture
 * to bind (the preview does not upload a material's own texture).
 */
static gpu_pipeline_t g_shadow_pso;
static gpu_texture_t  g_shadow_dummy;
static gpu_texture_t  g_color_dummy;
static fg_resource_t  g_shadow_res;

#define SHADOW_MAP_DIM 2048          /* sun depth-map resolution (matches tx below)*/

/*
 * A minimal depth-only shader for the sun-shadow pass: it speaks the scene
 * vertex contract (a_pos at location 0, Camera at block 0) and writes only
 * position, transformed by the light's view_proj (uploaded into the Camera
 * block's view_proj slot). The colour output is declared but discarded — the
 * pass has no colour attachment — so all that lands is depth from the light.
 */
static const char *SHADOW_SHADER_SRC =
	"(shader sun_shadow\n"
	"  (inputs (a_pos vec3 (location 0)))\n"
	"  (uniforms\n"
	"    (Camera (block 0) (layout std140)\n"
	"      (view_proj mat4)\n"
	"      (model     mat4)))\n"
	"  (targets (frag_color vec4 (location 0)))\n"
	"  (vertex   (set position (* view_proj model (vec4 a_pos 1.0))))\n"
	"  (fragment (set frag_color (vec4 1.0 1.0 1.0 1.0))))\n";

/* Last viewport pixel size the UI reported (0 = unknown -> no outline pass). */
static float g_view_w, g_view_h;

/* The transients the composite pass samples, published per-frame by the tick. */
static struct {
	fg_resource_t scene_color;
	fg_resource_t mask;
} g_outline_frame;

#define OUTLINE_UBO_FLOATS 8         /* vec2 texel + 2 pad + vec4 color          */
#define OUTLINE_THICKNESS  2.0f      /* border half-width, in pixels             */

/*
 * Silhouette shader: the selected mesh drawn flat white. It speaks the scene
 * pipeline's vertex contract (a_pos at location 0, Camera block 0) so it reuses
 * the shared mesh vertex layout; normals/uvs go unread. Culling is off during
 * the pass, so the fragment set is the mesh's full screen-space footprint — a
 * 1-bit "is the selected object here" mask, no depth needed.
 */
static const char *MASK_SHADER_SRC =
	"(shader sel_mask\n"
	"  (inputs (a_pos vec3 (location 0)))\n"
	"  (uniforms\n"
	"    (Camera (block 0) (layout std140)\n"
	"      (view_proj mat4)\n"
	"      (model     mat4)))\n"
	"  (targets (frag_color vec4 (location 0)))\n"
	"  (vertex   (set position (* view_proj model (vec4 a_pos 1.0))))\n"
	"  (fragment (set frag_color (vec4 1.0 1.0 1.0 1.0))))\n";

/*
 * Outline/composite shader: a full-screen triangle that reads the offscreen
 * scene colour and the selection mask and returns the scene tinted red exactly
 * where a mask edge sits. Both samplers bind by the backend's alphabetical unit
 * rule (mask -> unit 0, scene -> unit 1). `texel` is the per-tap offset already
 * scaled by the border thickness; `color` is the outline colour. edge = (any of
 * eight ring taps is inside the mask) AND (this pixel is outside it), so the red
 * lands just outside the silhouette without covering the object.
 */
static const char *OUTLINE_SHADER_SRC =
	"(shader sel_outline\n"
	"  (inputs (a_pos vec2 (location 0)))\n"
	"  (uniforms\n"
	"    (Outline (block 0) (layout std140)\n"
	"      (texel vec2)\n"
	"      (color vec4))\n"
	"    (mask  sampler2D)\n"
	"    (scene sampler2D))\n"
	"  (varyings (v_uv vec2))\n"
	"  (targets (frag_color vec4 (location 0)))\n"
	"  (vertex\n"
	"    (set v_uv (+ (* a_pos 0.5) 0.5))\n"
	"    (set position (vec4 a_pos 0.0 1.0)))\n"
	"  (fragment\n"
	"    (let* ((tx   (swizzle texel x))\n"
	"           (ty   (swizzle texel y))\n"
	"           (here (swizzle (sample mask v_uv) r))\n"
	"           (n0 (swizzle (sample mask (+ v_uv (vec2 tx    0.0))) r))\n"
	"           (n1 (swizzle (sample mask (+ v_uv (vec2 (- 0.0 tx) 0.0))) r))\n"
	"           (n2 (swizzle (sample mask (+ v_uv (vec2 0.0    ty))) r))\n"
	"           (n3 (swizzle (sample mask (+ v_uv (vec2 0.0 (- 0.0 ty)))) r))\n"
	"           (n4 (swizzle (sample mask (+ v_uv (vec2 tx    ty))) r))\n"
	"           (n5 (swizzle (sample mask (+ v_uv (vec2 (- 0.0 tx) ty))) r))\n"
	"           (n6 (swizzle (sample mask (+ v_uv (vec2 tx (- 0.0 ty)))) r))\n"
	"           (n7 (swizzle (sample mask (+ v_uv (vec2 (- 0.0 tx) (- 0.0 ty)))) r))\n"
	"           (ring (max (max (max n0 n1) (max n2 n3))\n"
	"                      (max (max n4 n5) (max n6 n7))))\n"
	"           (edge (* ring (- 1.0 here)))\n"
	"           (col  (mix (swizzle (sample scene v_uv) rgb)\n"
	"                      (swizzle color rgb) edge)))\n"
	"      (set frag_color (vec4 col 1.0)))))\n";

/*
 * Bloom is a three-pass LDR post chain over the offscreen scene colour, run in
 * the plain (non-outline) forward path: extract the bright part, blur it
 * separably, then add it back. It operates on the tonemapped scene the material
 * shaders already wrote (each pbr surface tonemaps itself), so this is cheap
 * "display-space" bloom — a glow around bright speculars and emissive, not a
 * physically-correct HDR bloom. All three are full-screen-triangle shaders that
 * reuse the outline path's clip-space quad and UV convention.
 *
 * Extract: keep only what a luma threshold leaves, weighted so the knee is soft
 * rather than a hard cut. The scene is already [0,1], so the threshold is in
 * display space; 0.75 catches hot highlights and any emissive above mid-grey.
 */
static const char *BLOOM_EXTRACT_SHADER_SRC =
	"(shader bloom_extract\n"
	"  (inputs (a_pos vec2 (location 0)))\n"
	"  (uniforms (scene sampler2D))\n"
	"  (varyings (v_uv vec2))\n"
	"  (targets (frag_color vec4 (location 0)))\n"
	"  (vertex\n"
	"    (set v_uv (+ (* a_pos 0.5) 0.5))\n"
	"    (set position (vec4 a_pos 0.0 1.0)))\n"
	"  (fragment\n"
	"    (let* ((c (swizzle (sample scene v_uv) rgb))\n"
	"           (l (dot c (vec3 0.2126 0.7152 0.0722)))\n"
	"           (k (max (- l 0.75) 0.0))\n"
	"           (w (/ k (+ l 0.0001))))\n"
	"      (set frag_color (vec4 (* c w) 1.0)))))\n";

/*
 * Blur: one separable 9-tap Gaussian, run twice (horizontal then vertical) off
 * the same pipeline. `dir` is the per-tap texel step along the axis this pass
 * blurs — the tick sets it to (1/w, 0) then (0, 1/h) of the half-res target.
 * The weights are a normalised sigma~2 Gaussian (they sum to 1), so the pass
 * preserves total brightness.
 */
static const char *BLOOM_BLUR_SHADER_SRC =
	"(shader bloom_blur\n"
	"  (inputs (a_pos vec2 (location 0)))\n"
	"  (uniforms\n"
	"    (Blur (block 0) (layout std140)\n"
	"      (dir vec2))\n"
	"    (src sampler2D))\n"
	"  (varyings (v_uv vec2))\n"
	"  (targets (frag_color vec4 (location 0)))\n"
	"  (vertex\n"
	"    (set v_uv (+ (* a_pos 0.5) 0.5))\n"
	"    (set position (vec4 a_pos 0.0 1.0)))\n"
	"  (fragment\n"
	"    (let* ((s0 (* (swizzle (sample src v_uv) rgb) 0.2270270))\n"
	"           (s1 (* (+ (swizzle (sample src (+ v_uv (* dir 1.0))) rgb)\n"
	"                     (swizzle (sample src (- v_uv (* dir 1.0))) rgb)) 0.1945946))\n"
	"           (s2 (* (+ (swizzle (sample src (+ v_uv (* dir 2.0))) rgb)\n"
	"                     (swizzle (sample src (- v_uv (* dir 2.0))) rgb)) 0.1216216))\n"
	"           (s3 (* (+ (swizzle (sample src (+ v_uv (* dir 3.0))) rgb)\n"
	"                     (swizzle (sample src (- v_uv (* dir 3.0))) rgb)) 0.0540540))\n"
	"           (s4 (* (+ (swizzle (sample src (+ v_uv (* dir 4.0))) rgb)\n"
	"                     (swizzle (sample src (- v_uv (* dir 4.0))) rgb)) 0.0162162)))\n"
	"      (set frag_color (vec4 (+ s0 s1 s2 s3 s4) 1.0)))))\n";

/*
 * Composite: add the blurred bloom back onto the full-res scene. Two samplers,
 * bound by the backend's alphabetical unit rule (bloom -> 0, scene -> 1). The
 * add is scaled so the glow reads as a halo, not a wash.
 */
static const char *BLOOM_COMPOSITE_SHADER_SRC =
	"(shader bloom_composite\n"
	"  (inputs (a_pos vec2 (location 0)))\n"
	"  (uniforms (bloom sampler2D) (scene sampler2D))\n"
	"  (varyings (v_uv vec2))\n"
	"  (targets (frag_color vec4 (location 0)))\n"
	"  (vertex\n"
	"    (set v_uv (+ (* a_pos 0.5) 0.5))\n"
	"    (set position (vec4 a_pos 0.0 1.0)))\n"
	"  (fragment\n"
	"    (let* ((s (swizzle (sample scene v_uv) rgb))\n"
	"           (b (swizzle (sample bloom v_uv) rgb)))\n"
	"      (set frag_color (vec4 (+ s (* b 0.65)) 1.0)))))\n";

/*
 * The shared Camera uniform block, std140-packed: view_proj[16] + model[16] +
 * cam_pos (a vec3 that std140 aligns to a vec4, so 3 floats + 1 pad). A scene
 * shader that only needs the matrices (scene-textured, the selection mask)
 * declares just { view_proj, model } and reads the leading 128 bytes; the pbr
 * shader adds cam_pos and reads all 144, for a view direction that tracks the
 * real camera. The renderer uploads the full block regardless, so both kinds of
 * shader bind the same buffer.
 */
#define SCENE_UBO_FLOATS 36          /* view_proj[16] + model[16] + cam_pos[4] */
#define SCENE_UBO_CAMPOS 32          /* float offset of cam_pos within the block */

/*
 * The Sun uniform block (the pbr shader's directional key), std140-packed:
 * light_dir (vec3 -> vec4) + light_radiance (vec3 -> vec4) + light_view_proj
 * (mat4) = 8 + 16 = 24 floats. Bound at slot 2; the block name "Sun" sorts
 * after "Camera"/"Material", which is how the webgl backend (alphabetical
 * block-name order) lands it on binding 2. A shader without a Sun block
 * (scene-textured, the mask) simply ignores the slot. light_view_proj is the
 * light's world->clip matrix the tick rebuilds each frame; the pbr shader uses
 * it to project a fragment into the shadow map's space.
 */
#define LIGHT_UBO_FLOATS 24          /* light_dir[4] + light_radiance[4] + mat4[16] */
#define LIGHT_UBO_VP     8           /* float offset of light_view_proj in the block */

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
 * Per-frame uniform rings.
 *
 * A draw's uniforms must survive until the frame's command buffer executes, so
 * every draw takes its own slot rather than rewriting one buffer at offset 0.
 * Rewriting was correct only by accident of GL's execution model: GL commands
 * run in submission order, so each draw saw its own write. WebGPU's
 * buffer_update is wgpuQueueWriteBuffer, which lands on the queue timeline and
 * flushes entirely before the frame's command buffer runs — so every draw read
 * whatever the last entity wrote, and the whole scene collapsed onto one
 * transform and one material with no error logged.
 *
 * 256-byte strides: WebGPU's default minUniformBufferOffsetAlignment is 256 and
 * WebGL2's UNIFORM_BUFFER_OFFSET_ALIGNMENT is 256 on every target we ship, so
 * one constant satisfies both. Do not query it per backend.
 *
 * Single-buffered on purpose — no double-buffering, no fences. Queue order is
 * submit(N), writeBuffer(N+1)..., submit(N+1); WebGPU guarantees queue-timeline
 * ordering, so frame N+1's writes cannot clobber reads that frame N's
 * already-submitted command buffer will make.
 */
#define SCENE_MAX_DRAWS  1024                       /* 512 KB across both rings */
#define SCENE_UBO_ALIGN  256u
#define ALIGN_UP_256(n)  (((n) + (SCENE_UBO_ALIGN - 1u)) & ~(SCENE_UBO_ALIGN - 1u))
#define UBO_STRIDE       ALIGN_UP_256(SCENE_UBO_FLOATS * sizeof(float))
#define MATERIAL_STRIDE  ALIGN_UP_256(MATERIAL_UBO_MAX)

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
	/*
	 * The multisampled variant, drawn by the forward pass into the
	 * multisampled offscreen target. Aliases pso when MSAA is off (never
	 * compiled or destroyed twice).
	 */
	gpu_pipeline_t pso_ms;
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
 * The world entity that owns the camera's eye, or -1 when none is bound. The
 * camera is whatever live entity a scene names "Camera": the boot demo seeds
 * one bound to orbit-camera, and each launcher scene authors its own — a fixed
 * eye for tic-tac-toe, an orbit for the demo. scene_renderer_tick copies its
 * world_xform position into g_cam.eye each frame, after the scene subsystem has
 * ticked scripts (see the "scene" before "scene_renderer" registration order in
 * engine.c). target/up/fov stay fixed; only eye moves.
 *
 * This holds only a cache hint: it is re-validated by name every frame (see
 * resolve_camera_entity). Trusting it blindly would break on a world clear —
 * switching games tombstones the old camera and restarts entity ids from zero,
 * so a stale id would point at some other scene's entity (a board cell down at
 * floor level, say, hiding the board it was meant to frame).
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

/*
 * Adapt the camera's view_proj for the active backend's clip-space convention
 * before it reaches a vertex shader's output position. g_cam.view_proj is
 * GL-convention (NDC z in [-1, 1]); on a [0, 1]-clip backend (WebGPU) that
 * puts the near part of the frustum at clip z < 0, which the backend clips
 * away outright rather than just shading wrong (the same failure #608 fixed
 * for the shadow write). mat4_clip_z01 lifts it into [0, 1].
 *
 * Every draw path that outputs camera-space clip position (forward_pass,
 * mask_pass, draw_particles) calls this at its write site rather than
 * mutating g_cam.view_proj itself, so camera_get_view_proj keeps returning
 * the GL-convention matrix editor overlays and picking unproject against —
 * those run on the CPU and never touch a backend's clip volume.
 */
static struct mat4 camera_clip_vp(const struct gpu_api *gpu)
{
	struct mat4 vp = g_cam.view_proj;

	if (gpu->caps & GPU_CAP_CLIP_Z_ZERO_TO_ONE)
		mat4_clip_z01(&vp);
	return vp;
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
	if (width > 0.0f && height > 0.0f) {
		g_cam.aspect = width / height;
		/* Kept for sizing the outline pass's offscreen targets to the canvas. */
		g_view_w = width;
		g_view_h = height;
	}
}

static const struct camera_api g_camera_api = {
	camera_get_view_proj,
	camera_get_eye,
	camera_set_viewport,
};

/*
 * The scene's directional key light in world space, as the Sun block wants it.
 * Defaults to the historical fixed sun so a scene with no light entity looks
 * exactly as it did before lights existed; scene_renderer_tick overrides the
 * direction from the first live COMPONENT_LIGHT entity's world rotation. The
 * direction need not be unit length — the shader normalizes it. radiance is a
 * constant white for now; per-light colour/intensity is the next increment.
 */
static struct {
	float       dir[3];
	float       radiance[3];
	struct mat4 view_proj;   /* light world->clip, rebuilt each tick */
} g_light = {
	{ 0.5f, 0.8f, 0.4f },
	{ 1.0f, 1.0f, 1.0f },
	{ { 1.0f, 0.0f, 0.0f, 0.0f,
	    0.0f, 1.0f, 0.0f, 0.0f,
	    0.0f, 0.0f, 1.0f, 0.0f,
	    0.0f, 0.0f, 0.0f, 1.0f } },
};

/*
 * Rotate v by the unit quaternion q (xyzw); out must not alias v. Mirrors the
 * entity system's quat_rotate — a light entity's direction is its transform
 * rotation applied to the default sun vector, so rotating the entity (by gizmo
 * or script) steers the light.
 */
static void light_quat_rotate(const float q[4], const float v[3], float out[3])
{
	float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
	float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
	float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);

	out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
	out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
	out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

/*
 * A symmetric orthographic projection (half-extent HALF on x and y, depth range
 * [near, far]), right-handed with z mapped to GL NDC [-1, 1] — the same
 * convention as mat4_perspective, so a shadow depth compared against gl_Position
 * from this matrix matches the window depth the pass wrote. Column-major, filled
 * directly (there is no mat4_ortho in the math library). A directional light has
 * no perspective, so its shadow frustum is a box.
 *
 * A [0, 1]-clip backend needs the near half of this range lifted out of clip
 * rejection; shadow_pass adapts the built matrix (mat4_clip_z01) for its write
 * rather than baking a second convention in here.
 */
static void light_ortho(struct mat4 *out, float half, float near, float far)
{
	float rl = 2.0f * half;      /* r - l with l = -half, r = +half */
	float fn = far - near;

	memset(out->m, 0, sizeof(out->m));
	out->m[0]  = 2.0f / rl;
	out->m[5]  = 2.0f / rl;
	out->m[10] = -2.0f / fn;
	out->m[14] = -(far + near) / fn;
	out->m[15] = 1.0f;
}

/*
 * Rebuild g_light.view_proj — the sun's world->clip matrix the shadow pass
 * renders with and the pbr shader samples against. The light is directional, so
 * it is framed as an ortho box that encloses the scene: fit a bounding sphere to
 * the live drawable entities (their world positions padded by their scale, a
 * cheap stand-in for real mesh extents), place the light back along its
 * direction far enough to see the whole sphere, and look at the centre. With no
 * drawable entity the scene is empty, so any valid matrix does — the shadow map
 * clears to "far" and nothing is occluded.
 */
static void update_light_view_proj(const struct world *w)
{
	float    mn[3] = {  1e30f,  1e30f,  1e30f };
	float    mx[3] = { -1e30f, -1e30f, -1e30f };
	float    center[3], eye[3], up[3], dir[3], len, radius, dist;
	uint32_t i, k, seen = 0;

	if (w) {
		for (i = 0; i < w->count; i++) {
			const struct transform *t;
			float                    ext;

			if (!w->alive[i] || !(w->mask[i] & COMPONENT_RENDER) ||
			    !(w->mask[i] & COMPONENT_MATERIAL))
				continue;
			t   = &w->world_xform[i];
			ext = fabsf(t->scale[0]);
			if (fabsf(t->scale[1]) > ext) ext = fabsf(t->scale[1]);
			if (fabsf(t->scale[2]) > ext) ext = fabsf(t->scale[2]);
			for (k = 0; k < 3; k++) {
				float lo = t->position[k] - ext;
				float hi = t->position[k] + ext;

				if (lo < mn[k]) mn[k] = lo;
				if (hi > mx[k]) mx[k] = hi;
			}
			seen++;
		}
	}

	if (!seen) {
		mn[0] = mn[1] = mn[2] = -1.0f;
		mx[0] = mx[1] = mx[2] =  1.0f;
	}

	for (k = 0; k < 3; k++)
		center[k] = 0.5f * (mn[k] + mx[k]);
	radius = 0.5f * sqrtf((mx[0] - mn[0]) * (mx[0] - mn[0]) +
			      (mx[1] - mn[1]) * (mx[1] - mn[1]) +
			      (mx[2] - mn[2]) * (mx[2] - mn[2]));
	if (radius < 1.0f)
		radius = 1.0f;

	/* g_light.dir points from the surface toward the sun, so the light sits
	 * up-direction from the scene; step back along it to frame the sphere. */
	dir[0] = g_light.dir[0];
	dir[1] = g_light.dir[1];
	dir[2] = g_light.dir[2];
	len = sqrtf(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
	if (len < 1e-6f) {
		dir[0] = 0.0f; dir[1] = 1.0f; dir[2] = 0.0f; len = 1.0f;
	}
	dir[0] /= len; dir[1] /= len; dir[2] /= len;

	dist = radius * 2.5f;
	for (k = 0; k < 3; k++)
		eye[k] = center[k] + dir[k] * dist;

	/* Pick an up hint not parallel to the light direction. */
	if (fabsf(dir[1]) > 0.99f) {
		up[0] = 0.0f; up[1] = 0.0f; up[2] = 1.0f;
	} else {
		up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f;
	}

	{
		struct mat4 view, proj;

		mat4_look_at(&view, eye, center, up);
		light_ortho(&proj, radius * 1.15f, 0.05f, dist + radius * 1.5f);
		mat4_mul(&g_light.view_proj, &proj, &view);
	}
}

/*
 * Pack the current directional light into the Sun UBO and bind it at slot 2.
 * The light is constant across a pass, so a caller does this once before its
 * draw loop; a shader with no Sun block (scene-textured, the mask) ignores the
 * slot. Shared by the forward pass and the mesh-preview render so a pbr
 * thumbnail lights the same way the scene does.
 */
static void bind_light(const struct gpu_api *gpu, gpu_cmd_buf_t cmd)
{
	float lubo[LIGHT_UBO_FLOATS];

	lubo[0] = g_light.dir[0];
	lubo[1] = g_light.dir[1];
	lubo[2] = g_light.dir[2];
	lubo[3] = 0.0f;               /* std140 vec3 tail pad */
	lubo[4] = g_light.radiance[0];
	lubo[5] = g_light.radiance[1];
	lubo[6] = g_light.radiance[2];
	lubo[7] = 0.0f;
	memcpy(&lubo[LIGHT_UBO_VP], g_light.view_proj.m, 16 * sizeof(float));
	gpu->buffer_update(g_light_ubo, 0, lubo, (uint32_t)sizeof(lubo));
	gpu->cmd_bind_uniform_buffer(cmd, 2, g_light_ubo, 0,
				     (uint32_t)sizeof(lubo));
}

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
/*
 * COLOR_COUNT and WANT_DEPTH describe the attachments of the pass this pipeline
 * runs in. GL never coupled the two -- it discards a fragment colour written
 * with no colour attachment bound, and ignores a depth state with no depth
 * buffer -- but WebGPU validates a pipeline's attachment state against the pass
 * it is used in, in both directions. So the sun shadow pass (depth, no colour)
 * and the selection mask pass (colour, no depth) each have to say what they
 * actually target.
 */
static gpu_pipeline_t create_pso(const struct gpu_api *gpu, const char *src,
				 uint32_t color_count, int want_depth,
				 uint32_t sample_count)
{
	struct gpu_pipeline_desc pd;

	memset(&pd, 0, sizeof(pd));
	pd.color_formats[0]   = GPU_FORMAT_RGBA8_UNORM;
	pd.color_format_count = color_count;
	pd.depth_format       = want_depth ? GPU_FORMAT_DEPTH32_FLOAT
					   : GPU_FORMAT_UNKNOWN;
	pd.topology           = GPU_TOPOLOGY_TRIANGLE_LIST;
	/*
	 * A pipeline's sample count must match the pass it runs in. The scene
	 * geometry pipelines have a multisampled variant (sample_count > 1) for
	 * the offscreen MSAA target; the shadow (single-sample depth map) and
	 * mask (single-sample) pipelines pass 1.
	 */
	pd.sample_count       = sample_count ? sample_count : 1;

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
	const char *src      = shader_src_by_path("builtin://shader/scene-textured");
	uint32_t    scene_id = asset_id_by_path("builtin://shader/scene-textured");

	if (!src) {
		g_log->write(LOG_LEVEL_WARN,
			     "scene_renderer: scene shader asset unavailable");
		return;
	}
	g_default_pso = create_pso(gpu, src, 1, 1, 1);
	/*
	 * The multisampled twin for the offscreen forward pass. With MSAA off it
	 * is the same pipeline, so there is nothing extra to compile or free.
	 */
	g_default_pso_ms = g_msaa_samples > 1
				 ? create_pso(gpu, src, 1, 1, g_msaa_samples)
				 : g_default_pso;
	/*
	 * Cache the built-in pipeline under the scene-textured shader's id so a
	 * material that names it (the default) reuses this pipeline instead of
	 * compiling a second, identical one.
	 */
	if (g_default_pso && scene_id &&
	    g_shader_pso_count < SCENE_MAX_SHADER_PSOS) {
		g_shader_psos[g_shader_pso_count].shader_ref = scene_id;
		g_shader_psos[g_shader_pso_count].pso        = g_default_pso;
		g_shader_psos[g_shader_pso_count].pso_ms     = g_default_pso_ms;
		g_shader_psos[g_shader_pso_count].mat_block_size =
			shader_material_block_size(src);
		g_shader_pso_count++;
	}
}

/*
 * A pipeline for a full-screen pass: a single vec2 clip-space position at
 * location 0, no depth, both stages from one DSL source. Used for the outline
 * composite, which reads the offscreen scene and the mask and writes the
 * backbuffer.
 */
/*
 * WANT_DEPTH follows the same rule as create_pso: a pipeline has to describe
 * the pass it runs in, and WebGPU validates that in both directions. It is not
 * about the shader wanting depth — none of these read or write it — but about
 * whether the pass carries a depth attachment at all. A full-screen pass that
 * targets the BACKBUFFER does, because the backend emulates GL's
 * default-framebuffer depth there; one that targets an offscreen colour target
 * does not.
 */
static gpu_pipeline_t create_fullscreen_pso(const struct gpu_api *gpu,
					    const char *src, int want_depth)
{
	struct gpu_pipeline_desc pd;

	memset(&pd, 0, sizeof(pd));
	pd.color_formats[0]   = GPU_FORMAT_RGBA8_UNORM;
	pd.color_format_count = 1;
	pd.depth_format       = want_depth ? GPU_FORMAT_DEPTH32_FLOAT
					   : GPU_FORMAT_UNKNOWN;
	pd.topology           = GPU_TOPOLOGY_TRIANGLE_LIST;

	pd.vertex_layout.attr_count = 1;
	pd.vertex_layout.stride     = 2 * (uint32_t)sizeof(float);
	pd.vertex_layout.attrs[0]   =
		(struct gpu_vertex_attr){ 0, 0, GPU_FORMAT_RG32_FLOAT };

	pd.vert.src     = src;
	pd.vert.stage   = GPU_SHADER_STAGE_VERTEX;
	pd.vert.dialect = GPU_SHADER_DIALECT_KRUDD;
	pd.frag.src     = src;
	pd.frag.stage   = GPU_SHADER_STAGE_FRAGMENT;
	pd.frag.dialect = GPU_SHADER_DIALECT_KRUDD;

	return gpu->pipeline_create(&pd);
}

/*
 * Compile the selection-outline pipelines and their static buffers, off-frame.
 * The mask pipeline reuses the scene vertex layout (create_pso); the outline
 * pipeline is full-screen. A failure leaves the handles null and the tick falls
 * back to the plain forward pass, so the outline is a pure add-on that never
 * breaks ordinary rendering.
 */
static void build_outline_resources(const struct gpu_api *gpu)
{
	/* A full-screen triangle: three clip-space corners that cover [-1,1]^2. */
	static const float    FS_TRI[6] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
	static const uint16_t FS_IDX[3] = { 0, 1, 2 };
	struct gpu_buffer_desc bd;

	/* The mask renders into the single-sample sel_mask target, so 1 sample. */
	g_mask_pso    = create_pso(gpu, MASK_SHADER_SRC, 1, 0, 1);
	/* The outline pass draws into the backbuffer, which carries emulated depth. */
	g_outline_pso = create_fullscreen_pso(gpu, OUTLINE_SHADER_SRC, 1);
	if (!g_mask_pso || !g_outline_pso)
		g_log->write(LOG_LEVEL_WARN,
			     "scene_renderer: outline pipeline unavailable; "
			     "selection outline disabled");

	memset(&bd, 0, sizeof(bd));
	bd.usage        = GPU_BUFFER_USAGE_VERTEX;
	bd.size         = sizeof(FS_TRI);
	bd.initial_data = FS_TRI;
	g_fs_vbo = gpu->buffer_create(&bd);

	bd.usage        = GPU_BUFFER_USAGE_INDEX;
	bd.size         = sizeof(FS_IDX);
	bd.initial_data = FS_IDX;
	g_fs_ebo = gpu->buffer_create(&bd);

	memset(&bd, 0, sizeof(bd));
	bd.usage        = GPU_BUFFER_USAGE_UNIFORM;
	bd.size         = OUTLINE_UBO_FLOATS * sizeof(float);
	g_outline_ubo = gpu->buffer_create(&bd);
}

/*
 * Compile the three bloom pipelines and the blur's uniform buffer, off-frame.
 * All three are full-screen and reuse the quad build_outline_resources made, so
 * this must run after it. Any failed compile leaves a null handle and the tick
 * takes its no-bloom fallback, so bloom never breaks ordinary rendering.
 */
static void build_bloom_resources(const struct gpu_api *gpu)
{
	struct gpu_buffer_desc bd;

	g_bloom_extract_pso   = create_fullscreen_pso(gpu, BLOOM_EXTRACT_SHADER_SRC, 0);
	g_bloom_blur_pso      = create_fullscreen_pso(gpu, BLOOM_BLUR_SHADER_SRC, 0);
	g_bloom_composite_pso = create_fullscreen_pso(gpu, BLOOM_COMPOSITE_SHADER_SRC, 0);
	g_bloom_composite_bb_pso =
		create_fullscreen_pso(gpu, BLOOM_COMPOSITE_SHADER_SRC, 1);
	if (!g_bloom_extract_pso || !g_bloom_blur_pso || !g_bloom_composite_pso ||
	    !g_bloom_composite_bb_pso)
		g_log->write(LOG_LEVEL_WARN,
			     "scene_renderer: bloom pipeline unavailable; "
			     "bloom disabled");

	memset(&bd, 0, sizeof(bd));
	bd.usage = GPU_BUFFER_USAGE_UNIFORM;
	bd.size  = BLUR_UBO_FLOATS * sizeof(float);
	g_blur_ubo = gpu->buffer_create(&bd);
}

/*
 * Compile the sun-shadow depth pipeline and bake the 1x1 "fully lit" dummy
 * shadow map, off-frame. A failed compile leaves g_shadow_pso null, so the tick
 * skips the shadow pass and the forward pass binds the dummy — the pbr shader
 * then shades with the sun but casts no shadows, exactly as before this feature.
 * The dummy is opaque white so its red channel reads 1.0 (the far plane), which
 * the shader's depth compare always treats as unoccluded.
 */
static void build_shadow_resources(const struct gpu_api *gpu)
{
	static const unsigned char WHITE_TEXEL[4] = { 255, 255, 255, 255 };
	struct gpu_texture_desc     td;
	struct gpu_render_pass_desc rp;
	gpu_cmd_buf_t               cmd;

	/* The shadow map is a single-sample depth target, so 1 sample. */
	g_shadow_pso = create_pso(gpu, SHADOW_SHADER_SRC, 0, 1, 1);
	if (!g_shadow_pso)
		g_log->write(LOG_LEVEL_WARN,
			     "scene_renderer: shadow pipeline unavailable; "
			     "sun casts no shadows");

	/* Colour dummy: a 1x1 white texel for an albedo slot with nothing to
	 * bind (the preview does not upload the material's own texture). */
	memset(&td, 0, sizeof(td));
	td.format       = GPU_FORMAT_RGBA8_UNORM;
	td.width        = 1;
	td.height       = 1;
	td.mip_levels   = 1;
	td.sample_count = 1;
	td.initial_data = WHITE_TEXEL;
	g_color_dummy   = gpu->texture_create(&td);

	/*
	 * Shadow dummy: a 1x1 depth texture, because the shaders declare
	 * shadow_map as depth2D -> texture_depth_2d and WebGPU rejects a colour
	 * texture in that slot. A depth texture cannot be filled by a pixel
	 * upload (WebGPU has no queue-write to the depth aspect), so it is cleared
	 * to the far plane (1.0) — which the depth compare reads as unoccluded —
	 * with a one-shot depth-only pass instead of initial_data.
	 */
	memset(&td, 0, sizeof(td));
	td.format       = GPU_FORMAT_DEPTH32_FLOAT;
	td.width        = 1;
	td.height       = 1;
	td.mip_levels   = 1;
	td.sample_count = 1;
	g_shadow_dummy  = gpu->texture_create(&td);
	if (!g_shadow_dummy)
		return;

	memset(&rp, 0, sizeof(rp));
	rp.color_count    = 0;
	rp.depth          = g_shadow_dummy;
	rp.depth_load_op  = GPU_LOAD_OP_CLEAR;
	rp.depth_store_op = GPU_STORE_OP_STORE;
	rp.clear_depth    = 1.0f;

	cmd = gpu->cmd_buf_begin();
	gpu->cmd_begin_render_pass(cmd, &rp);
	gpu->cmd_end_render_pass(cmd);
	gpu->cmd_buf_submit(cmd);
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
	gpu_pipeline_t pso_ms;

	if (g_shader_pso_count >= SCENE_MAX_SHADER_PSOS) {
		g_log->write(LOG_LEVEL_WARN,
			     "scene_renderer: shader pipeline cache full; "
			     "shader %u uses the default", shader_ref);
		return;
	}
	src = (const char *)g_asset->get_data(shader_ref, NULL);
	pso = src ? create_pso(gpu, src, 1, 1, 1) : 0;
	if (!pso)
		g_log->write(LOG_LEVEL_WARN,
			     "scene_renderer: shader %u failed to compile; "
			     "using the default", shader_ref);
	/*
	 * The multisampled variant, for when this material is drawn in the
	 * offscreen forward pass. With MSAA off (or a failed compile) it aliases
	 * pso, so there is nothing extra to build or free.
	 */
	pso_ms = (pso && g_msaa_samples > 1)
			 ? create_pso(gpu, src, 1, 1, g_msaa_samples)
			 : pso;

	g_shader_psos[g_shader_pso_count].shader_ref = shader_ref;
	g_shader_psos[g_shader_pso_count].pso        = pso;
	g_shader_psos[g_shader_pso_count].pso_ms     = pso_ms;
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
 * The multisampled twin of pso_for_shader, for the forward pass when it targets
 * the multisampled offscreen scene colour. Falls back to the multisampled
 * default, which itself aliases the single-sample default when MSAA is off.
 */
static gpu_pipeline_t pso_for_shader_ms(uint32_t shader_ref)
{
	struct shader_pso *e;

	if (shader_ref) {
		e = find_shader_pso(shader_ref);
		if (e && e->pso_ms)
			return e->pso_ms;
	}
	return g_default_pso_ms;
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
 * Take the next uniform-ring slot for a draw. One cursor feeds both rings, so a
 * draw's camera slot and material slot share an index.
 *
 * The cursor is reset exactly once per frame, ahead of every pass, and NOT
 * between passes: shadow and forward are separate command buffers inside one
 * frame, they share the queue timeline, and so they must share the ring.
 *
 * On overflow this returns 0 (failure) rather than wrapping to slot 0. Wrapping
 * would reproduce exactly the silent wrong-picture failure the ring exists to
 * kill; a visibly missing object plus one error line is strictly better than a
 * plausible wrong one.
 */
static int ring_take_slot(uint32_t *out_slot)
{
	if (g_ring_cursor >= SCENE_MAX_DRAWS) {
		if (!g_ring_overflowed) {
			g_ring_overflowed = 1;
			g_log->write(LOG_LEVEL_ERROR,
				     "scene_renderer: uniform ring overflow "
				     "(> SCENE_MAX_DRAWS draws in a frame); "
				     "skipping the remaining draws");
		}
		return 0;
	}
	*out_slot = g_ring_cursor++;
	return 1;
}

/*
 * Render mesh_ref shaded with material_ref into the preview target and return
 * the color texture's opaque id (see preview_api.h). Drives the device
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
		material_ref = asset_id_by_path("builtin://material/checker");
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

	/*
	 * Adapt the clip-space depth range the same way every live draw path
	 * does (forward_pass, mask_pass, draw_particles, the shadow write): a
	 * backend with NDC z in [0, 1] needs mat4_clip_z01, or the near half of
	 * the frustum lands at z < 0 and gets clipped away. camera_clip_vp reads
	 * the global camera, so the preview — which frames its own local camera —
	 * does the same adaptation here by hand.
	 */
	{
		struct mat4 preview_vp = cam.view_proj;

		if (gpu->caps & GPU_CAP_CLIP_Z_ZERO_TO_ONE)
			mat4_clip_z01(&preview_vp);
		memcpy(&ubo[0], preview_vp.m, 16 * sizeof(float));
	}
	memcpy(&ubo[16], model.m,         16 * sizeof(float));
	ubo[SCENE_UBO_CAMPOS + 0] = cam.eye[0];
	ubo[SCENE_UBO_CAMPOS + 1] = cam.eye[1];
	ubo[SCENE_UBO_CAMPOS + 2] = cam.eye[2];
	ubo[SCENE_UBO_CAMPOS + 3] = 0.0f; /* std140 vec3 tail pad */

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
	{
		uint32_t slot, uoff, moff;

		if (!ring_take_slot(&slot)) {
			gpu->cmd_end_render_pass(cmd);
			gpu->cmd_buf_submit(cmd);
			return 0;
		}
		uoff = slot * (uint32_t)UBO_STRIDE;
		moff = slot * (uint32_t)MATERIAL_STRIDE;
		gpu->buffer_update(g_ubo_ring, uoff, ubo, (uint32_t)sizeof(ubo));
		gpu->cmd_bind_uniform_buffer(cmd, 0, g_ubo_ring, uoff,
					     (uint32_t)sizeof(ubo));
		gpu->buffer_update(g_material_ring, moff, params, plen);
		gpu->cmd_bind_uniform_buffer(cmd, 1, g_material_ring, moff, plen);
	}
	bind_light(gpu, cmd); /* Sun at slot 2, so a pbr thumbnail lights right */
	/*
	 * The preview runs no shadow pass, so the depth dummy stands in for the
	 * shadow map — but it has to land in the right slot with the right type. A
	 * textured shader puts albedo at unit 0 and the shadow map at unit 1; an
	 * untextured one puts the shadow map at unit 0. Mirror the forward pass's
	 * material_texture split so WebGPU sees a depth texture in the depth slot
	 * and a colour texture in the albedo slot (WebGL is untyped, unchanged).
	 */
	{
		uint32_t       t_ref, tw, th, tpl = 0;
		const uint8_t *tp = NULL;

		if (material_texture(material_ref, shader_ref, &t_ref, &tw, &th,
				     &tp, &tpl)) {
			if (g_color_dummy)
				gpu->cmd_bind_texture(cmd, 0, g_color_dummy);
			if (g_shadow_dummy)
				gpu->cmd_bind_texture(cmd, 1, g_shadow_dummy);
		} else if (g_shadow_dummy) {
			gpu->cmd_bind_texture(cmd, 0, g_shadow_dummy);
		}
	}
	gpu->cmd_bind_vertex_buffer(cmd, 0, g_prev_vbo, 0);
	gpu->cmd_bind_index_buffer(cmd, g_prev_ebo, 0, GPU_INDEX_FORMAT_UINT16);

	memset(&draw, 0, sizeof(draw));
	draw.index_count    = g_prev_index_count;
	draw.instance_count = 1;
	gpu->cmd_draw_indexed(cmd, &draw);

	gpu->cmd_end_render_pass(cmd);
	gpu->cmd_buf_submit(cmd);

	return gpu->texture_handle(g_prev_color);
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
		float       scale[3];
		const char *script;   /* behavior script to bind, or NULL      */
		const char *material; /* material asset to wear                 */
		const char *name;     /* shown in the entity list              */
	} DEMO[] = {
		{ "builtin://mesh/plane",   { 0.0f, -0.5f,  0.0f }, { 6.0f, 1.0f, 6.0f }, NULL,                       "builtin://material/checker",     "Floor"   },
		{ "builtin://mesh/box",     { -1.5f, 0.0f,  0.0f }, { 1.0f, 1.0f, 1.0f }, "builtin://script/spinner", "builtin://material/pbr-plastic", "Box"     },
		{ "builtin://mesh/sphere",  {  0.0f, 0.0f, -1.0f }, { 1.0f, 1.0f, 1.0f }, "builtin://script/bounce",  "builtin://material/pbr-metal",   "Sphere"  },
		{ "builtin://mesh/pyramid", {  1.5f, 0.0f,  0.5f }, { 1.0f, 1.0f, 1.0f }, "builtin://script/wobble",  "builtin://material/checker",     "Pyramid" },
		{ "builtin://mesh/sdf-rook",{  0.0f, 0.0f,  1.4f }, { 1.0f, 1.0f, 1.0f }, "builtin://script/spinner", "builtin://material/pbr-metal",   "Rook"    },
	};
	/* The floor bakes the checker at a denser scale than the built-in
	 * default so it reads as a checkerboard rather than one giant tile. */
	static const float  FLOOR_TEX_SCALE = 12.0f;
	const struct world *w;
	uint32_t            i;
	uint32_t            checker;

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
	 * Every seeded entity wears a real, inspectable material, so the world
	 * scene never rests in the "no material" state (forward_pass skips any
	 * entity with no COMPONENT_MATERIAL — how an entity keeps its mesh for
	 * picking/collision but stops drawing). The mix shows off both scene
	 * shaders side by side: the sphere, box, and rook wear the physically based
	 * metal/plastic materials, while the floor and pyramid wear the textured
	 * checker so the procedural-texture path stays exercised too. The rook is
	 * the marching-cubes/SDF mesh — its gradient normals catch the pbr key
	 * light — so the demo now covers all three mesh shape engines (lathe,
	 * parametric grid, implicit surface). A material that fails to resolve
	 * falls back to the checker rather than going undrawn.
	 */
	checker = asset_id_by_path("builtin://material/checker");

	for (i = 0; i < (uint32_t)(sizeof(DEMO) / sizeof(DEMO[0])); i++) {
		struct transform t;
		int32_t          id;
		uint32_t         ref = asset_id_by_path(DEMO[i].path);
		uint32_t         mat = asset_id_by_path(DEMO[i].material);

		if (!ref)
			continue;
		if (!mat)
			mat = checker;
		memset(&t, 0, sizeof(t));
		t.position[0] = DEMO[i].pos[0];
		t.position[1] = DEMO[i].pos[1];
		t.position[2] = DEMO[i].pos[2];
		t.rotation[3] = 1.0f;
		t.scale[0] = DEMO[i].scale[0];
		t.scale[1] = DEMO[i].scale[1];
		t.scale[2] = DEMO[i].scale[2];
		id = g_scene->create_entity(WORLD_NO_PARENT, &t, 0u, ref);
		if (id >= 0 && mat && g_scene->set_material_ref)
			g_scene->set_material_ref(id, mat);
		if (id >= 0 && g_scene->set_name)
			g_scene->set_name(id, DEMO[i].name);

		if (id >= 0 && strcmp(DEMO[i].path, "builtin://mesh/plane") == 0 &&
		    g_scene->set_texture_params)
			g_scene->set_texture_params(id,
						    (const uint8_t *)&FLOOR_TEX_SCALE,
						    sizeof(FLOOR_TEX_SCALE));

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

	/*
	 * A light entity (proof of the light component): no mesh or material, just
	 * COMPONENT_LIGHT. The tick reads its world rotation to steer the pbr
	 * shader's key light; at identity rotation that reproduces the default sun,
	 * so the scene looks the same as before but the light is now a real,
	 * selectable entity you can rotate (bind a spinner to it to sweep the sun).
	 */
	{
		struct transform lt;
		int32_t          light_id;

		memset(&lt, 0, sizeof(lt));
		lt.rotation[3] = 1.0f;
		lt.scale[0] = lt.scale[1] = lt.scale[2] = 1.0f;
		light_id = g_scene->create_entity(WORLD_NO_PARENT, &lt,
						  COMPONENT_LIGHT, 0u);
		if (light_id >= 0 && g_scene->set_name)
			g_scene->set_name(light_id, "Sun");
	}
}

/*
 * (particle-burst! x y z r g b count): spawn COUNT cosmetic particles at world
 * (x,y,z) tinted (r,g,b). The render layer owns this primitive — not the entity
 * layer's scene-* set — because particles are an effect on top of the scene, not
 * scene-graph state; a game's rules (tic-tac-toe fires it on each placement) call
 * it through the shared image with no C glue of their own. COUNT is clamped to
 * the pool inside particles_burst; a negative or absurd count is coerced to a
 * bounded unsigned there.
 */
static s7_pointer sp_particle_burst(s7_scheme *sc, s7_pointer args)
{
	float    pos[3], rgb[3];
	double   n;
	uint32_t count;

	pos[0] = (float)s7_number_to_real(sc, s7_list_ref(sc, args, 0));
	pos[1] = (float)s7_number_to_real(sc, s7_list_ref(sc, args, 1));
	pos[2] = (float)s7_number_to_real(sc, s7_list_ref(sc, args, 2));
	rgb[0] = (float)s7_number_to_real(sc, s7_list_ref(sc, args, 3));
	rgb[1] = (float)s7_number_to_real(sc, s7_list_ref(sc, args, 4));
	rgb[2] = (float)s7_number_to_real(sc, s7_list_ref(sc, args, 5));
	n      = s7_number_to_real(sc, s7_list_ref(sc, args, 6));
	count  = (n > 0.0) ? (uint32_t)n : 0u;

	particles_burst(pos, rgb, count);
	return s7_nil(sc);
}

/* Register particle-burst! into the shared image, so scene rules can fire it. */
static void register_particle_script(void)
{
	s7_scheme *sc = script_s7();

	if (sc)
		s7_define_function(sc, "particle-burst!", sp_particle_burst,
				   7, 0, false,
				   "(particle-burst! x y z r g b count)");
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

	/*
	 * 4x MSAA on a backend that can resolve a multisampled colour target to a
	 * single-sample texture in-pass (WebGPU); otherwise single-sample, the
	 * pre-MSAA path. Fixed here so every pipeline and offscreen target built
	 * below agrees on the sample count. See build_pipeline / the tick's
	 * offscreen paths.
	 */
	g_msaa_samples = (gpu->caps & GPU_CAP_MSAA_RESOLVE) ? 4u : 1u;

	build_pipeline(gpu);
	/* Cosmetic particle system: its pipeline and buffers are created here,
	 * off-frame like every other persistent resource, and the burst primitive
	 * is wired into the script image. */
	particles_init(gpu);
	register_particle_script();
	/* Mesh buffers are created lazily by ensure_meshes() on the first tick,
	 * once the seeded/loaded world exists — not from a fixed list here. */

	{
		struct gpu_buffer_desc bd;

		memset(&bd, 0, sizeof(bd));
		bd.usage = GPU_BUFFER_USAGE_UNIFORM;
		/* One 256-aligned slot per draw, so a draw's uniforms survive
		 * until the frame's command buffer executes. See the ring
		 * comment above SCENE_MAX_DRAWS. */
		bd.size  = SCENE_MAX_DRAWS * (uint32_t)UBO_STRIDE;
		g_ubo_ring = gpu->buffer_create(&bd);

		/*
		 * Each slot is sized to the largest Material block the renderer
		 * will upload; a draw fills only its material's actual param
		 * bytes and binds exactly that range within its slot.
		 */
		bd.size = SCENE_MAX_DRAWS * (uint32_t)MATERIAL_STRIDE;
		g_material_ring = gpu->buffer_create(&bd);

		/* The Sun block: the scene's one directional light, uploaded per
		 * frame and bound at slot 2 for the pbr shader. */
		bd.size = LIGHT_UBO_FLOATS * sizeof(float);
		g_light_ubo = gpu->buffer_create(&bd);
	}

	build_outline_resources(gpu);
	build_bloom_resources(gpu);
	build_shadow_resources(gpu);

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
 * The sun-shadow pass: render the scene's depth from the light's viewpoint into
 * the shadow map. It walks the same drawable entities the forward pass does, but
 * binds one depth-only pipeline and uploads the light's view_proj (into the
 * Camera block's view_proj slot) instead of the camera's — so the depth left in
 * the target is each fragment's distance from the sun, which the forward pass
 * later compares against. No material, texture, or light state is bound; only
 * geometry matters here. Skipped cleanly when the depth pipeline failed to
 * compile (the forward pass then falls back to the fully-lit dummy map).
 */
static void shadow_pass(struct fg_pass_ctx *ctx, void *userdata)
{
	const struct gpu_api *gpu = fg_ctx_gpu(ctx);
	gpu_cmd_buf_t         cmd = fg_ctx_cmd(ctx);
	const struct world   *w;
	uint32_t              i;
	float                 ubo[SCENE_UBO_FLOATS];
	struct mat4           shadow_vp;

	(void)userdata;
	if (!gpu || !g_scene || !g_shadow_pso)
		return;
	w = g_scene->get_world();
	if (!w)
		return;

	/*
	 * The light matrix is the whole pass's view_proj; only model changes.
	 *
	 * g_light.view_proj is GL-convention (NDC z in [-1, 1]). On a [0, 1]-clip
	 * backend (WebGPU) that would put the near half of the shadow frustum at
	 * clip z < 0 — clipped away — and write raw NDC z into the map. Adapting
	 * it here maps light depth into [0, 1] and stores 0.5*z + 0.5, which is
	 * exactly the window-depth value the forward pass already reconstructs for
	 * the compare (uvw.z = proj.z*0.5 + 0.5). So only this write copy is
	 * adapted: the forward pass keeps the GL matrix (bind_light) and the pbr
	 * shader needs no change. On GL the cap is clear and this is a no-op.
	 */
	shadow_vp = g_light.view_proj;
	if (gpu->caps & GPU_CAP_CLIP_Z_ZERO_TO_ONE)
		mat4_clip_z01(&shadow_vp);
	memcpy(&ubo[0], shadow_vp.m, 16 * sizeof(float));
	ubo[SCENE_UBO_CAMPOS + 0] = 0.0f;
	ubo[SCENE_UBO_CAMPOS + 1] = 0.0f;
	ubo[SCENE_UBO_CAMPOS + 2] = 0.0f;
	ubo[SCENE_UBO_CAMPOS + 3] = 0.0f;

	gpu->cmd_set_pipeline(cmd, g_shadow_pso);

	for (i = 0; i < w->count; i++) {
		struct gpu_draw_indexed_args draw;
		struct mesh_gpu             *m;
		struct mat4                  model;
		const uint8_t               *mp;
		uint32_t                     mplen;
		uint32_t                     slot, uoff;

		if (!w->alive[i] || !(w->mask[i] & COMPONENT_RENDER) ||
		    !(w->mask[i] & COMPONENT_MATERIAL))
			continue;
		mp = entity_mesh_params(w, i, &mplen);
		m  = find_mesh(w->render_ref[i], mp, mplen);
		if (!m)
			continue;

		mat4_from_transform(&model, &w->world_xform[i]);
		memcpy(&ubo[16], model.m, 16 * sizeof(float));
		if (!ring_take_slot(&slot))
			break; /* overflow: skip the remaining draws */
		uoff = slot * (uint32_t)UBO_STRIDE;
		gpu->buffer_update(g_ubo_ring, uoff, ubo, (uint32_t)sizeof(ubo));
		gpu->cmd_bind_uniform_buffer(cmd, 0, g_ubo_ring, uoff,
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

/*
 * Draw the cosmetic particles over the scene the pass just rendered. Particles
 * are pre-oriented on the CPU into camera-facing quads, so the pass hands the
 * system the world-space camera right/up (derived from the eye→target view and
 * the camera up) plus the view·projection; particles_render is a no-op when the
 * pool is empty, so an ordinary frame pays nothing here.
 */
static void draw_particles(const struct gpu_api *gpu, gpu_cmd_buf_t cmd)
{
	float fwd[3], right[3], up[3], len;

	fwd[0] = g_cam.target[0] - g_cam.eye[0];
	fwd[1] = g_cam.target[1] - g_cam.eye[1];
	fwd[2] = g_cam.target[2] - g_cam.eye[2];

	/* right = normalize(fwd × up_world) */
	right[0] = fwd[1] * g_cam.up[2] - fwd[2] * g_cam.up[1];
	right[1] = fwd[2] * g_cam.up[0] - fwd[0] * g_cam.up[2];
	right[2] = fwd[0] * g_cam.up[1] - fwd[1] * g_cam.up[0];
	len = sqrtf(right[0] * right[0] + right[1] * right[1] +
		    right[2] * right[2]);
	if (len < 1e-6f)
		return; /* degenerate camera (looking along up); skip this frame */
	right[0] /= len; right[1] /= len; right[2] /= len;

	/* up = normalize(right × fwd) — orthonormal screen-up regardless of the
	 * camera's roll or the world-up's tilt. */
	up[0] = right[1] * fwd[2] - right[2] * fwd[1];
	up[1] = right[2] * fwd[0] - right[0] * fwd[2];
	up[2] = right[0] * fwd[1] - right[1] * fwd[0];
	len = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
	if (len < 1e-6f)
		return;
	up[0] /= len; up[1] /= len; up[2] /= len;

	{
		struct mat4 vp = camera_clip_vp(gpu);

		particles_render(gpu, cmd, vp.m, right, up);
	}
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
	gpu_texture_t         shadow_tex;
	struct mat4           cam_vp;

	(void)userdata;
	if (!gpu || !g_scene)
		return;
	w = g_scene->get_world();
	if (!w)
		return;

	cam_vp = camera_clip_vp(gpu);
	memcpy(&ubo[0], cam_vp.m, 16 * sizeof(float));
	/* cam_pos is constant across the frame; only model changes per draw. */
	ubo[SCENE_UBO_CAMPOS + 0] = g_cam.eye[0];
	ubo[SCENE_UBO_CAMPOS + 1] = g_cam.eye[1];
	ubo[SCENE_UBO_CAMPOS + 2] = g_cam.eye[2];
	ubo[SCENE_UBO_CAMPOS + 3] = 0.0f; /* std140 vec3 tail pad */

	/* The directional light is constant across the pass — bind it once. */
	bind_light(gpu, cmd);

	/*
	 * The sun shadow map, for the pbr shader's shadow_map sampler (unit 0).
	 * Prefer the depth target the shadow pass just wrote; if this frame ran no
	 * shadow pass (or its pipeline is missing), fall back to the 1x1 lit dummy
	 * so an untextured pbr draw samples "unoccluded" rather than stale texels.
	 */
	shadow_tex = g_shadow_res ? fg_ctx_resource(ctx, g_shadow_res) : NULL;
	if (!shadow_tex)
		shadow_tex = g_shadow_dummy;

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
		uint32_t                      slot, uoff, moff;
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
		pso = g_forward_msaa ? pso_for_shader_ms(shader_ref)
				     : pso_for_shader(shader_ref);
		if (!have_pso || pso != cur_pso) {
			gpu->cmd_set_pipeline(cmd, pso);
			cur_pso  = pso;
			have_pso = 1;
		}

		if (!ring_take_slot(&slot))
			break; /* overflow: skip the remaining draws */
		uoff = slot * (uint32_t)UBO_STRIDE;
		moff = slot * (uint32_t)MATERIAL_STRIDE;

		mat4_from_transform(&model, &w->world_xform[i]);
		memcpy(&ubo[16], model.m, 16 * sizeof(float));
		gpu->buffer_update(g_ubo_ring, uoff, ubo, (uint32_t)sizeof(ubo));

		gpu->cmd_bind_uniform_buffer(cmd, 0, g_ubo_ring, uoff,
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
		gpu->buffer_update(g_material_ring, moff, params, plen);
		gpu->cmd_bind_uniform_buffer(cmd, 1, g_material_ring, moff, plen);

		/*
		 * Bind the sun shadow map and this material's texture. The two
		 * built-in scene shaders name their samplers so the backend's
		 * alphabetical rule assigns matching units:
		 *   scene-textured -> albedo (unit 0), shadow_map (unit 1)
		 *   pbr            -> shadow_map (unit 0, its only sampler)
		 * So a textured material binds its albedo to unit 0 and the shadow
		 * map to unit 1; an untextured (pbr) material binds the shadow map
		 * to unit 0. The albedo combo is already resident (ensure_textures
		 * ran off-frame).
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
				if (shadow_tex)
					gpu->cmd_bind_texture(cmd, 1, shadow_tex);
			} else if (shadow_tex) {
				gpu->cmd_bind_texture(cmd, 0, shadow_tex);
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

	/* Cosmetic particles composite over the opaque scene, in this same pass
	 * (so the outline path picks them up through scene_color too). */
	draw_particles(gpu, cmd);
}

/*
 * The selected entity's id when it is a live, drawable mesh worth outlining, or
 * 0-return when there is no such selection. Selection is read through the scene
 * api's get_selected (absent in headless test harnesses, which then never take
 * the outline path).
 */
static int outline_selected_entity(const struct world *w, uint32_t *out_id)
{
	int32_t sel;

	if (!g_scene)
		return 0;
	/*
	 * In editor chrome the outline follows the editor selection; in-game
	 * (chrome off) it follows the game's own outline target — the piece the
	 * chess rules picked up, set through entity_api.set_outline — so the ring
	 * shows in play, not just in the editor. Either source must still name a
	 * live, drawable mesh to be worth the pass.
	 */
	if (EDITOR_CHROME()) {
		if (!g_scene->get_selected)
			return 0;
		sel = g_scene->get_selected();
	} else {
		if (!g_scene->get_outline)
			return 0;
		sel = g_scene->get_outline();
	}
	if (sel < 0 || (uint32_t)sel >= w->count)
		return 0;
	if (!w->alive[sel] || !(w->mask[sel] & COMPONENT_RENDER))
		return 0;
	*out_id = (uint32_t)sel;
	return 1;
}

/*
 * Mask pass: draw the selected entity alone, flat white, into the silhouette
 * target (cleared to black by the pass). No material, no texture, no depth —
 * the union of its triangles is all the composite needs. Reuses the shared
 * Camera UBO with the selected entity's transform.
 */
static void mask_pass(struct fg_pass_ctx *ctx, void *userdata)
{
	const struct gpu_api        *gpu = fg_ctx_gpu(ctx);
	gpu_cmd_buf_t                 cmd = fg_ctx_cmd(ctx);
	const struct world          *w;
	uint32_t                     sel, plen = 0;
	const uint8_t               *pbytes;
	struct mesh_gpu             *m;
	struct mat4                  model;
	struct mat4                  cam_vp;
	float                        ubo[SCENE_UBO_FLOATS];
	struct gpu_draw_indexed_args draw;
	uint32_t                     slot, uoff;

	(void)userdata;
	if (!gpu || !g_scene || !g_mask_pso)
		return;
	w = g_scene->get_world();
	if (!w || !outline_selected_entity(w, &sel))
		return;
	pbytes = entity_mesh_params(w, sel, &plen);
	m = find_mesh(w->render_ref[sel], pbytes, plen);
	if (!m)
		return;

	cam_vp = camera_clip_vp(gpu);
	memcpy(&ubo[0], cam_vp.m, 16 * sizeof(float));
	mat4_from_transform(&model, &w->world_xform[sel]);
	memcpy(&ubo[16], model.m, 16 * sizeof(float));
	/* The mask shader reads only the matrices, but the block is uploaded
	 * whole — fill cam_pos so no uninitialised stack reaches the buffer. */
	ubo[SCENE_UBO_CAMPOS + 0] = g_cam.eye[0];
	ubo[SCENE_UBO_CAMPOS + 1] = g_cam.eye[1];
	ubo[SCENE_UBO_CAMPOS + 2] = g_cam.eye[2];
	ubo[SCENE_UBO_CAMPOS + 3] = 0.0f;

	if (!ring_take_slot(&slot))
		return; /* overflow: skip this draw */
	uoff = slot * (uint32_t)UBO_STRIDE;

	gpu->cmd_set_pipeline(cmd, g_mask_pso);
	gpu->buffer_update(g_ubo_ring, uoff, ubo, (uint32_t)sizeof(ubo));
	gpu->cmd_bind_uniform_buffer(cmd, 0, g_ubo_ring, uoff,
				     (uint32_t)sizeof(ubo));
	gpu->cmd_bind_vertex_buffer(cmd, 0, m->vbo, 0);
	gpu->cmd_bind_index_buffer(cmd, m->ebo, 0, GPU_INDEX_FORMAT_UINT16);

	memset(&draw, 0, sizeof(draw));
	draw.index_count    = m->index_count;
	draw.instance_count = 1;
	gpu->cmd_draw_indexed(cmd, &draw);
}

/*
 * Composite pass: a full-screen triangle that samples the offscreen scene and
 * the silhouette mask and writes the backbuffer, tinting red along the mask's
 * outer edge. texel carries the per-tap offset (thickness / viewport), so the
 * border width is resolution-independent.
 */
static void composite_pass(struct fg_pass_ctx *ctx, void *userdata)
{
	const struct gpu_api        *gpu = fg_ctx_gpu(ctx);
	gpu_cmd_buf_t                 cmd = fg_ctx_cmd(ctx);
	gpu_texture_t                 scene_tex, mask_tex;
	float                        ubo[OUTLINE_UBO_FLOATS];
	struct gpu_draw_indexed_args draw;

	(void)userdata;
	if (!gpu || !g_outline_pso)
		return;
	scene_tex = fg_ctx_resource(ctx, g_outline_frame.scene_color);
	mask_tex  = fg_ctx_resource(ctx, g_outline_frame.mask);

	memset(ubo, 0, sizeof(ubo));
	ubo[0] = g_view_w > 0.0f ? OUTLINE_THICKNESS / g_view_w : 0.0f; /* texel.x */
	ubo[1] = g_view_h > 0.0f ? OUTLINE_THICKNESS / g_view_h : 0.0f; /* texel.y */
	/*
	 * Editor selection outlines red; an in-game outline (a picked chess
	 * piece, chrome off) uses a warm gold that reads on both the ivory and
	 * the ebony pieces where a hard red would fight the dark set.
	 */
	if (EDITOR_CHROME()) {
		ubo[4] = 1.0f;              /* red   */
	} else {
		ubo[4] = 1.0f;             /* gold: */
		ubo[5] = 0.78f;            /* r 1.0 */
		ubo[6] = 0.30f;            /* g .78 b .30 */
	}
	ubo[7] = 1.0f; /* color.a — opaque */

	gpu->cmd_set_pipeline(cmd, g_outline_pso);
	gpu->buffer_update(g_outline_ubo, 0, ubo, (uint32_t)sizeof(ubo));
	gpu->cmd_bind_uniform_buffer(cmd, 0, g_outline_ubo, 0, (uint32_t)sizeof(ubo));
	/* Alphabetical unit rule: mask -> 0, scene -> 1 (see the backend). */
	gpu->cmd_bind_texture(cmd, 0, mask_tex);
	gpu->cmd_bind_texture(cmd, 1, scene_tex);
	gpu->cmd_bind_vertex_buffer(cmd, 0, g_fs_vbo, 0);
	gpu->cmd_bind_index_buffer(cmd, g_fs_ebo, 0, GPU_INDEX_FORMAT_UINT16);

	memset(&draw, 0, sizeof(draw));
	draw.index_count    = 3;
	draw.instance_count = 1;
	gpu->cmd_draw_indexed(cmd, &draw);
}

/* One full-screen-triangle draw of PSO reading TEX at unit 0. Shared by the
 * bloom extract pass and (with a bound UBO) the blur; the composite binds two
 * textures itself. */
static void fullscreen_draw(const struct gpu_api *gpu, gpu_cmd_buf_t cmd)
{
	struct gpu_draw_indexed_args draw;

	gpu->cmd_bind_vertex_buffer(cmd, 0, g_fs_vbo, 0);
	gpu->cmd_bind_index_buffer(cmd, g_fs_ebo, 0, GPU_INDEX_FORMAT_UINT16);
	memset(&draw, 0, sizeof(draw));
	draw.index_count    = 3;
	draw.instance_count = 1;
	gpu->cmd_draw_indexed(cmd, &draw);
}

/* Bloom, pass 1: threshold the offscreen scene into the half-res bright target. */
static void bloom_extract_pass(struct fg_pass_ctx *ctx, void *userdata)
{
	const struct gpu_api *gpu = fg_ctx_gpu(ctx);
	gpu_cmd_buf_t         cmd = fg_ctx_cmd(ctx);

	(void)userdata;
	if (!gpu || !g_bloom_extract_pso)
		return;
	gpu->cmd_set_pipeline(cmd, g_bloom_extract_pso);
	gpu->cmd_bind_texture(cmd, 0, fg_ctx_resource(ctx, g_bloom_frame.scene_color));
	fullscreen_draw(gpu, cmd);
}

/* One separable blur pass over INPUT with per-tap step (dx, dy). */
static void bloom_blur(struct fg_pass_ctx *ctx, fg_resource_t input,
		       float dx, float dy)
{
	const struct gpu_api *gpu = fg_ctx_gpu(ctx);
	gpu_cmd_buf_t         cmd = fg_ctx_cmd(ctx);
	float                 ubo[BLUR_UBO_FLOATS];

	if (!gpu || !g_bloom_blur_pso)
		return;
	memset(ubo, 0, sizeof(ubo));
	ubo[0] = dx;
	ubo[1] = dy;
	gpu->cmd_set_pipeline(cmd, g_bloom_blur_pso);
	gpu->buffer_update(g_blur_ubo, 0, ubo, (uint32_t)sizeof(ubo));
	gpu->cmd_bind_uniform_buffer(cmd, 0, g_blur_ubo, 0, (uint32_t)sizeof(ubo));
	gpu->cmd_bind_texture(cmd, 0, fg_ctx_resource(ctx, input));
	fullscreen_draw(gpu, cmd);
}

/* Bloom, pass 2: horizontal blur of the bright target (bloom_a -> bloom_b). */
static void bloom_blur_h_pass(struct fg_pass_ctx *ctx, void *userdata)
{
	(void)userdata;
	bloom_blur(ctx, g_bloom_frame.bloom_a,
		   g_bloom_frame.half_w ? 1.0f / (float)g_bloom_frame.half_w : 0.0f,
		   0.0f);
}

/* Bloom, pass 3: vertical blur back the other way (bloom_b -> bloom_a). */
static void bloom_blur_v_pass(struct fg_pass_ctx *ctx, void *userdata)
{
	(void)userdata;
	bloom_blur(ctx, g_bloom_frame.bloom_b, 0.0f,
		   g_bloom_frame.half_h ? 1.0f / (float)g_bloom_frame.half_h : 0.0f);
}

/* Bloom, pass 4: add the blurred bloom onto the full-res scene, into the bb. */
static void bloom_composite_pass(struct fg_pass_ctx *ctx, void *userdata)
{
	const struct gpu_api *gpu = fg_ctx_gpu(ctx);
	gpu_cmd_buf_t         cmd = fg_ctx_cmd(ctx);
	gpu_pipeline_t        pso = g_bloom_composite_to_bb
					    ? g_bloom_composite_bb_pso
					    : g_bloom_composite_pso;

	(void)userdata;
	if (!gpu || !pso)
		return;
	gpu->cmd_set_pipeline(cmd, pso);
	/* Alphabetical unit rule: bloom -> 0, scene -> 1. */
	gpu->cmd_bind_texture(cmd, 0, fg_ctx_resource(ctx, g_bloom_frame.bloom_c));
	gpu->cmd_bind_texture(cmd, 1, fg_ctx_resource(ctx, g_bloom_frame.scene_color));
	fullscreen_draw(gpu, cmd);
}

/* A live entity's name, or NULL — a self-contained twin of world_entity_name
 * so the renderer need not link the entity module for one lookup. */
static const char *camera_entity_name(const struct world *w, uint32_t e)
{
	if (e >= w->count || !(w->mask[e] & COMPONENT_NAME) ||
	    w->name_off[e] == WORLD_NO_NAME)
		return NULL;
	return w->names + w->name_off[e];
}

/* Is e a live entity named "Camera"? */
static int is_camera_entity(const struct world *w, int32_t e)
{
	const char *nm;

	if (e < 0 || (uint32_t)e >= w->count || !w->alive[e])
		return 0;
	nm = camera_entity_name(w, (uint32_t)e);
	return nm && strcmp(nm, "Camera") == 0;
}

/* The live "Camera" entity: the cached hint when still valid, else a scan for
 * whatever the current scene named "Camera", else -1. */
static int32_t resolve_camera_entity(const struct world *w)
{
	uint32_t i;

	if (is_camera_entity(w, g_camera_entity_id))
		return g_camera_entity_id;
	for (i = 0; i < w->count; i++)
		if (is_camera_entity(w, (int32_t)i))
			return (int32_t)i;
	return -1;
}

static void scene_renderer_tick(void)
{
	static const float  CLEAR[4]      = { 0.10f, 0.11f, 0.13f, 1.0f };
	static const float  MASK_CLEAR[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	struct fg          *fg;
	fg_resource_t       bb;
	fg_pass_t           pass;
	fg_resource_t       shadow;
	fg_pass_t           spass;
	const struct world *w;
	uint32_t            sel = 0;
	int                 outline;

	if (!g_ready || !g_fg_api || !g_scene)
		return;

	/* Warm pipelines and mesh buffers for the live world, off-frame, so the
	 * forward pass never creates or destroys a GPU resource mid-pass. */
	ensure_shader_pipelines();
	ensure_textures();
	ensure_meshes();

	w = g_scene->get_world();

	if (w) {
		g_camera_entity_id = resolve_camera_entity(w);
		if (g_camera_entity_id >= 0) {
			const struct transform *x =
				&w->world_xform[g_camera_entity_id];

			g_cam.eye[0] = x->position[0];
			g_cam.eye[1] = x->position[1];
			g_cam.eye[2] = x->position[2];
		}
	}

	/*
	 * Steer the directional light from the scene's first live light entity:
	 * its world rotation turns the default sun vector, so rotating the entity
	 * (gizmo or script) moves the light. With no light entity the default sun
	 * stays, so a scene without one looks exactly as it did before. Radiance
	 * is left at its constant white — per-light colour/intensity is the next
	 * increment, when a per-entity light-data column earns its keep.
	 */
	{
		static const float BASE_DIR[3] = { 0.5f, 0.8f, 0.4f };
		uint32_t j;

		g_light.dir[0] = BASE_DIR[0];
		g_light.dir[1] = BASE_DIR[1];
		g_light.dir[2] = BASE_DIR[2];
		if (w) {
			for (j = 0; j < w->count; j++) {
				if (!w->alive[j] ||
				    !(w->mask[j] & COMPONENT_LIGHT))
					continue;
				light_quat_rotate(w->world_xform[j].rotation,
						  BASE_DIR, g_light.dir);
				break;
			}
		}
	}

	camera_update(&g_cam);
	update_light_view_proj(w);

	/*
	 * Advance the cosmetic particles once per frame, before the pass that
	 * draws them. A fixed timestep (not the frame's real delta) — the pool is
	 * purely visual, so a steady rate reads fine and keeps a dropped frame
	 * from teleporting a burst; matching the ~60 Hz loop is close enough.
	 */
	particles_update(1.0f / 60.0f);

	/*
	 * The outline path adds two passes around the forward pass, so it only
	 * runs when there is actually a selected mesh to outline, its pipelines
	 * compiled, and the UI has reported a viewport to size the offscreen
	 * targets to. Otherwise (every shipped game, and any frame with nothing
	 * selected) the renderer stays the single forward-to-backbuffer pass it
	 * has always been, at zero added cost.
	 */
	outline = w && g_mask_pso && g_outline_pso &&
		  g_view_w > 0.0f && g_view_h > 0.0f &&
		  outline_selected_entity(w, &sel);

	fg = g_fg_api->create();
	if (!fg)
		return;
	bb = g_fg_api->import_backbuffer(fg);

	/*
	 * Rewind the uniform rings for the frame. This runs once, ahead of every
	 * pass, and deliberately NOT between passes: shadow and forward are
	 * separate command buffers within one frame and must share the ring.
	 * The backend's frame_end would also read correct, but scene_renderer
	 * does not own that hook, so the reset lives where the frame's passes
	 * are declared.
	 */
	g_ring_cursor = 0;

	/*
	 * The sun-shadow pass runs first when its depth pipeline compiled: it
	 * renders the scene's depth from the light into a square depth target the
	 * forward pass then samples. Declaring it here (and reading it from the
	 * forward pass) makes the frame graph order shadow-before-forward and keep
	 * the depth map alive across the boundary. With no pipeline the pass is
	 * skipped and the forward pass falls back to the fully-lit dummy map.
	 */
	shadow = 0;
	spass  = 0;
	if (g_shadow_pso && w) {
		fg_tex_desc sdesc;

		memset(&sdesc, 0, sizeof(sdesc));
		sdesc.format       = GPU_FORMAT_DEPTH32_FLOAT;
		sdesc.width        = SHADOW_MAP_DIM;
		sdesc.height       = SHADOW_MAP_DIM;
		sdesc.mip_levels   = 1;
		sdesc.sample_count = 1;
		shadow = g_fg_api->declare_transient(fg, "sun_shadow", sdesc);
		spass  = g_fg_api->pass_declare(fg, "sun_shadow", NULL, 0,
						&shadow, 1);
	}
	g_shadow_res = shadow;

	/*
	 * Bloom runs when its pipelines compiled and the UI has reported a
	 * viewport to size the offscreen targets to — the forward pass renders
	 * into an offscreen colour, then extract/blur/blur/composite adds a glow
	 * on the way to the backbuffer. Without either (a headless or pre-report
	 * frame, or a failed compile) the renderer falls back to the direct
	 * forward-to-backbuffer pass it has always been, at zero added cost.
	 */
	if (!outline) {
		int bloom = g_bloom_extract_pso && g_bloom_blur_pso &&
			    g_bloom_composite_pso && g_view_w > 0.0f &&
			    g_view_h > 0.0f;

		if (!bloom) {
			fg_resource_t freads[1];
			uint32_t      frn = 0;

			if (spass) { freads[0] = shadow; frn = 1; }
			pass = g_fg_api->pass_declare(fg, "forward",
						      frn ? freads : NULL, frn,
						      &bb, 1);
			if (pass && (!shadow || spass)) {
				if (spass) {
					g_fg_api->pass_set_depth_clear(spass, 1.0f);
					g_fg_api->pass_set_execute(spass,
								   shadow_pass, NULL);
				}
				g_fg_api->pass_set_color_clear(pass, 0, CLEAR);
				g_fg_api->pass_set_depth_clear(pass, 1.0f);
				/* Direct to the single-sample backbuffer: no MSAA. */
				g_forward_msaa = 0;
				g_fg_api->pass_set_execute(pass, forward_pass, NULL);
				g_fg_api->compile(fg);
				g_fg_api->execute(fg);
			}
		} else {
			uint32_t      vw = (uint32_t)g_view_w, vh = (uint32_t)g_view_h;
			uint32_t      hw = vw > 1 ? vw / 2 : 1;
			uint32_t      hh = vh > 1 ? vh / 2 : 1;
			uint32_t      samples = g_msaa_samples;
			fg_tex_desc   cdesc, ddesc, hdesc, scdesc, sddesc;
			fg_resource_t scene_color, scene_depth, scene_resolve, post;
			fg_resource_t ba, bb2, bc;
			fg_resource_t fwrites[2], freads[1], xr[1], hr[1], vr[1],
				      cr[2];
			uint32_t      frn = 0;
			fg_pass_t     fpass, xpass, hpass, vpass, cpass;

			memset(&cdesc, 0, sizeof(cdesc));
			cdesc.format       = GPU_FORMAT_RGBA8_UNORM;
			cdesc.width        = vw;
			cdesc.height       = vh;
			cdesc.mip_levels   = 1;
			cdesc.sample_count = 1;
			ddesc              = cdesc;
			ddesc.format       = GPU_FORMAT_DEPTH32_FLOAT;
			hdesc              = cdesc;
			hdesc.width        = hw;
			hdesc.height       = hh;
			/*
			 * The scene colour + depth the forward pass renders into are
			 * multisampled; the half-res bloom targets and the resolve
			 * target stay single-sample (cdesc). The post passes sample
			 * the resolved colour, not the multisampled one — a multisample
			 * texture is not directly sampleable — so `post` is the resolve
			 * when MSAA is on and the scene colour itself when it is off.
			 */
			scdesc              = cdesc;
			scdesc.sample_count = samples;
			sddesc              = ddesc;
			sddesc.sample_count = samples;

			scene_color = g_fg_api->declare_transient(fg, "scene_color",
								  scdesc);
			scene_depth = g_fg_api->declare_transient(fg, "scene_depth",
								  sddesc);
			ba  = g_fg_api->declare_transient(fg, "bloom_a", hdesc);
			bb2 = g_fg_api->declare_transient(fg, "bloom_b", hdesc);
			bc  = g_fg_api->declare_transient(fg, "bloom_c", hdesc);
			scene_resolve = 0;
			post          = scene_color;
			if (samples > 1) {
				scene_resolve = g_fg_api->declare_transient(
					fg, "scene_resolve", cdesc);
				post = scene_resolve;
			}

			g_bloom_frame.scene_color = post;
			g_bloom_frame.bloom_a     = ba;
			g_bloom_frame.bloom_b     = bb2;
			g_bloom_frame.bloom_c     = bc;
			g_bloom_frame.half_w      = hw;
			g_bloom_frame.half_h      = hh;

			fwrites[0] = scene_color;
			fwrites[1] = scene_depth;
			if (spass) { freads[0] = shadow; frn = 1; }
			fpass = g_fg_api->pass_declare(fg, "forward",
						       frn ? freads : NULL, frn,
						       fwrites, 2);
			if (fpass && samples > 1)
				g_fg_api->pass_set_resolve(fpass, 0, scene_resolve);
			xr[0] = post;
			xpass = g_fg_api->pass_declare(fg, "bloom_extract", xr, 1,
						       &ba, 1);
			hr[0] = ba;
			hpass = g_fg_api->pass_declare(fg, "bloom_blur_h", hr, 1,
						       &bb2, 1);
			vr[0] = bb2;
			vpass = g_fg_api->pass_declare(fg, "bloom_blur_v", vr, 1,
						       &bc, 1);
			cr[0] = post;
			cr[1] = bc;
			cpass = g_fg_api->pass_declare(fg, "bloom_composite", cr, 2,
						       &bb, 1);
			g_bloom_composite_to_bb = 1;

			if (fpass && xpass && hpass && vpass && cpass &&
			    (!shadow || spass)) {
				if (spass) {
					g_fg_api->pass_set_depth_clear(spass, 1.0f);
					g_fg_api->pass_set_execute(spass,
								   shadow_pass, NULL);
				}
				g_fg_api->pass_set_color_clear(fpass, 0, CLEAR);
				g_fg_api->pass_set_depth_clear(fpass, 1.0f);
				g_forward_msaa = samples > 1;
				g_fg_api->pass_set_execute(fpass, forward_pass, NULL);
				g_fg_api->pass_set_execute(xpass, bloom_extract_pass,
							   NULL);
				g_fg_api->pass_set_execute(hpass, bloom_blur_h_pass,
							   NULL);
				g_fg_api->pass_set_execute(vpass, bloom_blur_v_pass,
							   NULL);
				g_fg_api->pass_set_execute(cpass,
							   bloom_composite_pass, NULL);
				g_fg_api->compile(fg);
				g_fg_api->execute(fg);
			}
		}
	} else {
		/*
		 * A mesh is outlined this frame. When the bloom pipelines are up
		 * the two post chains compose (#630/#622): the forward scene is
		 * bloomed into a full-res intermediate `lit`, then the outline
		 * edge is drawn over `lit` on the way to the backbuffer, so the
		 * piece both glows and wears its selection ring. Both stages
		 * reuse their existing shaders unchanged — bloom_composite just
		 * targets `lit` instead of the backbuffer, and the outline
		 * composite reads `lit` as its scene. Without bloom it is the
		 * plain forward -> mask -> outline path.
		 */
		int           bloom = g_bloom_extract_pso && g_bloom_blur_pso &&
				      g_bloom_composite_pso;
		uint32_t      vw = (uint32_t)g_view_w, vh = (uint32_t)g_view_h;
		uint32_t      samples = g_msaa_samples;
		fg_tex_desc   cdesc, ddesc, scdesc, sddesc;
		fg_resource_t scene_color, scene_depth, scene_resolve, post, mask;
		fg_resource_t fwrites[2], creads[2], freads[1];
		uint32_t      frn = 0;
		fg_pass_t     fpass, mpass, cpass;

		memset(&cdesc, 0, sizeof(cdesc));
		cdesc.format       = GPU_FORMAT_RGBA8_UNORM;
		cdesc.width        = vw;
		cdesc.height       = vh;
		cdesc.mip_levels   = 1;
		cdesc.sample_count = 1;
		ddesc              = cdesc;
		ddesc.format       = GPU_FORMAT_DEPTH32_FLOAT;
		/*
		 * Scene colour + depth are multisampled; the mask, the bloom
		 * half-res targets and outline_lit stay single-sample (cdesc). The
		 * post passes (bloom, outline composite) sample the resolved colour
		 * `post`, which is the resolve target when MSAA is on and the scene
		 * colour itself when it is off.
		 */
		scdesc              = cdesc;
		scdesc.sample_count = samples;
		sddesc              = ddesc;
		sddesc.sample_count = samples;

		scene_color = g_fg_api->declare_transient(fg, "scene_color", scdesc);
		scene_depth = g_fg_api->declare_transient(fg, "scene_depth", sddesc);
		mask        = g_fg_api->declare_transient(fg, "sel_mask", cdesc);
		scene_resolve = 0;
		post          = scene_color;
		if (samples > 1) {
			scene_resolve = g_fg_api->declare_transient(
				fg, "scene_resolve", cdesc);
			post = scene_resolve;
		}

		g_outline_frame.mask = mask;

		fwrites[0] = scene_color;
		fwrites[1] = scene_depth;
		if (spass) { freads[0] = shadow; frn = 1; }
		fpass = g_fg_api->pass_declare(fg, "forward", frn ? freads : NULL,
					       frn, fwrites, 2);
		if (fpass && samples > 1)
			g_fg_api->pass_set_resolve(fpass, 0, scene_resolve);
		mpass = g_fg_api->pass_declare(fg, "sel_mask", NULL, 0, &mask, 1);

		if (bloom) {
			uint32_t      hw = vw > 1 ? vw / 2 : 1;
			uint32_t      hh = vh > 1 ? vh / 2 : 1;
			fg_tex_desc   hdesc = cdesc;
			fg_resource_t ba, bb2, bc, lit;
			fg_resource_t xr[1], hr[1], vr[1], lr[2], cr[2];
			fg_pass_t     xpass, hpass, vpass, lpass;

			hdesc.width  = hw;
			hdesc.height = hh;
			ba  = g_fg_api->declare_transient(fg, "bloom_a", hdesc);
			bb2 = g_fg_api->declare_transient(fg, "bloom_b", hdesc);
			bc  = g_fg_api->declare_transient(fg, "bloom_c", hdesc);
			lit = g_fg_api->declare_transient(fg, "outline_lit", cdesc);

			g_bloom_frame.scene_color = post;
			g_bloom_frame.bloom_a     = ba;
			g_bloom_frame.bloom_b     = bb2;
			g_bloom_frame.bloom_c     = bc;
			g_bloom_frame.half_w      = hw;
			g_bloom_frame.half_h      = hh;
			g_outline_frame.scene_color = lit;

			xr[0] = post;
			xpass = g_fg_api->pass_declare(fg, "bloom_extract", xr, 1,
						       &ba, 1);
			hr[0] = ba;
			hpass = g_fg_api->pass_declare(fg, "bloom_blur_h", hr, 1,
						       &bb2, 1);
			vr[0] = bb2;
			vpass = g_fg_api->pass_declare(fg, "bloom_blur_v", vr, 1,
						       &bc, 1);
			lr[0] = post;
			lr[1] = bc;
			lpass = g_fg_api->pass_declare(fg, "bloom_composite", lr, 2,
						       &lit, 1);
			g_bloom_composite_to_bb = 0;
			cr[0] = lit;
			cr[1] = mask;
			cpass = g_fg_api->pass_declare(fg, "outline", cr, 2, &bb, 1);

			if (fpass && mpass && xpass && hpass && vpass && lpass &&
			    cpass && (!shadow || spass)) {
				if (spass) {
					g_fg_api->pass_set_depth_clear(spass, 1.0f);
					g_fg_api->pass_set_execute(spass,
								   shadow_pass, NULL);
				}
				g_fg_api->pass_set_color_clear(fpass, 0, CLEAR);
				g_fg_api->pass_set_depth_clear(fpass, 1.0f);
				g_forward_msaa = samples > 1;
				g_fg_api->pass_set_execute(fpass, forward_pass, NULL);
				g_fg_api->pass_set_color_clear(mpass, 0, MASK_CLEAR);
				g_fg_api->pass_set_execute(mpass, mask_pass, NULL);
				g_fg_api->pass_set_execute(xpass, bloom_extract_pass,
							   NULL);
				g_fg_api->pass_set_execute(hpass, bloom_blur_h_pass,
							   NULL);
				g_fg_api->pass_set_execute(vpass, bloom_blur_v_pass,
							   NULL);
				g_fg_api->pass_set_execute(lpass,
							   bloom_composite_pass, NULL);
				g_fg_api->pass_set_execute(cpass, composite_pass,
							   NULL);
				g_fg_api->compile(fg);
				g_fg_api->execute(fg);
			}
		} else {
			g_outline_frame.scene_color = post;
			creads[0] = post;
			creads[1] = mask;
			cpass = g_fg_api->pass_declare(fg, "outline", creads, 2,
						       &bb, 1);

			if (fpass && mpass && cpass && (!shadow || spass)) {
				if (spass) {
					g_fg_api->pass_set_depth_clear(spass, 1.0f);
					g_fg_api->pass_set_execute(spass,
								   shadow_pass, NULL);
				}
				g_fg_api->pass_set_color_clear(fpass, 0, CLEAR);
				g_fg_api->pass_set_depth_clear(fpass, 1.0f);
				g_forward_msaa = samples > 1;
				g_fg_api->pass_set_execute(fpass, forward_pass, NULL);
				g_fg_api->pass_set_color_clear(mpass, 0, MASK_CLEAR);
				g_fg_api->pass_set_execute(mpass, mask_pass, NULL);
				g_fg_api->pass_set_execute(cpass, composite_pass,
							   NULL);
				g_fg_api->compile(fg);
				g_fg_api->execute(fg);
			}
		}
	}
	g_fg_api->destroy(fg);
	g_shadow_res = 0;
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
		if (g_ubo_ring)
			gpu->buffer_destroy(g_ubo_ring);
		if (g_material_ring)
			gpu->buffer_destroy(g_material_ring);
		if (g_light_ubo)
			gpu->buffer_destroy(g_light_ubo);
		if (g_mask_pso)
			gpu->pipeline_destroy(g_mask_pso);
		if (g_outline_pso)
			gpu->pipeline_destroy(g_outline_pso);
		if (g_shadow_pso)
			gpu->pipeline_destroy(g_shadow_pso);
		if (g_shadow_dummy)
			gpu->texture_destroy(g_shadow_dummy);
		if (g_color_dummy)
			gpu->texture_destroy(g_color_dummy);
		if (g_fs_vbo)
			gpu->buffer_destroy(g_fs_vbo);
		if (g_fs_ebo)
			gpu->buffer_destroy(g_fs_ebo);
		if (g_outline_ubo)
			gpu->buffer_destroy(g_outline_ubo);
		if (g_bloom_extract_pso)
			gpu->pipeline_destroy(g_bloom_extract_pso);
		if (g_bloom_blur_pso)
			gpu->pipeline_destroy(g_bloom_blur_pso);
		if (g_bloom_composite_pso)
			gpu->pipeline_destroy(g_bloom_composite_pso);
		if (g_bloom_composite_bb_pso)
			gpu->pipeline_destroy(g_bloom_composite_bb_pso);
		if (g_blur_ubo)
			gpu->buffer_destroy(g_blur_ubo);
		/*
		 * The default pipeline is also cached (under the scene shader's
		 * id), so destroy the distinct cache entries first, then it once.
		 * Each slot's multisampled twin is destroyed only when it is a
		 * distinct pipeline (it aliases pso, or the cached default's twin,
		 * when MSAA is off or the compile failed), so nothing is freed twice.
		 */
		for (i = 0; i < g_shader_pso_count; i++) {
			if (g_shader_psos[i].pso_ms &&
			    g_shader_psos[i].pso_ms != g_shader_psos[i].pso &&
			    g_shader_psos[i].pso_ms != g_default_pso_ms)
				gpu->pipeline_destroy(g_shader_psos[i].pso_ms);
			if (g_shader_psos[i].pso &&
			    g_shader_psos[i].pso != g_default_pso)
				gpu->pipeline_destroy(g_shader_psos[i].pso);
		}
		if (g_default_pso_ms && g_default_pso_ms != g_default_pso)
			gpu->pipeline_destroy(g_default_pso_ms);
		if (g_default_pso)
			gpu->pipeline_destroy(g_default_pso);
	}
	g_default_pso      = 0;
	g_default_pso_ms   = 0;
	g_mask_pso         = 0;
	g_outline_pso      = 0;
	g_shadow_pso       = 0;
	g_shadow_dummy     = 0;
	g_color_dummy      = 0;
	g_shadow_res       = 0;
	g_fs_vbo           = 0;
	g_fs_ebo           = 0;
	g_outline_ubo      = 0;
	g_bloom_extract_pso   = 0;
	g_bloom_blur_pso      = 0;
	g_bloom_composite_pso = 0;
	g_bloom_composite_bb_pso = 0;
	g_blur_ubo            = 0;
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
