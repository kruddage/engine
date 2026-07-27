/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The (krudd-stats) binding — see kgui_stats.h for why it is its own unit.
 */
#include "kgui_stats.h"

#include "s7.h"
#include "stats_api.h"

#include <stddef.h>

/*
 * One resolver for the process, matching the one process-global s7 interpreter
 * script_s7() hands out. Held rather than the vtable itself so the lookup stays
 * lazy: kruddgui boots before nothing in particular guarantees "stats" is
 * registered, and a stale NULL cached at registration time would strand the
 * HUD.
 */
static kgui_stats_resolve_fn s_resolve;

static s7_pointer sp_krudd_stats(s7_scheme *sc, s7_pointer args)
{
	const struct stats_api *st = s_resolve ? s_resolve() : NULL;

	(void)args;
	if (!st)
		return s7_f(sc);
	return s7_list(sc, 3, s7_make_real(sc, (s7_double)st->fps_avg),
		       s7_make_real(sc, (s7_double)st->last_frame_ms),
		       s7_make_integer(sc, (s7_int)st->frame_count));
}

void kgui_stats_register(struct s7_scheme *sc, kgui_stats_resolve_fn resolve)
{
	if (!sc)
		return;
	s_resolve = resolve;
	s7_define_function(sc, "krudd-stats", sp_krudd_stats, 0, 0, false,
			   "(krudd-stats) -> (fps frame-ms frame-count), "
			   "or #f when no stats subsystem is registered");
}
