/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "fg.h"
#include "renderer.h"
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __EMSCRIPTEN__
#include "log.h"
static const struct log_api native_log = { log_write };
#endif

#ifdef __EMSCRIPTEN__
static const struct log_api *g_log;
#else
static const struct log_api *g_log = &native_log;
#endif

#define FG_MAX_PASSES     64
#define FG_MAX_RESOURCES  64
#define FG_MAX_PASS_DEPS  16

/* --- Internal structures ------------------------------------------------- */

struct fg_pass_ctx {
	gpu_cmd_buf_t         cmd;
	const struct gpu_api *gpu;
};

/* fg_resource / fg_pass are the concrete types behind the opaque handles */
struct fg_resource {
	char          name[64];
	fg_tex_desc   desc;
	gpu_texture_t handle;      /* transient: set by texture_create; imported: bound at import */
	uint8_t       allocated;   /* 1 after texture_create, 0 after destroy */
	uint8_t       imported;    /* 1 = external (backbuffer); graph binds but never owns */
	uint32_t      reader_count;
	uint32_t      first_write; /* sorted-order index; UINT32_MAX if unset */
	uint32_t      last_read;   /* sorted-order index; UINT32_MAX if unset */
};

struct fg_pass {
	char          name[64];
	fg_resource_t reads[FG_MAX_PASS_DEPS];
	uint32_t      read_count;
	fg_resource_t writes[FG_MAX_PASS_DEPS];
	uint32_t      write_count;
	fg_execute_fn execute_fn;
	void         *userdata;
	/* Per-pass attachment load config, indexed by color-attachment order */
	gpu_load_op   color_load_op[GPU_MAX_COLOR_ATTACHMENTS];
	float         color_clear[GPU_MAX_COLOR_ATTACHMENTS][4];
	gpu_load_op   depth_load_op;
	float         depth_clear;
	uint32_t      ref_count;  /* readers of our outputs among alive passes */
	uint32_t      in_degree;  /* for Kahn's topo sort */
	int           alive;
};

struct fg {
	const struct gpu_api *gpu;
	struct fg_resource    resources[FG_MAX_RESOURCES];
	uint32_t              resource_count;
	struct fg_pass        passes[FG_MAX_PASSES];
	uint32_t              pass_count;
	uint32_t              sorted[FG_MAX_PASSES];
	uint32_t              sorted_count;
};

/* --- Public API ---------------------------------------------------------- */

struct fg *fg_create(const struct gpu_api *gpu)
{
	struct fg *fg = calloc(1, sizeof(*fg));

	if (!fg)
		return NULL;
	fg->gpu = gpu;
	return fg;
}

void fg_destroy(struct fg *fg)
{
	free(fg);
}

fg_resource_t fg_declare_transient(struct fg *fg, const char *name,
				    fg_tex_desc desc)
{
	struct fg_resource *r;

	if (fg->resource_count >= FG_MAX_RESOURCES) {
		g_log->write(LOG_LEVEL_ERROR, "fg: resource limit (%u) reached", FG_MAX_RESOURCES);
		return NULL;
	}
	r = &fg->resources[fg->resource_count++];
	snprintf(r->name, sizeof(r->name), "%s", name);
	r->desc        = desc;
	r->handle      = NULL;
	r->allocated   = 0;
	r->imported    = 0;
	r->reader_count = 0;
	r->first_write = UINT32_MAX;
	r->last_read   = UINT32_MAX;
	return r;
}

fg_resource_t fg_import_backbuffer(struct fg *fg)
{
	struct fg_resource *r;

	if (fg->resource_count >= FG_MAX_RESOURCES) {
		g_log->write(LOG_LEVEL_ERROR, "fg: resource limit (%u) reached", FG_MAX_RESOURCES);
		return NULL;
	}
	r = &fg->resources[fg->resource_count++];
	snprintf(r->name, sizeof(r->name), "backbuffer");
	/*
	 * The default framebuffer / canvas. desc.format is descriptive only;
	 * the graph never creates storage for it. NULL handle is the backend's
	 * default-framebuffer sentinel.
	 */
	r->desc        = (fg_tex_desc){ .format = GPU_FORMAT_RGBA8_UNORM };
	r->handle      = NULL;
	r->allocated   = 0;
	r->imported    = 1;
	r->reader_count = 0;
	r->first_write = UINT32_MAX;
	r->last_read   = UINT32_MAX;
	return r;
}

fg_pass_t fg_pass_declare(struct fg *fg, const char *name,
			   fg_resource_t *reads,  uint32_t read_count,
			   fg_resource_t *writes, uint32_t write_count)
{
	struct fg_pass *p;
	uint32_t i;

	if (fg->pass_count >= FG_MAX_PASSES) {
		g_log->write(LOG_LEVEL_ERROR, "fg: pass limit (%u) reached", FG_MAX_PASSES);
		return NULL;
	}
	if (read_count > FG_MAX_PASS_DEPS || write_count > FG_MAX_PASS_DEPS) {
		g_log->write(LOG_LEVEL_ERROR, "fg: pass dependency limit (%u) exceeded",
			     FG_MAX_PASS_DEPS);
		return NULL;
	}
	p = &fg->passes[fg->pass_count++];
	snprintf(p->name, sizeof(p->name), "%s", name);
	for (i = 0; i < read_count; i++)
		p->reads[i] = reads[i];
	p->read_count = read_count;
	for (i = 0; i < write_count; i++)
		p->writes[i] = writes[i];
	p->write_count = write_count;
	p->alive = 1;
	return p;
}

void fg_pass_set_execute(fg_pass_t pass, fg_execute_fn cb, void *userdata)
{
	pass->execute_fn = cb;
	pass->userdata   = userdata;
}

void fg_pass_set_color_clear(fg_pass_t pass, uint32_t index,
			      const float rgba[4])
{
	if (index >= GPU_MAX_COLOR_ATTACHMENTS)
		return;
	pass->color_load_op[index] = GPU_LOAD_OP_CLEAR;
	pass->color_clear[index][0] = rgba[0];
	pass->color_clear[index][1] = rgba[1];
	pass->color_clear[index][2] = rgba[2];
	pass->color_clear[index][3] = rgba[3];
}

void fg_pass_set_depth_clear(fg_pass_t pass, float depth)
{
	pass->depth_load_op = GPU_LOAD_OP_CLEAR;
	pass->depth_clear   = depth;
}

/* --- Compile ------------------------------------------------------------- */

void fg_compile(struct fg *fg)
{
	uint32_t queue[FG_MAX_PASSES];
	uint32_t qhead, qtail;
	uint32_t i, j, k, m;

	/*
	 * Reset per-compile state. Imported resources (the backbuffer) carry
	 * an implicit external reference so the pass producing them is never
	 * culled, even though no in-graph pass reads them.
	 */
	for (i = 0; i < fg->resource_count; i++)
		fg->resources[i].reader_count = fg->resources[i].imported ? 1 : 0;
	for (i = 0; i < fg->pass_count; i++) {
		fg->passes[i].alive     = 1;
		fg->passes[i].ref_count = 0;
		fg->passes[i].in_degree = 0;
	}

	/* Count readers per resource */
	for (i = 0; i < fg->pass_count; i++) {
		struct fg_pass *p = &fg->passes[i];

		for (j = 0; j < p->read_count; j++)
			p->reads[j]->reader_count++;
	}

	/* Compute ref_count per pass: sum of reader_counts for written resources */
	for (i = 0; i < fg->pass_count; i++) {
		struct fg_pass *p = &fg->passes[i];

		for (j = 0; j < p->write_count; j++)
			p->ref_count += p->writes[j]->reader_count;
	}

	/*
	 * Cull dead passes (Frostbite reference-counting).
	 * A pass is dead if it has writes but no other alive pass reads them.
	 * Passes with no writes are terminal consumers — always alive.
	 */
	qhead = qtail = 0;
	for (i = 0; i < fg->pass_count; i++) {
		if (fg->passes[i].write_count > 0 &&
		    fg->passes[i].ref_count == 0)
			queue[qtail++] = i;
	}
	while (qhead < qtail) {
		struct fg_pass *p = &fg->passes[queue[qhead++]];

		if (!p->alive)
			continue;
		p->alive = 0;
		g_log->write(LOG_LEVEL_DEBUG, "fg: culling dead pass '%s'", p->name);

		/* Decrement reader_count for each resource this pass reads;
		 * propagate to the writer's ref_count */
		for (j = 0; j < p->read_count; j++) {
			struct fg_resource *r = p->reads[j];

			if (r->reader_count > 0)
				r->reader_count--;

			for (k = 0; k < fg->pass_count; k++) {
				struct fg_pass *w = &fg->passes[k];

				if (!w->alive)
					continue;
				for (m = 0; m < w->write_count; m++) {
					if (w->writes[m] != r)
						continue;
					if (w->ref_count > 0)
						w->ref_count--;
					if (w->ref_count == 0 &&
					    w->write_count > 0 &&
					    qtail < FG_MAX_PASSES)
						queue[qtail++] = k;
				}
			}
		}
	}

	/* Topological sort of alive passes — Kahn's BFS algorithm */
	for (i = 0; i < fg->pass_count; i++) {
		struct fg_pass *p = &fg->passes[i];

		if (!p->alive)
			continue;
		for (j = 0; j < p->read_count; j++) {
			struct fg_resource *r = p->reads[j];

			for (k = 0; k < fg->pass_count; k++) {
				struct fg_pass *w = &fg->passes[k];

				if (!w->alive || w == p)
					continue;
				for (m = 0; m < w->write_count; m++) {
					if (w->writes[m] == r) {
						p->in_degree++;
						break; /* one edge per writer */
					}
				}
			}
		}
	}

	fg->sorted_count = 0;
	qhead = qtail = 0;
	for (i = 0; i < fg->pass_count; i++) {
		if (fg->passes[i].alive && fg->passes[i].in_degree == 0)
			queue[qtail++] = i;
	}
	while (qhead < qtail) {
		uint32_t pidx   = queue[qhead++];
		struct fg_pass *p = &fg->passes[pidx];

		fg->sorted[fg->sorted_count++] = pidx;

		for (j = 0; j < p->write_count; j++) {
			struct fg_resource *r = p->writes[j];

			for (k = 0; k < fg->pass_count; k++) {
				struct fg_pass *dep = &fg->passes[k];

				if (!dep->alive)
					continue;
				for (m = 0; m < dep->read_count; m++) {
					if (dep->reads[m] != r)
						continue;
					dep->in_degree--;
					if (dep->in_degree == 0 &&
					    qtail < FG_MAX_PASSES)
						queue[qtail++] = k;
					break;
				}
			}
		}
	}

	/* Compute transient resource lifetimes in sorted execution order */
	for (i = 0; i < fg->resource_count; i++) {
		fg->resources[i].first_write = UINT32_MAX;
		fg->resources[i].last_read   = UINT32_MAX;
	}
	for (i = 0; i < fg->sorted_count; i++) {
		struct fg_pass *p = &fg->passes[fg->sorted[i]];

		for (j = 0; j < p->write_count; j++) {
			struct fg_resource *r = p->writes[j];

			if (r->first_write == UINT32_MAX)
				r->first_write = i;
		}
		for (j = 0; j < p->read_count; j++)
			p->reads[j]->last_read = i;
	}
}

/* --- Execute ------------------------------------------------------------- */

void fg_execute(struct fg *fg)
{
	const struct gpu_api *gpu = fg->gpu;
	struct fg_pass_ctx ctx;
	gpu_cmd_buf_t cmd;
	uint32_t i, j;

	cmd      = gpu->cmd_buf_begin();
	ctx.cmd  = cmd;
	ctx.gpu  = gpu;

	for (i = 0; i < fg->sorted_count; i++) {
		struct fg_pass *p = &fg->passes[fg->sorted[i]];

		struct gpu_render_pass_desc rp = { 0 };

		/* Allocate transients whose first write falls on this pass.
		 * Imported resources (backbuffer) are bound, never created. */
		for (j = 0; j < fg->resource_count; j++) {
			struct fg_resource *r = &fg->resources[j];

			if (r->first_write == i && !r->imported) {
				r->handle    = gpu->texture_create(&r->desc);
				r->allocated = 1;
			}
		}

		/*
		 * Auto barrier: for each resource this pass reads that was
		 * written by an earlier pass, emit a WRITE→READ hazard barrier.
		 */
		for (j = 0; j < p->read_count; j++) {
			struct fg_resource *r = p->reads[j];

			if (r->first_write != UINT32_MAX &&
			    r->first_write < i) {
				struct gpu_barrier b = {
					.src_stage   = GPU_STAGE_FRAGMENT,
					.dst_stage   = GPU_STAGE_FRAGMENT,
					.src_access  = GPU_ACCESS_SHADER_WRITE,
					.dst_access  = GPU_ACCESS_SHADER_READ,
					.hazard_flags = 0,
				};
				gpu->cmd_barrier(cmd, &b, 1);
			}
		}

		/*
		 * Render-pass setup is the graph's job. Build the pass desc
		 * from declared writes[]: depth-format writes become the depth
		 * attachment, others become color attachments in declaration
		 * order, each with this pass's load/clear config.
		 */
		for (j = 0; j < p->write_count; j++) {
			struct fg_resource *r = p->writes[j];

			if (r->desc.format == GPU_FORMAT_DEPTH32_FLOAT) {
				rp.depth          = r->handle;
				rp.depth_load_op  = p->depth_load_op;
				rp.depth_store_op = GPU_STORE_OP_STORE;
				rp.clear_depth    = p->depth_clear;
			} else if (rp.color_count < GPU_MAX_COLOR_ATTACHMENTS) {
				uint32_t c = rp.color_count++;

				rp.color[c].texture  = r->handle;
				rp.color[c].load_op  = p->color_load_op[c];
				rp.color[c].store_op = GPU_STORE_OP_STORE;
				rp.color[c].clear[0] = p->color_clear[c][0];
				rp.color[c].clear[1] = p->color_clear[c][1];
				rp.color[c].clear[2] = p->color_clear[c][2];
				rp.color[c].clear[3] = p->color_clear[c][3];
			}
		}

		gpu->cmd_begin_render_pass(cmd, &rp);

		/* The pass records real GPU commands on the lent context. */
		if (p->execute_fn)
			p->execute_fn(&ctx, p->userdata);

		gpu->cmd_end_render_pass(cmd);

		/* Free transients whose last read falls on this pass.
		 * Imported resources are never owned, so never freed. */
		for (j = 0; j < fg->resource_count; j++) {
			struct fg_resource *r = &fg->resources[j];

			if (r->last_read == i && r->allocated) {
				gpu->texture_destroy(r->handle);
				r->handle    = NULL;
				r->allocated = 0;
			}
		}
	}

	gpu->cmd_buf_submit(cmd);
}

/* --- Lent command context ------------------------------------------------ */

/*
 * Borrowed-not-kept: these are valid only for the duration of the execute
 * callback. The pass records commands with gpu->cmd_* on the lent cmd buffer;
 * it must never cache the api, the cmd buffer, or returned handles.
 */
const struct gpu_api *fg_ctx_gpu(struct fg_pass_ctx *ctx)
{
	return ctx->gpu;
}

gpu_cmd_buf_t fg_ctx_cmd(struct fg_pass_ctx *ctx)
{
	return ctx->cmd;
}

gpu_texture_t fg_ctx_resource(struct fg_pass_ctx *ctx, fg_resource_t r)
{
	(void)ctx;
	return r ? r->handle : NULL;
}

/* --- Plugin registration ------------------------------------------------- */

static const struct gpu_api *g_gpu;

static struct fg *api_create(void)
{
	return fg_create(g_gpu);
}

static const struct fg_api g_fg_api = {
	.create               = api_create,
	.destroy              = fg_destroy,
	.declare_transient    = fg_declare_transient,
	.import_backbuffer    = fg_import_backbuffer,
	.pass_declare         = fg_pass_declare,
	.pass_set_execute     = fg_pass_set_execute,
	.pass_set_color_clear = fg_pass_set_color_clear,
	.pass_set_depth_clear = fg_pass_set_depth_clear,
	.compile              = fg_compile,
	.execute              = fg_execute,
};

static void frame_graph_init(void)
{
	g_log->write(LOG_LEVEL_INFO, "frame_graph: init");
}

static void frame_graph_shutdown(void)
{
	g_log->write(LOG_LEVEL_INFO, "frame_graph: shutdown");
}

static const struct subsystem g_desc = {
	.name     = "frame_graph",
	.api      = &g_fg_api,
	.init     = frame_graph_init,
	.shutdown = frame_graph_shutdown,
};

/*
 * WASM: the engine's plugin_loader calls dlsym("plugin_entry").
 * Native: renamed to fg_plugin_entry to avoid symbol collision when
 * multiple plugin static libs are linked into a single test binary.
 */
#ifdef __EMSCRIPTEN__
void plugin_entry(struct subsystem_manager *mgr)
#else
void fg_plugin_entry(struct subsystem_manager *mgr)
#endif
{
#ifdef __EMSCRIPTEN__
	g_log = subsystem_manager_get_api(mgr, "log");
#endif
	g_gpu = subsystem_manager_get_api(mgr, "renderer");
	if (!g_gpu)
		g_log->write(LOG_LEVEL_WARN, "frame_graph: renderer not registered yet");
	subsystem_manager_register(mgr, &g_desc);
}
