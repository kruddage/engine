/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cas_mem.h"
#include "memory_api.h"

#include <string.h>

/*
 * Open-addressing (linear-probe) hash table keyed by cas_hash_t.  cas_hash
 * never returns 0, so a slot with hash == 0 is empty; deletion uses backward-
 * shift to keep probe chains intact without tombstones.
 */

struct cas_mem_rec {
	cas_hash_t hash;    /* 0 = empty slot */
	uint32_t   size;
	uint32_t   refs;
	void      *data;
};

struct cas_mem {
	const struct memory_api *mem;
	struct cas_mem_rec      *slots;
	uint32_t                 cap;    /* power of two */
	uint32_t                 used;
};

#define CAS_MEM_INIT_CAP 16u

/* Index of h's slot if present, or its first empty slot; *found set 1/0. */
static uint32_t mem_lookup(const struct cas_mem *m, cas_hash_t h, int *found)
{
	uint32_t mask = m->cap - 1u;
	uint32_t i    = (uint32_t)(h & mask);

	for (;;) {
		if (m->slots[i].hash == 0) {
			*found = 0;
			return i;
		}
		if (m->slots[i].hash == h) {
			*found = 1;
			return i;
		}
		i = (i + 1u) & mask;
	}
}

static int32_t mem_grow(struct cas_mem *m)
{
	struct cas_mem_rec *old = m->slots;
	uint32_t            oldcap = m->cap;
	uint32_t            newcap = oldcap << 1;
	struct cas_mem_rec *ns;
	uint32_t            i;

	ns = m->mem->alloc_zero(newcap * sizeof(*ns));
	if (!ns)
		return -1;

	m->slots = ns;
	m->cap   = newcap;
	for (i = 0; i < oldcap; i++) {
		int      found;
		uint32_t j;

		if (old[i].hash == 0)
			continue;
		j = mem_lookup(m, old[i].hash, &found);   /* found == 0 always */
		m->slots[j] = old[i];
	}
	m->mem->free(old);
	return 0;
}

static int32_t mem_put(void *ctx, cas_hash_t h, const void *bytes, uint32_t size)
{
	struct cas_mem *m = ctx;
	int             found;
	uint32_t        i;
	void           *data;

	i = mem_lookup(m, h, &found);
	if (found) {
		m->slots[i].refs++;   /* dedup: identical content already stored */
		return 0;
	}

	/* Grow before insert when the table would pass a 3/4 load factor. */
	if ((m->used + 1u) * 4u > m->cap * 3u) {
		if (mem_grow(m) != 0)
			return -1;
		i = mem_lookup(m, h, &found);
	}

	data = m->mem->alloc(size ? size : 1u);   /* present blob is never NULL */
	if (!data)
		return -1;
	if (size)
		memcpy(data, bytes, size);

	m->slots[i].hash = h;
	m->slots[i].size = size;
	m->slots[i].refs = 1u;
	m->slots[i].data = data;
	m->used++;
	return 0;
}

static const void *mem_get(void *ctx, cas_hash_t h, uint32_t *out_size)
{
	struct cas_mem *m = ctx;
	int             found;
	uint32_t        i = mem_lookup(m, h, &found);

	if (!found)
		return NULL;
	if (out_size)
		*out_size = m->slots[i].size;
	return m->slots[i].data;
}

static int32_t mem_has(void *ctx, cas_hash_t h)
{
	struct cas_mem *m = ctx;
	int             found;

	(void)mem_lookup(m, h, &found);
	return found;
}

/* Backward-shift deletion: repair the probe chain left by emptying slot i. */
static void mem_backshift(struct cas_mem *m, uint32_t i)
{
	uint32_t mask = m->cap - 1u;

	for (;;) {
		uint32_t j = i;
		uint32_t k;

		m->slots[i].hash = 0;
		do {
			j = (j + 1u) & mask;
			if (m->slots[j].hash == 0)
				return;
			k = (uint32_t)(m->slots[j].hash & mask);
			/* Keep probing while slot j sits correctly in (i, j]. */
		} while ((i <= j) ? (i < k && k <= j) : (i < k || k <= j));
		m->slots[i] = m->slots[j];
		i = j;
	}
}

static void mem_drop(void *ctx, cas_hash_t h)
{
	struct cas_mem *m = ctx;
	int             found;
	uint32_t        i = mem_lookup(m, h, &found);

	if (!found)
		return;
	if (--m->slots[i].refs > 0)
		return;

	m->mem->free(m->slots[i].data);
	m->slots[i].data = NULL;
	m->slots[i].size = 0;
	m->slots[i].refs = 0;
	m->used--;
	mem_backshift(m, i);
}

static uint32_t mem_count(void *ctx)
{
	struct cas_mem *m = ctx;

	return m->used;
}

int32_t cas_mem_init(struct cas *s, const struct memory_api *mem)
{
	struct cas_mem *m;

	if (!s || !mem)
		return -1;

	m = mem->alloc_zero(sizeof(*m));
	if (!m)
		return -1;
	m->mem = mem;
	m->cap = CAS_MEM_INIT_CAP;
	m->slots = mem->alloc_zero(m->cap * sizeof(*m->slots));
	if (!m->slots) {
		mem->free(m);
		return -1;
	}

	s->mem          = mem;
	s->backing.ctx  = m;
	s->backing.put  = mem_put;
	s->backing.get  = mem_get;
	s->backing.has  = mem_has;
	s->backing.drop = mem_drop;
	s->backing.count = mem_count;
	return 0;
}

void cas_mem_shutdown(struct cas *s)
{
	struct cas_mem *m;
	uint32_t        i;

	if (!s || !s->backing.ctx)
		return;
	m = s->backing.ctx;
	for (i = 0; i < m->cap; i++) {
		if (m->slots[i].hash != 0)
			m->mem->free(m->slots[i].data);
	}
	m->mem->free(m->slots);
	m->mem->free(m);
	s->backing.ctx = NULL;
}
