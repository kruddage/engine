/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FG_H
#define FG_H

#include "renderer.h"
#include <stdint.h>

/*
 * Frame graph plugin — Frostbite/Unreal-style render graph.
 *
 * The graph owns resource lifetimes, the pass DAG, automatic barriers, and
 * render-pass setup (it begins and ends the render pass around each pass from
 * that pass's declared writes). It does NOT wrap draw calls.
 *
 * At execute time the graph LENDS the GPU command API into each pass's execute
 * callback via struct fg_pass_ctx. Inside the callback a pass records normal
 * GPU commands (set pipeline, draw, dispatch) on the lent command buffer. The
 * lent context is borrowed, not kept: it is valid ONLY for the duration of the
 * callback and must never be cached, stored, or resolved as "renderer".
 *
 * Persistent GPU resources (pipeline state objects, static mesh buffers) live
 * longer than a frame, so they fall outside the lend. A consumer creates them
 * ONCE by resolving the "renderer" device directly, outside any frame; per-frame
 * work stays on the lent context. See docs/frame-graph.md.
 *
 * Usage:
 *   struct fg *fg = fg_api->create();
 *   r  = fg_api->declare_transient(fg, "depth", desc);
 *   bb = fg_api->import_backbuffer(fg);
 *   p  = fg_api->pass_declare(fg, "main", NULL, 0, writes, 2);
 *   fg_api->pass_set_color_clear(p, 0, (float[4]){0, 0, 0, 1});
 *   fg_api->pass_set_execute(p, my_cb, userdata);
 *   fg_api->compile(fg);
 *   fg_api->execute(fg);  // called each frame
 */

/* --- Opaque handles ------------------------------------------------------ */

/* Forward-declared; defined in fg.c */
struct fg_pass;
struct fg_resource;

typedef struct fg_pass     *fg_pass_t;
typedef struct fg_resource *fg_resource_t;

/* --- Transient texture descriptor ---------------------------------------- */

/* Alias gpu_texture_desc so callers don't need to include renderer.h */
typedef struct gpu_texture_desc fg_tex_desc;

/* --- Pass execute context ------------------------------------------------ */

/*
 * Command context lent to a pass execute callback. The accessors below are
 * valid ONLY inside the callback (borrowed, not kept) — do not cache them or
 * the handles they return across frames.
 */
struct fg_pass_ctx;
typedef void (*fg_execute_fn)(struct fg_pass_ctx *ctx, void *userdata);

/* The lent GPU API. Record commands; never retain past the callback. */
const struct gpu_api *fg_ctx_gpu(struct fg_pass_ctx *ctx);
/* The command buffer the render pass was begun on. */
gpu_cmd_buf_t         fg_ctx_cmd(struct fg_pass_ctx *ctx);
/* Resolve a declared resource to its bound GPU texture for this frame. */
gpu_texture_t         fg_ctx_resource(struct fg_pass_ctx *ctx, fg_resource_t r);

/* --- Frame graph object -------------------------------------------------- */

struct fg;

struct fg *fg_create(const struct gpu_api *gpu);
void       fg_destroy(struct fg *fg);

fg_resource_t fg_declare_transient(struct fg *fg, const char *name,
				    fg_tex_desc desc);

/*
 * Import the backbuffer (canvas / default framebuffer) as a graph resource a
 * pass can name as a write target. Imported resources are bound by the graph
 * but never texture_create'd or texture_destroy'd — the graph does not own
 * their storage.
 */
fg_resource_t fg_import_backbuffer(struct fg *fg);

fg_pass_t fg_pass_declare(struct fg *fg, const char *name,
			   fg_resource_t *reads,  uint32_t read_count,
			   fg_resource_t *writes, uint32_t write_count);

void fg_pass_set_execute(fg_pass_t pass, fg_execute_fn cb, void *userdata);

/*
 * Per-pass attachment load configuration. By default every attachment loads
 * its existing contents. These switch an attachment to CLEAR with the given
 * value. The color index counts color-format writes in declaration order.
 */
void fg_pass_set_color_clear(fg_pass_t pass, uint32_t index,
			      const float rgba[4]);
void fg_pass_set_depth_clear(fg_pass_t pass, float depth);

/*
 * Resolve color attachment `color_index` of this pass into `resolve_target` (a
 * declared transient) at pass end — the multisampled-to-single-sample step. The
 * graph allocates and lifetimes the resolve target like any other write and
 * emits it as gpu_color_attachment.resolve_target; passes that read it depend on
 * this pass. Only meaningful when the color target is multisampled and the
 * backend advertises GPU_CAP_MSAA_RESOLVE — the caller gates on that; a backend
 * without it ignores the resolve_target and renders single-sample.
 */
void fg_pass_set_resolve(fg_pass_t pass, uint32_t color_index,
			  fg_resource_t resolve_target);

void fg_compile(struct fg *fg);
void fg_execute(struct fg *fg);

/* --- Service vtable registered as "frame_graph" with subsystem_manager --- */

struct fg_api {
	struct fg    *(*create)(void);
	void          (*destroy)(struct fg *fg);

	fg_resource_t (*declare_transient)(struct fg *fg, const char *name,
					   fg_tex_desc desc);
	fg_resource_t (*import_backbuffer)(struct fg *fg);
	fg_pass_t     (*pass_declare)(struct fg *fg, const char *name,
				      fg_resource_t *reads,  uint32_t read_count,
				      fg_resource_t *writes, uint32_t write_count);
	void          (*pass_set_execute)(fg_pass_t pass, fg_execute_fn cb,
					  void *userdata);
	void          (*pass_set_color_clear)(fg_pass_t pass, uint32_t index,
					      const float rgba[4]);
	void          (*pass_set_depth_clear)(fg_pass_t pass, float depth);
	void          (*pass_set_resolve)(fg_pass_t pass,
					  uint32_t color_index,
					  fg_resource_t resolve_target);
	void          (*compile)(struct fg *fg);
	void          (*execute)(struct fg *fg);
};

#endif /* FG_H */
