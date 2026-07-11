/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>

void  mem_init(void);
void  mem_shutdown(void);

void *mem_alloc(size_t size);
void *mem_alloc_zero(size_t size);
void  mem_free(void *ptr);

struct mem_pool *mem_pool_create(size_t obj_size);
void            *mem_pool_alloc(struct mem_pool *pool);
void             mem_pool_free(struct mem_pool *pool, void *ptr);
void             mem_pool_destroy(struct mem_pool *pool);

#endif /* MEMORY_H */
