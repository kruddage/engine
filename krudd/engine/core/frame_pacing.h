/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FRAME_PACING_H
#define FRAME_PACING_H

#include <stdint.h>

/*
 * What a frame loop needs to stay honest across a driver handoff: a note that
 * the time since the last sample belongs to whatever suspended the loop, not
 * to the tick that happens to land next. The same state serves an
 * uninterrupted rAF loop, one that has been suspended, and one an external
 * host is driving by hand — none of that has to know which of those is
 * happening right now, only whether it just resumed from a gap.
 */
struct frame_pacing {
	int32_t resync_pending;
	double  last_ms;
};

void frame_pacing_init(struct frame_pacing *fp);

/*
 * Mark that the next tick's elapsed time must not be folded into a
 * frame-time average. Called when the loop is about to be suspended (a host
 * is taking it over, or handing it back): whatever wall-clock time passes
 * before the next sample belongs to that gap, not to a frame that took that
 * long to render.
 */
void frame_pacing_suspend(struct frame_pacing *fp);

/* frame_pacing_tick's return value when NOW is not a meaningful duration. */
#define FRAME_PACING_RESYNC (-1.0)

/*
 * Report the elapsed time since the previous call, in milliseconds. Returns
 * FRAME_PACING_RESYNC instead of a duration on the very first call and on
 * the first call after frame_pacing_suspend(): in both cases NOW says
 * nothing about how long a frame took, so the caller should record the tick
 * happened without averaging this call in.
 */
double frame_pacing_tick(struct frame_pacing *fp, double now_ms);

#endif /* FRAME_PACING_H */
