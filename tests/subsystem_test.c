/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "subsystem.h"

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

static int seq[16];
static int seq_idx;

static void reset(void) { seq_idx = 0; }
static void record(int v) { seq[seq_idx++] = v; }

static void a_init(void)     { record(1); }
static void a_tick(void)     { record(2); }
static void a_shutdown(void) { record(3); }
static void b_init(void)     { record(4); }
static void b_shutdown(void) { record(5); }

static const struct subsystem two_subsystems[] = {
	{ "a", a_init, a_tick, a_shutdown },
	{ "b", b_init, NULL,   b_shutdown },
	{ NULL }
};

static void test_init_order(void)
{
	reset();
	subsystem_init_all(two_subsystems);
	assert(seq_idx == 2);
	assert(seq[0] == 1); /* a_init first */
	assert(seq[1] == 4); /* b_init second */
}

static void test_shutdown_reverse(void)
{
	reset();
	subsystem_shutdown_all(two_subsystems);
	assert(seq_idx == 2);
	assert(seq[0] == 5); /* b_shutdown first */
	assert(seq[1] == 3); /* a_shutdown second */
}

static void test_tick_skips_null(void)
{
	reset();
	subsystem_tick_all(two_subsystems);
	assert(seq_idx == 1); /* only a has a tick */
	assert(seq[0] == 2);
}

static void test_empty_table(void)
{
	static const struct subsystem empty[] = {{ NULL }};

	subsystem_init_all(empty);
	subsystem_tick_all(empty);
	subsystem_shutdown_all(empty);
}

int main(void)
{
	RUN(init_order);
	RUN(shutdown_reverse);
	RUN(tick_skips_null);
	RUN(empty_table);

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
