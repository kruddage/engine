/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "frame_pacing.h"

void frame_pacing_init(struct frame_pacing *fp)
{
	fp->resync_pending = 1;
	fp->last_ms        = 0.0;
}

void frame_pacing_suspend(struct frame_pacing *fp)
{
	fp->resync_pending = 1;
}

double frame_pacing_tick(struct frame_pacing *fp, double now_ms)
{
	double dt;

	if (fp->resync_pending) {
		fp->resync_pending = 0;
		fp->last_ms        = now_ms;
		return FRAME_PACING_RESYNC;
	}

	dt          = now_ms - fp->last_ms;
	fp->last_ms = now_ms;
	return dt;
}
