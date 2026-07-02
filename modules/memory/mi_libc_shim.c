/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libc allocator shim (Emscripten only).
 *
 * The WASM main module is linked with -sMALLOC=none, so Emscripten ships no
 * allocator of its own.  These definitions supply the C/C++ runtime's malloc
 * family and forward each call to mimalloc — the same allocator that backs the
 * engine's memory_api.  The result is a single heap shared by everything in the
 * linear memory: the engine, Emscripten's libc, the FETCH and dynamic-linker
 * runtimes, and every side-module plugin (which imports malloc/free from the
 * main module).
 *
 * Before this, libc used emmalloc while memory_api used mimalloc: two
 * allocators partitioning one -sALLOW_MEMORY_GROWTH heap.  Any buffer that
 * crossed the boundary — or either allocator's own growth bookkeeping — could
 * corrupt the other once the heap grew.
 *
 * Native builds link the platform libc directly and never compile this file
 * (see modules/memory/CMakeLists.txt).  It lives under modules/memory/ so
 * check-no-raw-malloc.sh permits the definitions of the very symbols it bans
 * everywhere else.  Compiled with -fno-builtin so the compiler does not assume
 * the standard semantics of the functions we are replacing.
 */
#ifdef __EMSCRIPTEN__

#include <mimalloc.h>

#include <malloc.h>
#include <stdlib.h>

void *malloc(size_t size)
{
	return mi_malloc(size);
}

void free(void *ptr)
{
	mi_free(ptr);
}

void *calloc(size_t nmemb, size_t size)
{
	return mi_calloc(nmemb, size);
}

void *realloc(void *ptr, size_t size)
{
	return mi_realloc(ptr, size);
}

void *aligned_alloc(size_t alignment, size_t size)
{
	return mi_malloc_aligned(size, alignment);
}

void *memalign(size_t alignment, size_t size)
{
	return mi_malloc_aligned(size, alignment);
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	return mi_posix_memalign(memptr, alignment, size);
}

size_t malloc_usable_size(void *ptr)
{
	return mi_usable_size(ptr);
}

#endif /* __EMSCRIPTEN__ */
