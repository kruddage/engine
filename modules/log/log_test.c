/* SPDX-License-Identifier: GPL-2.0-or-later */
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
	log_set_level(LOG_LEVEL_DEBUG);
	LOG_DEBUG("debug message");
	LOG_INFO("info message");
	LOG_WARN("warn message");
	LOG_ERROR("error message");
}

static void test_level_filter(void)
{
	/* set to WARN — debug and info calls must not crash */
	log_set_level(LOG_LEVEL_WARN);
	LOG_DEBUG("suppressed");
	LOG_INFO("suppressed");
	LOG_WARN("visible");
	LOG_ERROR("visible");
}

static void test_format(void)
{
	log_set_level(LOG_LEVEL_DEBUG);
	LOG_DEBUG("value: %d", 42);
	LOG_INFO("string: %s", "hello");
	LOG_WARN("float: %.2f", 3.14);
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
