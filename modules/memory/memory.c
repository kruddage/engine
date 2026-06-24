/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "memory.h"

#include <mimalloc.h>
#include <stddef.h>

struct mem_pool {
	mi_heap_t *heap;
	size_t     obj_size;
};

void mem_init(void)
{
	mi_option_disable(mi_option_verbose);
}

void mem_shutdown(void)
{
	mi_collect(1);
}

void *mem_alloc(size_t size)
{
	return mi_malloc(size);
}

void *mem_alloc_zero(size_t size)
{
	return mi_zalloc(size);
}

void mem_free(void *ptr)
{
	mi_free(ptr);
}

struct mem_pool *mem_pool_create(size_t obj_size)
{
	struct mem_pool *pool = mi_zalloc(sizeof(*pool));
	if (!pool)
		return NULL;
	pool->heap = mi_heap_new();
	if (!pool->heap) {
		mi_free(pool);
		return NULL;
	}
	pool->obj_size = obj_size;
	return pool;
}

void *mem_pool_alloc(struct mem_pool *pool)
{
	return mi_heap_malloc(pool->heap, pool->obj_size);
}

void mem_pool_free(struct mem_pool *pool, void *ptr)
{
	(void)pool;
	mi_free(ptr);
}

void mem_pool_destroy(struct mem_pool *pool)
{
	mi_heap_destroy(pool->heap);
	mi_free(pool);
}
