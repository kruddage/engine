/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "particles.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Pool cap. A burst is a few dozen particles and lifetimes are under a second,
 * so a few hundred covers several simultaneous effects; past this a burst drops
 * its overflow (see particles_burst). Sized small on purpose — this is the
 * WebGL budget tier, bounded by the per-frame vertex upload, not GPU residency.
 */
#define PARTICLE_MAX 512u

/* Six vertices (two triangles) per particle quad, seven floats each. */
#define PARTICLE_VERTS 6u
#define PARTICLE_VERT_FLOATS 7u /* pos.xyz (3) + color.rgba (4) */
#define PARTICLE_QUAD_FLOATS (PARTICLE_VERTS * PARTICLE_VERT_FLOATS)

/* Simulation constants. Gravity pulls the fountain back down; the speed and
 * lifetime ranges give a burst its spread and fade. Tuned by eye for a small
 * spark, not exposed yet — an emitter descriptor is the next increment. */
#define PARTICLE_GRAVITY (-4.0f)
#define PARTICLE_SPEED_MIN 1.4f
#define PARTICLE_SPEED_MAX 3.6f
#define PARTICLE_LIFE_MIN 0.35f
#define PARTICLE_LIFE_MAX 0.85f
#define PARTICLE_SIZE_MIN 0.04f
#define PARTICLE_SIZE_MAX 0.08f

/*
 * The particle shader, authored in the krudd DSL (lowered to GLSL on WebGL and
 * WGSL on WebGPU by the backend at pipeline-create). A particle is already a
 * world-space, camera-facing quad by the time it reaches the GPU — the CPU built
 * the corners — so the vertex stage only projects it and passes the colour
 * through; the fragment stage writes that colour straight out, alpha and all.
 */
static const char *const PARTICLE_SHADER_SRC =
	"(shader particles"
	"  (inputs (a_pos vec3 (location 0)) (a_color vec4 (location 1)))"
	"  (uniforms (Camera (block 0) (view_proj mat4)))"
	"  (varyings (v_color vec4))"
	"  (targets (frag_color vec4 (location 0)))"
	"  (vertex"
	"    (set v_color a_color)"
	"    (set position (* view_proj (vec4 a_pos 1.0))))"
	"  (fragment"
	"    (set frag_color v_color)))";

struct particle {
	float pos[3];
	float vel[3];
	float color[3];
	float life;     /* seconds remaining */
	float life_max; /* seconds at spawn, for the alpha fade */
	float size;     /* half-extent of the quad, world units */
};

static struct particle g_pool[PARTICLE_MAX];
static uint32_t        g_live; /* live particles are packed in [0, g_live) */

static gpu_pipeline_t         g_pso;
static gpu_buffer_t           g_vbo;
static gpu_buffer_t           g_ubo;
static int                    g_ready;

/*
 * Vertex staging, baked each frame and uploaded whole. File-scope rather than on
 * the stack — the full pool is ~86 KB of floats, too large for a frame's stack.
 */
static float g_verts[PARTICLE_MAX * PARTICLE_QUAD_FLOATS];

/*
 * A tiny xorshift PRNG seeded from a burst counter, so a burst spreads its
 * particles without pulling in a platform RNG and without Math.random — and so
 * the spread is reproducible for a given burst index, which the test relies on.
 */
static uint32_t g_rng = 0x1234567u;

static float rng_unit(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 17;
	g_rng ^= g_rng << 5;
	return (float)(g_rng & 0xffffffu) / (float)0x1000000u; /* [0,1) */
}

static float rng_range(float lo, float hi)
{
	return lo + (hi - lo) * rng_unit();
}

void particles_init(const struct gpu_api *device)
{
	struct gpu_pipeline_desc pd;
	struct gpu_buffer_desc   bd;

	if (!device)
		return;

	memset(&pd, 0, sizeof(pd));
	pd.color_formats[0]   = GPU_FORMAT_RGBA8_UNORM;
	pd.color_format_count = 1;
	/* Match the forward pass's depth target so the pipeline is compatible on
	 * WebGPU; the test is disabled below, so particles never occlude and are
	 * never occluded — they simply composite on top of the drawn scene. */
	pd.depth_format       = GPU_FORMAT_DEPTH32_FLOAT;
	pd.topology           = GPU_TOPOLOGY_TRIANGLE_LIST;

	/* particle vertex: a_pos(vec3) @0, a_color(vec4) @12; stride 28. */
	pd.vertex_layout.attr_count = 2;
	pd.vertex_layout.stride     = PARTICLE_VERT_FLOATS * (uint32_t)sizeof(float);
	pd.vertex_layout.attrs[0]   =
		(struct gpu_vertex_attr){ 0, 0, GPU_FORMAT_RGB32_FLOAT };
	pd.vertex_layout.attrs[1]   = (struct gpu_vertex_attr){
		1, 3u * (uint32_t)sizeof(float), GPU_FORMAT_RGBA32_FLOAT };

	pd.vert.src     = PARTICLE_SHADER_SRC;
	pd.vert.stage   = GPU_SHADER_STAGE_VERTEX;
	pd.vert.dialect = GPU_SHADER_DIALECT_KRUDD;
	pd.frag.src     = PARTICLE_SHADER_SRC;
	pd.frag.stage   = GPU_SHADER_STAGE_FRAGMENT;
	pd.frag.dialect = GPU_SHADER_DIALECT_KRUDD;

	/* Straight-alpha compositing over the scene, and no depth test so a
	 * brief burst always reads on top (the gpu_api's one blend mode; an
	 * additive glow would need a blend-mode field the ABI does not carry
	 * yet). */
	pd.blend_enable       = 1;
	pd.disable_depth_test = 1;

	g_pso = device->pipeline_create(&pd);

	memset(&bd, 0, sizeof(bd));
	bd.usage = GPU_BUFFER_USAGE_VERTEX;
	bd.size  = sizeof(g_verts);
	g_vbo    = device->buffer_create(&bd);

	memset(&bd, 0, sizeof(bd));
	bd.usage = GPU_BUFFER_USAGE_UNIFORM;
	bd.size  = 16u * sizeof(float); /* view_proj mat4 */
	g_ubo    = device->buffer_create(&bd);

	g_live  = 0;
	g_ready = 1;
}

void particles_burst(const float pos[3], const float rgb[3], uint32_t count)
{
	uint32_t i;

	if (!pos || !rgb)
		return;
	if (g_live + count > PARTICLE_MAX)
		count = PARTICLE_MAX - g_live; /* fill what fits, drop the rest */

	for (i = 0; i < count; i++) {
		struct particle *p = &g_pool[g_live++];
		float            dir[3];
		float            speed = rng_range(PARTICLE_SPEED_MIN,
						    PARTICLE_SPEED_MAX);
		float            len;

		/* A random direction in the unit cube, biased upward, then
		 * normalised — a rough hemisphere fountain without any trig. */
		dir[0] = rng_range(-1.0f, 1.0f);
		dir[1] = rng_range(0.2f, 1.0f);
		dir[2] = rng_range(-1.0f, 1.0f);
		len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
		if (len < 1e-4f)
			len = 1.0f;

		p->pos[0] = pos[0];
		p->pos[1] = pos[1];
		p->pos[2] = pos[2];
		p->vel[0] = dir[0] / len * speed;
		p->vel[1] = dir[1] / len * speed;
		p->vel[2] = dir[2] / len * speed;
		p->color[0] = rgb[0];
		p->color[1] = rgb[1];
		p->color[2] = rgb[2];
		p->life_max = rng_range(PARTICLE_LIFE_MIN, PARTICLE_LIFE_MAX);
		p->life     = p->life_max;
		p->size     = rng_range(PARTICLE_SIZE_MIN, PARTICLE_SIZE_MAX);
	}
}

void particles_update(float dt)
{
	uint32_t i = 0;

	if (dt <= 0.0f)
		return;

	/*
	 * Integrate, then age out by swap-remove: an expired particle is
	 * overwritten by the last live one and the count shrinks, so the pool
	 * stays packed in [0, g_live) without a per-slot alive flag. i is not
	 * advanced on a removal — the swapped-in particle is processed in place.
	 */
	while (i < g_live) {
		struct particle *p = &g_pool[i];

		p->life -= dt;
		if (p->life <= 0.0f) {
			g_pool[i] = g_pool[--g_live];
			continue;
		}
		p->vel[1] += PARTICLE_GRAVITY * dt;
		p->pos[0] += p->vel[0] * dt;
		p->pos[1] += p->vel[1] * dt;
		p->pos[2] += p->vel[2] * dt;
		i++;
	}
}

/* Write one quad vertex into DST and return the next slot. */
static float *put_vertex(float *dst, const float pos[3], const float rgba[4])
{
	dst[0] = pos[0];
	dst[1] = pos[1];
	dst[2] = pos[2];
	dst[3] = rgba[0];
	dst[4] = rgba[1];
	dst[5] = rgba[2];
	dst[6] = rgba[3];
	return dst + PARTICLE_VERT_FLOATS;
}

void particles_render(const struct gpu_api *device, gpu_cmd_buf_t cmd,
		      const float view_proj[16], const float cam_right[3],
		      const float cam_up[3])
{
	float   *w = g_verts;
	uint32_t i;

	if (!g_ready || !device || g_live == 0)
		return; /* idle frame: no upload, no draw, no state touched */

	for (i = 0; i < g_live; i++) {
		const struct particle *p = &g_pool[i];
		float rgba[4];
		float c0[3], c1[3], c2[3], c3[3];
		float rx = cam_right[0] * p->size;
		float ry = cam_right[1] * p->size;
		float rz = cam_right[2] * p->size;
		float ux = cam_up[0] * p->size;
		float uy = cam_up[1] * p->size;
		float uz = cam_up[2] * p->size;

		/* Fade out over the tail of the particle's life. */
		rgba[0] = p->color[0];
		rgba[1] = p->color[1];
		rgba[2] = p->color[2];
		rgba[3] = p->life_max > 0.0f ? p->life / p->life_max : 0.0f;

		/* The four corners: centre ± right ± up, in world space. */
		c0[0] = p->pos[0] - rx - ux;
		c0[1] = p->pos[1] - ry - uy;
		c0[2] = p->pos[2] - rz - uz;
		c1[0] = p->pos[0] + rx - ux;
		c1[1] = p->pos[1] + ry - uy;
		c1[2] = p->pos[2] + rz - uz;
		c2[0] = p->pos[0] + rx + ux;
		c2[1] = p->pos[1] + ry + uy;
		c2[2] = p->pos[2] + rz + uz;
		c3[0] = p->pos[0] - rx + ux;
		c3[1] = p->pos[1] - ry + uy;
		c3[2] = p->pos[2] - rz + uz;

		w = put_vertex(w, c0, rgba); /* triangle 1: c0, c1, c2 */
		w = put_vertex(w, c1, rgba);
		w = put_vertex(w, c2, rgba);
		w = put_vertex(w, c2, rgba); /* triangle 2: c2, c3, c0 */
		w = put_vertex(w, c3, rgba);
		w = put_vertex(w, c0, rgba);
	}

	device->buffer_update(g_ubo, 0, view_proj, 16u * (uint32_t)sizeof(float));
	device->buffer_update(g_vbo, 0, g_verts,
			      g_live * PARTICLE_QUAD_FLOATS *
				      (uint32_t)sizeof(float));

	device->cmd_set_pipeline(cmd, g_pso);
	device->cmd_bind_uniform_buffer(cmd, 0, g_ubo, 0,
					16u * (uint32_t)sizeof(float));
	device->cmd_bind_vertex_buffer(cmd, 0, g_vbo, 0);
	device->cmd_draw(cmd, g_live * PARTICLE_VERTS, 1, 0, 0);
}

uint32_t particles_live_count(void)
{
	return g_live;
}
