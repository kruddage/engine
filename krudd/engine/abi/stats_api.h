/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef STATS_API_H
#define STATS_API_H

#include <stdint.h>

/* Most startup phases the boot profiler records (one per plugin plus the
 * script_init / runtime-image bookends). Overflow phases are dropped. */
#define STATS_MAX_PHASES 16

/* One named startup phase: how long a slice of engine_init took to run.
 * name points at a static string literal in the module, valid for its life. */
struct stats_phase {
	const char *name;
	float       ms;
};

struct stats_api {
	float    last_frame_ms;
	float    fps_avg;
	uint32_t frame_count;

	/*
	 * Startup profiling. Filled once during engine_init (and first tick)
	 * and never mutated after, so a reader always sees a settled boot
	 * picture. init_ms is the whole engine_init wall time; first_frame_ms
	 * is boot-start to the first engine_tick (init plus the browser's first
	 * animation-frame round-trip). phases[0..phase_count) breaks init_ms
	 * down by the slice that produced each cost.
	 */
	float              init_ms;
	float              first_frame_ms;
	uint32_t           phase_count;
	struct stats_phase phases[STATS_MAX_PHASES];
};

#endif /* STATS_API_H */
