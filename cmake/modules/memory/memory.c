/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "memory.h"

#include <stdlib.h>

/*
 * The allocator seam. Everything in the engine allocates through here (or the
 * plugin memory_api vtable) so there is a single, swappable heap.
 *
 * These wrappers deliberately call the libc malloc family directly. On the
 * WASM target the main module is linked -sMALLOC=mimalloc, so libc malloc IS
 * mimalloc — libc, the FETCH and dynamic-linker runtimes, every side-module
 * plugin, and this seam all share one mimalloc heap. On native builds it is the
 * platform libc. Either way there is exactly one allocator over the heap;
 * routing this file through a second, independently-linked allocator is what
 * previously corrupted memory once the WASM heap grew.
 *
 * modules/memory/ is the sole place check-no-raw-malloc.sh permits the raw
 * malloc family.
 */

void mem_init(void)
{
}

void mem_shutdown(void)
{
}

void *mem_alloc(size_t size)
{
	return malloc(size);
}

void *mem_alloc_zero(size_t size)
{
	return calloc(1, size);
}

void mem_free(void *ptr)
{
	free(ptr);
}

/*
 * The pool allocator is a thin wrapper over the shared heap: it remembers the
 * fixed object size so callers allocate without repeating it. No separate
 * arena — freeing an object returns it straight to the heap.
 */
struct mem_pool {
	size_t obj_size;
};

struct mem_pool *mem_pool_create(size_t obj_size)
{
	struct mem_pool *pool = malloc(sizeof(*pool));

	if (!pool)
		return NULL;
	pool->obj_size = obj_size;
	return pool;
}

void *mem_pool_alloc(struct mem_pool *pool)
{
	return malloc(pool->obj_size);
}

void mem_pool_free(struct mem_pool *pool, void *ptr)
{
	(void)pool;
	free(ptr);
}

void mem_pool_destroy(struct mem_pool *pool)
{
	free(pool);
}
