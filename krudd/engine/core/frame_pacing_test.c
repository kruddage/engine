/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "frame_pacing.h"

#include <assert.h>
#include <stdio.h>

static int tests_run;
static int tests_passed;

#define RUN(name) do { \
	tests_run++; \
	test_##name(); \
	tests_passed++; \
	printf("PASS: " #name "\n"); \
} while (0)

/* The very first tick has nothing to measure a delta against. */
static void test_first_tick_resyncs(void)
{
	struct frame_pacing fp;

	frame_pacing_init(&fp);
	assert(frame_pacing_tick(&fp, 1000.0) == FRAME_PACING_RESYNC);
}

/* Once past the opening resync, an ordinary run of ticks reports the real
 * gap between them — this is the case the browser's rAF loop hits every
 * frame, and it must come out unchanged by anything this module adds. */
static void test_steady_ticks_report_real_deltas(void)
{
	struct frame_pacing fp;

	frame_pacing_init(&fp);
	frame_pacing_tick(&fp, 1000.0);
	assert(frame_pacing_tick(&fp, 1016.0) == 16.0);
	assert(frame_pacing_tick(&fp, 1033.0) == 17.0);
}

/* A suspend followed by a driven tick seconds later must not report that
 * gap as a frame duration — the whole reason this module exists. */
static void test_suspend_resyncs_the_next_tick(void)
{
	struct frame_pacing fp;

	frame_pacing_init(&fp);
	frame_pacing_tick(&fp, 1000.0);
	frame_pacing_tick(&fp, 1016.0);

	frame_pacing_suspend(&fp);
	/* Five seconds pass while an external host owns the loop. */
	assert(frame_pacing_tick(&fp, 6016.0) == FRAME_PACING_RESYNC);

	/* Ticks resume reporting real deltas from here, not from before the
	 * suspend — the gap is fully absorbed by the one resync above. */
	assert(frame_pacing_tick(&fp, 6027.0) == 11.0);
}

/* Suspending more than once before the next tick is still exactly one
 * resync — a host that calls suspend defensively must not get charged for
 * it twice. */
static void test_repeated_suspend_is_one_resync(void)
{
	struct frame_pacing fp;

	frame_pacing_init(&fp);
	frame_pacing_tick(&fp, 1000.0);

	frame_pacing_suspend(&fp);
	frame_pacing_suspend(&fp);
	assert(frame_pacing_tick(&fp, 9000.0) == FRAME_PACING_RESYNC);
	assert(frame_pacing_tick(&fp, 9010.0) == 10.0);
}

/* A driven tick that lands right after a suspend, with essentially no gap
 * at all (the host takes over immediately), still resyncs rather than
 * reporting a near-zero delta — the resync is unconditional on the first
 * tick after a suspend, not a threshold on how long the gap was. */
static void test_suspend_resyncs_even_a_short_gap(void)
{
	struct frame_pacing fp;

	frame_pacing_init(&fp);
	frame_pacing_tick(&fp, 1000.0);
	frame_pacing_tick(&fp, 1016.0);

	frame_pacing_suspend(&fp);
	assert(frame_pacing_tick(&fp, 1017.0) == FRAME_PACING_RESYNC);
	assert(frame_pacing_tick(&fp, 1028.0) == 11.0);
}

int main(void)
{
	RUN(first_tick_resyncs);
	RUN(steady_ticks_report_real_deltas);
	RUN(suspend_resyncs_the_next_tick);
	RUN(repeated_suspend_is_one_resync);
	RUN(suspend_resyncs_even_a_short_gap);

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
