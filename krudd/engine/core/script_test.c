/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "script.h"

#include "log.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int tests_run;
static int tests_passed;

#define RUN(name) do { \
	tests_run++; \
	test_##name(); \
	tests_passed++; \
	printf("PASS: " #name "\n"); \
} while (0)

/* True if the log history holds a message whose text contains NEEDLE. */
static int history_has(const char *needle)
{
	struct log_message hist[LOG_HISTORY_CAP];
	uint32_t           n = log_get_history(hist, LOG_HISTORY_CAP);
	uint32_t           i;

	for (i = 0; i < n; i++)
		if (strstr(hist[i].text, needle))
			return 1;
	return 0;
}

/* A primitive call from the image crosses back into the engine's log. */
static void test_primitive_reaches_log(void)
{
	assert(script_eval("(krudd-log 1 \"scheme spoke\")") == 0);
	assert(history_has("scheme spoke"));
}

/* The engine hands the frame to the image's (tick). */
static void test_tick_hook_runs(void)
{
	assert(script_eval(
		"(define *n* 0)"
		"(define (tick)"
		"  (set! *n* (+ *n* 1))"
		"  (krudd-log 1 (string-append \"tick \" (number->string *n*))))")
	       == 0);
	script_tick();
	script_tick();
	assert(history_has("tick 2"));
}

/* Ticking with no (tick) defined, and eval before init, are both safe. */
static void test_degrades_safely(void)
{
	script_shutdown();
	assert(script_eval("(krudd-log 1 \"ignored\")") == -1);
	script_tick(); /* no interpreter — must not crash */
}

int main(void)
{
	log_init();
	script_init();

	RUN(primitive_reaches_log);
	RUN(tick_hook_runs);
	RUN(degrades_safely);

	printf("%d/%d script tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
