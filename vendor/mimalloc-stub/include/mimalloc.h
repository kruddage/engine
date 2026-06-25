/* Minimal mimalloc stub — wraps stdlib for native test builds */
#pragma once
#include <stddef.h>
#include <stdlib.h>

typedef struct mi_heap_s { int _; } mi_heap_t;
typedef enum { mi_option_verbose } mi_option_t;

static inline void *mi_malloc(size_t n)            { return malloc(n); }
static inline void *mi_zalloc(size_t n)            { return calloc(1, n); }
static inline void  mi_free(void *p)               { free(p); }
static inline void  mi_collect(int force)          { (void)force; }
static inline void  mi_option_disable(mi_option_t o) { (void)o; }

static inline mi_heap_t *mi_heap_new(void)
{
	return (mi_heap_t *)malloc(sizeof(mi_heap_t));
}
static inline void *mi_heap_malloc(mi_heap_t *h, size_t n)
{
	(void)h;
	return malloc(n);
}
static inline void mi_heap_destroy(mi_heap_t *h) { free(h); }
