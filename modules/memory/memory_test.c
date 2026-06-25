/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "memory.h"

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

static void test_basic_alloc(void)
{
	void *p = mem_alloc(64);
	assert(p != NULL);
	mem_free(p);
}

static void test_zero_alloc(void)
{
	unsigned char *p = mem_alloc_zero(16);
	int i;

	assert(p != NULL);
	for (i = 0; i < 16; i++)
		assert(p[i] == 0);
	mem_free(p);
}

static void test_pool_alloc_free(void)
{
	struct mem_pool *pool = mem_pool_create(32);
	void *a, *b;

	assert(pool != NULL);
	a = mem_pool_alloc(pool);
	b = mem_pool_alloc(pool);
	assert(a != NULL);
	assert(b != NULL);
	assert(a != b);
	mem_pool_free(pool, a);
	mem_pool_free(pool, b);
	mem_pool_destroy(pool);
}

static void test_pool_reuse(void)
{
	struct mem_pool *pool = mem_pool_create(8);
	void *a, *b;

	assert(pool != NULL);
	a = mem_pool_alloc(pool);
	assert(a != NULL);
	mem_pool_free(pool, a);
	b = mem_pool_alloc(pool);
	assert(b != NULL);
	mem_pool_free(pool, b);
	mem_pool_destroy(pool);
}

int main(void)
{
	mem_init();

	RUN(basic_alloc);
	RUN(zero_alloc);
	RUN(pool_alloc_free);
	RUN(pool_reuse);

	mem_shutdown();

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
