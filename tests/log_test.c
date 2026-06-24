/* SPDX-License-Identifier: MIT */
#include "log.h"

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

static void test_all_levels(void)
{
	log_set_level(LOG_DEBUG);
	log_debug("debug message");
	log_info("info message");
	log_warn("warn message");
	log_error("error message");
}

static void test_level_filter(void)
{
	/* set to WARN — debug and info calls must not crash */
	log_set_level(LOG_WARN);
	log_debug("suppressed");
	log_info("suppressed");
	log_warn("visible");
	log_error("visible");
}

static void test_format(void)
{
	log_set_level(LOG_DEBUG);
	log_debug("value: %d", 42);
	log_info("string: %s", "hello");
	log_warn("float: %.2f", 3.14);
}

int main(void)
{
	log_init();

	RUN(all_levels);
	RUN(level_filter);
	RUN(format);

	log_shutdown();

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
