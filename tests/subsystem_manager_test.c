/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "subsystem_manager.h"

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

static int seq[32];
static int seq_idx;

static void reset(void) { seq_idx = 0; }
static void record(int v) { seq[seq_idx++] = v; }

static void a_init(void)     { record(1); }
static void a_tick(void)     { record(2); }
static void a_shutdown(void) { record(3); }
static void b_init(void)     { record(4); }
static void b_tick(void)     { record(5); }
static void b_shutdown(void) { record(6); }
static void c_init(void)     { record(7); }
static void c_tick(void)     { record(8); }
static void c_shutdown(void) { record(9); }

static const struct subsystem static_table[] = {
	{ "a", a_init, a_tick, a_shutdown },
	{ NULL }
};

static void test_static_lifecycle(void)
{
	struct subsystem_manager mgr;

	reset();
	subsystem_manager_init(&mgr, static_table);
	assert(seq_idx == 1);
	assert(seq[0] == 1); /* a_init */

	reset();
	subsystem_manager_tick(&mgr);
	assert(seq_idx == 1);
	assert(seq[0] == 2); /* a_tick */

	reset();
	subsystem_manager_shutdown(&mgr);
	assert(seq_idx == 1);
	assert(seq[0] == 3); /* a_shutdown */
}

static void test_dynamic_register_calls_init(void)
{
	struct subsystem_manager mgr;
	struct subsystem dyn = { "b", b_init, b_tick, b_shutdown };

	reset();
	subsystem_manager_init(&mgr, static_table);
	assert(seq_idx == 1); /* a_init */

	subsystem_manager_register(&mgr, &dyn);
	assert(seq_idx == 2); /* b_init called immediately */
	assert(seq[1] == 4);
}

static void test_tick_order_static_then_dynamic(void)
{
	struct subsystem_manager mgr;
	struct subsystem dyn = { "b", b_init, b_tick, b_shutdown };

	subsystem_manager_init(&mgr, static_table);
	subsystem_manager_register(&mgr, &dyn);

	reset();
	subsystem_manager_tick(&mgr);
	assert(seq_idx == 2);
	assert(seq[0] == 2); /* a_tick first */
	assert(seq[1] == 5); /* b_tick second */
}

static void test_shutdown_dynamic_before_static(void)
{
	struct subsystem_manager mgr;
	struct subsystem dyn = { "b", b_init, b_tick, b_shutdown };

	subsystem_manager_init(&mgr, static_table);
	subsystem_manager_register(&mgr, &dyn);

	reset();
	subsystem_manager_shutdown(&mgr);
	assert(seq_idx == 2);
	assert(seq[0] == 6); /* b_shutdown first (dynamic) */
	assert(seq[1] == 3); /* a_shutdown second (static) */
}

static void test_shutdown_dynamic_reverse_order(void)
{
	struct subsystem_manager mgr;
	struct subsystem empty[] = {{ NULL }};
	struct subsystem db = { "b", b_init, NULL, b_shutdown };
	struct subsystem dc = { "c", c_init, NULL, c_shutdown };

	subsystem_manager_init(&mgr, empty);
	subsystem_manager_register(&mgr, &db);
	subsystem_manager_register(&mgr, &dc);

	reset();
	subsystem_manager_shutdown(&mgr);
	assert(seq_idx == 2);
	assert(seq[0] == 9); /* c_shutdown first (registered last) */
	assert(seq[1] == 6); /* b_shutdown second */
}

static void test_max_dynamic_slots(void)
{
	struct subsystem_manager mgr;
	struct subsystem empty[] = {{ NULL }};
	struct subsystem dyn = { "x", NULL, NULL, NULL };
	int i;

	subsystem_manager_init(&mgr, empty);
	for (i = 0; i < SUBSYSTEM_MANAGER_MAX_DYNAMIC + 5; i++)
		subsystem_manager_register(&mgr, &dyn);

	assert(mgr.dynamic_count == SUBSYSTEM_MANAGER_MAX_DYNAMIC);
}

int main(void)
{
	RUN(static_lifecycle);
	RUN(dynamic_register_calls_init);
	RUN(tick_order_static_then_dynamic);
	RUN(shutdown_dynamic_before_static);
	RUN(shutdown_dynamic_reverse_order);
	RUN(max_dynamic_slots);

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
