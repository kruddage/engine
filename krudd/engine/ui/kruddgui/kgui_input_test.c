/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Host tests for the pointer router (kgui_input). They drive the router with
 * synthetic pointer events — no Emscripten, no GL — and assert the routing
 * decisions and the per-region results: capture on down, drag accumulation,
 * same-region taps, z-order, multi-touch independence, single-pointer host
 * forwarding, wheel routing, and the per-frame clear at commit.
 */

#include "kgui_input.h"

#include <assert.h>
#include <stdio.h>

static int tests_run;
static int tests_passed;

#define RUN(name) do {                 \
	tests_run++;                   \
	test_##name();                 \
	tests_passed++;                \
	printf("PASS: " #name "\n");   \
} while (0)

/* Two side-by-side regions used by most tests: BAR left, LOG right. */
#define BAR kgui_name_hash("bar")
#define LOG kgui_name_hash("log")

static void commit_two(struct kgui_input *in)
{
	kgui_input_frame_begin(in);
	kgui_input_region(in, BAR, 0.0f, 0.0f, 100.0f, 50.0f);
	kgui_input_region(in, LOG, 200.0f, 0.0f, 100.0f, 50.0f);
	kgui_input_frame_commit(in);
}

/* Re-declare a region and hand back its accumulated result slot to read. */
static struct kgui_region_io *read_bar(struct kgui_input *in)
{
	kgui_input_frame_begin(in);
	return kgui_input_region(in, BAR, 0.0f, 0.0f, 100.0f, 50.0f);
}

static void test_name_hash_nonzero(void)
{
	assert(kgui_name_hash("bar") != 0);
	assert(kgui_name_hash(NULL) != 0);
	assert(kgui_name_hash("bar") != kgui_name_hash("log"));
	assert(kgui_name_hash("bar") == kgui_name_hash("bar"));
}

static void test_down_on_region_is_consumed(void)
{
	struct kgui_input in;
	struct kgui_region_io *io;

	kgui_input_init(&in);
	commit_two(&in);

	assert(kgui_input_pointer_down(&in, 1, 10.0f, 10.0f) ==
	       KGUI_ROUTE_CONSUMED);

	io = read_bar(&in);
	assert(io->pressed);
	assert(!io->tapped);
}

static void test_miss_is_forwarded(void)
{
	struct kgui_input in;

	kgui_input_init(&in);
	commit_two(&in);

	/* Down between the two regions reaches the host. */
	assert(kgui_input_pointer_down(&in, 1, 150.0f, 10.0f) ==
	       KGUI_ROUTE_FORWARD);
	assert(kgui_input_pointer_move(&in, 1, 155.0f, 10.0f) ==
	       KGUI_ROUTE_FORWARD);
	assert(kgui_input_pointer_up(&in, 1, 155.0f, 10.0f) ==
	       KGUI_ROUTE_FORWARD);
}

static void test_tap_same_region(void)
{
	struct kgui_input in;
	struct kgui_region_io *io;

	kgui_input_init(&in);
	commit_two(&in);

	kgui_input_pointer_down(&in, 1, 10.0f, 10.0f);
	assert(kgui_input_pointer_up(&in, 1, 20.0f, 12.0f) ==
	       KGUI_ROUTE_CONSUMED);

	io = read_bar(&in);
	assert(io->tapped);
	assert(io->tap_x == 20.0f && io->tap_y == 12.0f);
	assert(!io->pressed);
}

static void test_up_outside_region_no_tap(void)
{
	struct kgui_input in;
	struct kgui_region_io *io;

	kgui_input_init(&in);
	commit_two(&in);

	kgui_input_pointer_down(&in, 1, 10.0f, 10.0f);
	/* Drift off the region before release: a captured drag, not a tap. */
	assert(kgui_input_pointer_up(&in, 1, 150.0f, 10.0f) ==
	       KGUI_ROUTE_CONSUMED);

	io = read_bar(&in);
	assert(!io->tapped);
	assert(!io->pressed);
}

static void test_drag_accumulates(void)
{
	struct kgui_input in;
	struct kgui_region_io *io;

	kgui_input_init(&in);
	commit_two(&in);

	kgui_input_pointer_down(&in, 1, 10.0f, 10.0f);
	assert(kgui_input_pointer_move(&in, 1, 13.0f, 16.0f) ==
	       KGUI_ROUTE_CONSUMED);
	assert(kgui_input_pointer_move(&in, 1, 15.0f, 20.0f) ==
	       KGUI_ROUTE_CONSUMED);

	io = read_bar(&in);
	assert(io->drag_dx == 5.0f);  /* 3 + 2 */
	assert(io->drag_dy == 10.0f); /* 6 + 4 */
	assert(io->pressed);
}

static void test_commit_clears_per_frame_fields(void)
{
	struct kgui_input in;
	struct kgui_region_io *io;

	kgui_input_init(&in);
	commit_two(&in);

	kgui_input_pointer_down(&in, 1, 10.0f, 10.0f);
	kgui_input_pointer_move(&in, 1, 15.0f, 15.0f);
	kgui_input_pointer_up(&in, 1, 15.0f, 15.0f);

	io = read_bar(&in);
	assert(io->tapped && io->drag_dx == 5.0f);
	kgui_input_frame_commit(&in);

	/* After commit the one-frame results are gone; pressed stays live. */
	io = read_bar(&in);
	assert(!io->tapped);
	assert(io->drag_dx == 0.0f && io->drag_dy == 0.0f);
	assert(!io->pressed);
}

static void test_zorder_topmost_wins(void)
{
	struct kgui_input in;
	struct kgui_region_io *lo, *hi;

	kgui_input_init(&in);
	kgui_input_frame_begin(&in);
	kgui_input_region(&in, BAR, 0.0f, 0.0f, 100.0f, 100.0f);
	kgui_input_region(&in, LOG, 0.0f, 0.0f, 100.0f, 100.0f); /* on top */
	kgui_input_frame_commit(&in);

	kgui_input_pointer_down(&in, 1, 10.0f, 10.0f);
	kgui_input_pointer_up(&in, 1, 10.0f, 10.0f);

	kgui_input_frame_begin(&in);
	lo = kgui_input_region(&in, BAR, 0.0f, 0.0f, 100.0f, 100.0f);
	hi = kgui_input_region(&in, LOG, 0.0f, 0.0f, 100.0f, 100.0f);
	assert(hi->tapped);   /* the later-declared region owns the overlap */
	assert(!lo->tapped);
}

static void test_multitouch_independent(void)
{
	struct kgui_input in;
	struct kgui_region_io *bar, *log;

	kgui_input_init(&in);
	commit_two(&in);

	/* Finger 1 taps BAR; finger 2 drags LOG off its edge — concurrently. */
	kgui_input_pointer_down(&in, 1, 10.0f, 10.0f);
	kgui_input_pointer_down(&in, 2, 210.0f, 10.0f);
	kgui_input_pointer_move(&in, 2, 210.0f, 30.0f);
	kgui_input_pointer_up(&in, 1, 12.0f, 10.0f);
	kgui_input_pointer_up(&in, 2, 210.0f, 80.0f); /* released below LOG */

	kgui_input_frame_begin(&in);
	bar = kgui_input_region(&in, BAR, 0.0f, 0.0f, 100.0f, 50.0f);
	log = kgui_input_region(&in, LOG, 200.0f, 0.0f, 100.0f, 50.0f);

	assert(bar->tapped);          /* finger 1's tap landed on BAR only */
	assert(bar->drag_dy == 0.0f);
	assert(!log->tapped);         /* finger 2 dragged off-tap on LOG */
	assert(log->drag_dy == 20.0f); /* the 10->30 move; the up adds none */
}

static void test_second_unclaimed_pointer_swallowed(void)
{
	struct kgui_input in;

	kgui_input_init(&in);
	commit_two(&in);

	/* First unclaimed finger drives the host; a second is swallowed so it
	 * can't fight the first for ImGui's single pointer. */
	assert(kgui_input_pointer_down(&in, 1, 150.0f, 10.0f) ==
	       KGUI_ROUTE_FORWARD);
	assert(kgui_input_pointer_down(&in, 2, 160.0f, 40.0f) ==
	       KGUI_ROUTE_CONSUMED);
	assert(kgui_input_pointer_move(&in, 2, 165.0f, 40.0f) ==
	       KGUI_ROUTE_CONSUMED);
	assert(kgui_input_pointer_up(&in, 2, 165.0f, 40.0f) ==
	       KGUI_ROUTE_CONSUMED);
	assert(kgui_input_pointer_up(&in, 1, 150.0f, 10.0f) ==
	       KGUI_ROUTE_FORWARD);
}

static void test_region_press_does_not_block_host_pointer(void)
{
	struct kgui_input in;

	kgui_input_init(&in);
	commit_two(&in);

	/* A finger captured by a region must not count as the host's pointer:
	 * an unclaimed finger still forwards while the region is held. */
	assert(kgui_input_pointer_down(&in, 1, 10.0f, 10.0f) ==
	       KGUI_ROUTE_CONSUMED);
	assert(kgui_input_pointer_down(&in, 2, 150.0f, 10.0f) ==
	       KGUI_ROUTE_FORWARD);
}

static void test_wheel_routing(void)
{
	struct kgui_input in;
	struct kgui_region_io *io;

	kgui_input_init(&in);
	commit_two(&in);

	assert(kgui_input_wheel(&in, 210.0f, 10.0f, 3.0f) ==
	       KGUI_ROUTE_CONSUMED);
	assert(kgui_input_wheel(&in, 150.0f, 10.0f, 3.0f) ==
	       KGUI_ROUTE_FORWARD);

	kgui_input_frame_begin(&in);
	kgui_input_region(&in, BAR, 0.0f, 0.0f, 100.0f, 50.0f);
	io = kgui_input_region(&in, LOG, 200.0f, 0.0f, 100.0f, 50.0f);
	assert(io->wheel == 3.0f);
}

/*
 * A slider (or 2D picker) is a small region declared ON TOP of the scroll body,
 * after it in draw order. A down inside the slider is captured by the slider —
 * its press position tracks the pointer so the widget can map it to a value —
 * while the body stays unpressed, so a drag on the slider never also scrolls.
 * A down on the body away from the slider still hits the body. This is the whole
 * per-widget drag-capture mechanism kruddgui-slider / -color-swatch build on, no
 * kgui_input change needed.
 */
static void commit_body_and_slider(struct kgui_input *in)
{
	kgui_input_frame_begin(in);
	kgui_input_region(in, BAR, 0.0f, 0.0f, 300.0f, 300.0f); /* scroll body */
	kgui_input_region(in, LOG, 50.0f, 100.0f, 200.0f, 40.0f); /* slider ontop */
	kgui_input_frame_commit(in);
}

static void test_slider_captures_over_body(void)
{
	struct kgui_input *inp;
	struct kgui_input in;
	struct kgui_region_io *body, *slider;

	inp = &in;
	kgui_input_init(inp);
	commit_body_and_slider(inp);

	/* Down inside the slider rect: the slider (later-declared) captures it. */
	assert(kgui_input_pointer_down(inp, 1, 120.0f, 120.0f) ==
	       KGUI_ROUTE_CONSUMED);
	assert(kgui_input_pointer_move(inp, 1, 180.0f, 121.0f) ==
	       KGUI_ROUTE_CONSUMED);

	kgui_input_frame_begin(inp);
	body   = kgui_input_region(inp, BAR, 0.0f, 0.0f, 300.0f, 300.0f);
	slider = kgui_input_region(inp, LOG, 50.0f, 100.0f, 200.0f, 40.0f);

	assert(slider->pressed);          /* the slider owns the gesture */
	assert(slider->press_x == 180.0f); /* live x maps to the value */
	assert(slider->drag_dx == 60.0f);  /* and the accumulated drag */
	assert(!body->pressed);            /* so the body does NOT scroll */
	assert(body->drag_dx == 0.0f);
}

static void test_down_off_slider_hits_body(void)
{
	struct kgui_input in;
	struct kgui_region_io *body, *slider;

	kgui_input_init(&in);
	commit_body_and_slider(&in);

	/* Down on the body but outside the slider: the body captures it. */
	assert(kgui_input_pointer_down(&in, 1, 20.0f, 20.0f) ==
	       KGUI_ROUTE_CONSUMED);
	kgui_input_pointer_move(&in, 1, 20.0f, 60.0f);

	kgui_input_frame_begin(&in);
	body   = kgui_input_region(&in, BAR, 0.0f, 0.0f, 300.0f, 300.0f);
	slider = kgui_input_region(&in, LOG, 50.0f, 100.0f, 200.0f, 40.0f);

	assert(body->pressed);           /* the body owns it — this scrolls */
	assert(body->drag_dy == 40.0f);
	assert(!slider->pressed);
}

static void test_mouse_hover_forwards(void)
{
	struct kgui_input in;

	kgui_input_init(&in);
	commit_two(&in);

	/* A move with no preceding down (hover) is the host's, even over a
	 * region, so ImGui hover keeps working. */
	assert(kgui_input_pointer_move(&in, KGUI_MOUSE_ID, 10.0f, 10.0f) ==
	       KGUI_ROUTE_FORWARD);
}

int main(void)
{
	RUN(name_hash_nonzero);
	RUN(down_on_region_is_consumed);
	RUN(miss_is_forwarded);
	RUN(tap_same_region);
	RUN(up_outside_region_no_tap);
	RUN(drag_accumulates);
	RUN(commit_clears_per_frame_fields);
	RUN(zorder_topmost_wins);
	RUN(multitouch_independent);
	RUN(second_unclaimed_pointer_swallowed);
	RUN(region_press_does_not_block_host_pointer);
	RUN(slider_captures_over_body);
	RUN(down_off_slider_hits_body);
	RUN(wheel_routing);
	RUN(mouse_hover_forwards);

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
