/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FG_H
#define FG_H

#include "renderer.h"
#include <stdint.h>

/*
 * Frame graph plugin — Unreal/Frostbite-style render graph.
 *
 * The frame graph is the sole owner of struct gpu_api *. Higher-level
 * plugins build on the fg_api service; they never touch the GPU directly.
 *
 * Usage:
 *   struct fg *fg = fg_api->create();
 *   r = fg_api->declare_transient(fg, "depth", desc);
 *   p = fg_api->pass_declare(fg, "main", NULL, 0, &r, 1);
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
 * Opaque context passed to pass execute callbacks. Never exposes gpu_api *.
 * Use fg_cmd_* wrappers to record GPU work from within a pass.
 */
struct fg_pass_ctx;
typedef void (*fg_execute_fn)(struct fg_pass_ctx *ctx, void *userdata);

void fg_cmd_draw_indexed(struct fg_pass_ctx *ctx,
			  const struct gpu_draw_indexed_args *args);
void fg_cmd_dispatch(struct fg_pass_ctx *ctx,
		     uint32_t x, uint32_t y, uint32_t z);

/* --- Frame graph object -------------------------------------------------- */

struct fg;

struct fg *fg_create(const struct gpu_api *gpu);
void       fg_destroy(struct fg *fg);

fg_resource_t fg_declare_transient(struct fg *fg, const char *name,
				    fg_tex_desc desc);

fg_pass_t fg_pass_declare(struct fg *fg, const char *name,
			   fg_resource_t *reads,  uint32_t read_count,
			   fg_resource_t *writes, uint32_t write_count);

void fg_pass_set_execute(fg_pass_t pass, fg_execute_fn cb, void *userdata);

void fg_compile(struct fg *fg);
void fg_execute(struct fg *fg);

/* --- Service vtable registered as "frame_graph" with subsystem_manager --- */

struct fg_api {
	struct fg    *(*create)(void);
	void          (*destroy)(struct fg *fg);

	fg_resource_t (*declare_transient)(struct fg *fg, const char *name,
					   fg_tex_desc desc);
	fg_pass_t     (*pass_declare)(struct fg *fg, const char *name,
				      fg_resource_t *reads,  uint32_t read_count,
				      fg_resource_t *writes, uint32_t write_count);
	void          (*pass_set_execute)(fg_pass_t pass, fg_execute_fn cb,
					  void *userdata);
	void          (*compile)(struct fg *fg);
	void          (*execute)(struct fg *fg);
};

#endif /* FG_H */
