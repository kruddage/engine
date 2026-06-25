/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef MEMORY_API_H
#define MEMORY_API_H

/*
 * Plugin-facing memory interface. Plugins obtain a pointer to this struct via
 * subsystem_manager_get_api(mgr, "memory") and call through it — no direct
 * import of mem_alloc/mem_free etc. from the main module.
 */

#include <stddef.h>

struct mem_pool;

struct memory_api {
	void            *(*alloc)(size_t size);
	void            *(*alloc_zero)(size_t size);
	void             (*free)(void *ptr);
	struct mem_pool *(*pool_create)(size_t obj_size);
	void            *(*pool_alloc)(struct mem_pool *pool);
	void             (*pool_free)(struct mem_pool *pool, void *ptr);
	void             (*pool_destroy)(struct mem_pool *pool);
};

#endif /* MEMORY_API_H */
