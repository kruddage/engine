/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cas_idb.h"
#include "memory_api.h"

#include <string.h>

#ifndef __EMSCRIPTEN__
#error "cas_idb.c is the browser-only cas backing; native builds use cas_mem.c"
#endif

/*
 * IndexedDB blob bridge.  These EM_JS functions are defined in the main
 * module (modules/core/plugin_abi.c) — a side module's own EM_JS becomes a
 * throwing stub at load time, so backend_plugin.wasm (which this file
 * compiles into under EMSCRIPTEN; see plugins/backend/build.scm)
 * imports them instead of defining them.  See plugin_abi.c's IndexedDB
 * blob-store comment for the wire schema and cas_idb.h for the
 * eventual-consistency model these calls sit inside.
 */
extern void krudd_idb_blob_open(void);
extern int  krudd_idb_blob_state(void);
extern int  krudd_idb_blob_peek(char *hash_out, uint32_t hash_cap,
				uint32_t *size_out);
extern void krudd_idb_blob_pop(uint8_t *dst);
extern void krudd_idb_blob_put(const char *hash_ptr, const void *data_ptr,
			       uint32_t size);
extern void krudd_idb_blob_del(const char *hash_ptr);

/*
 * Open-addressing (linear-probe) table — the SYNCHRONOUS source of truth,
 * structurally the same table cas_mem.c uses: cas_hash_t never returns 0, so
 * a slot with hash == 0 is empty, and deletion backward-shifts to keep probe
 * chains intact without tombstones.  IndexedDB durability is layered on top
 * of this table, not woven into it — see cas_idb.h.
 */

struct cas_idb_rec {
	cas_hash_t hash;    /* 0 = empty slot */
	uint32_t   size;
	uint32_t   refs;
	void      *data;
};

struct cas_idb {
	const struct memory_api *mem;
	struct cas_idb_rec      *slots;
	uint32_t                 cap;      /* power of two */
	uint32_t                 used;
	int32_t                  loaded;   /* 1 once the IDB load has drained */
};

#define CAS_IDB_INIT_CAP 16u
#define CAS_IDB_HASH_LEN 16u   /* 64-bit hash as lowercase hex, sans NUL */

static const char cas_idb_hex[] = "0123456789abcdef";

/* Render h as CAS_IDB_HASH_LEN lowercase hex digits + NUL into out. */
static void hash_to_hex(cas_hash_t h, char *out)
{
	uint32_t i;

	for (i = 0; i < CAS_IDB_HASH_LEN; i++) {
		out[CAS_IDB_HASH_LEN - 1u - i] = cas_idb_hex[h & 0xfull];
		h >>= 4;
	}
	out[CAS_IDB_HASH_LEN] = '\0';
}

/* Parse a hex string produced by hash_to_hex back into a hash. */
static cas_hash_t hex_to_hash(const char *in)
{
	cas_hash_t h = 0;
	uint32_t   i;

	for (i = 0; i < CAS_IDB_HASH_LEN && in[i]; i++) {
		char c = in[i];
		uint32_t v;

		if (c >= '0' && c <= '9')
			v = (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v = (uint32_t)(c - 'a') + 10u;
		else
			break;
		h = (h << 4) | v;
	}
	return h;
}

/* Index of h's slot if present, or its first empty slot; *found set 1/0. */
static uint32_t idb_lookup(const struct cas_idb *m, cas_hash_t h, int *found)
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

static int32_t idb_grow(struct cas_idb *m)
{
	struct cas_idb_rec *old    = m->slots;
	uint32_t             oldcap = m->cap;
	uint32_t             newcap = oldcap << 1;
	struct cas_idb_rec  *ns;
	uint32_t             i;

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
		j = idb_lookup(m, old[i].hash, &found);   /* found == 0 always */
		m->slots[j] = old[i];
	}
	m->mem->free(old);
	return 0;
}

/*
 * Insert bytes already known-absent under h, taking ownership of `data`
 * verbatim (no copy).  Used only by cas_idb_poll() to fold a record loaded
 * from IndexedDB into the table — unlike idb_put(), this never calls back
 * into krudd_idb_blob_put(), since the record is already persisted.
 */
static int32_t idb_table_absorb(struct cas_idb *m, cas_hash_t h, void *data,
				uint32_t size)
{
	int      found;
	uint32_t i;

	i = idb_lookup(m, h, &found);
	if (found) {
		/* Raced with a put() this session for the same content: keep
		 * the live entry, the loaded bytes are redundant. */
		m->slots[i].refs++;
		if (data)
			m->mem->free(data);
		return 0;
	}

	if ((m->used + 1u) * 4u > m->cap * 3u) {
		if (idb_grow(m) != 0)
			return -1;
		i = idb_lookup(m, h, &found);
	}

	m->slots[i].hash = h;
	m->slots[i].size = size;
	m->slots[i].refs = 1u;
	m->slots[i].data = data;
	m->used++;
	return 0;
}

static int32_t idb_put(void *ctx, cas_hash_t h, const void *bytes,
		       uint32_t size)
{
	struct cas_idb *m = ctx;
	int              found;
	uint32_t         i;
	void            *data;
	char             hex[CAS_IDB_HASH_LEN + 1u];

	i = idb_lookup(m, h, &found);
	if (found) {
		m->slots[i].refs++;   /* dedup: identical content already stored */
		return 0;
	}

	if ((m->used + 1u) * 4u > m->cap * 3u) {
		if (idb_grow(m) != 0)
			return -1;
		i = idb_lookup(m, h, &found);
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

	/* Write-behind: the table above is already the visible truth, so
	 * put() returns without waiting on this async persist. */
	hash_to_hex(h, hex);
	krudd_idb_blob_put(hex, data, size);
	return 0;
}

static const void *idb_get(void *ctx, cas_hash_t h, uint32_t *out_size)
{
	struct cas_idb *m = ctx;
	int              found;
	uint32_t         i = idb_lookup(m, h, &found);

	if (!found)
		return NULL;
	if (out_size)
		*out_size = m->slots[i].size;
	return m->slots[i].data;
}

static int32_t idb_has(void *ctx, cas_hash_t h)
{
	struct cas_idb *m = ctx;
	int              found;

	(void)idb_lookup(m, h, &found);
	return found;
}

/* Backward-shift deletion: repair the probe chain left by emptying slot i. */
static void idb_backshift(struct cas_idb *m, uint32_t i)
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

static void idb_drop(void *ctx, cas_hash_t h)
{
	struct cas_idb *m = ctx;
	int              found;
	uint32_t         i = idb_lookup(m, h, &found);
	char             hex[CAS_IDB_HASH_LEN + 1u];

	if (!found)
		return;
	if (--m->slots[i].refs > 0)
		return;

	m->mem->free(m->slots[i].data);
	m->slots[i].data = NULL;
	m->slots[i].size = 0;
	m->slots[i].refs = 0;
	m->used--;
	idb_backshift(m, i);

	/* Write-behind: drop the persisted copy now that no in-memory
	 * reference remains. */
	hash_to_hex(h, hex);
	krudd_idb_blob_del(hex);
}

static uint32_t idb_count(void *ctx)
{
	struct cas_idb *m = ctx;

	return m->used;
}

int32_t cas_idb_init(struct cas *s, const struct memory_api *mem)
{
	struct cas_idb *m;

	if (!s || !mem)
		return -1;

	m = mem->alloc_zero(sizeof(*m));
	if (!m)
		return -1;
	m->mem = mem;
	m->cap = CAS_IDB_INIT_CAP;
	m->slots = mem->alloc_zero(m->cap * sizeof(*m->slots));
	if (!m->slots) {
		mem->free(m);
		return -1;
	}

	s->mem           = mem;
	s->backing.ctx   = m;
	s->backing.put   = idb_put;
	s->backing.get   = idb_get;
	s->backing.has   = idb_has;
	s->backing.drop  = idb_drop;
	s->backing.count = idb_count;

	/*
	 * Kick off the async load; the table starts EMPTY and cas_idb_poll()
	 * drains previously-persisted blobs into it over the frames that
	 * follow (see cas_idb.h for the eventual-consistency model).  This is
	 * safe for branch_host's v1 usage: a fresh session bootstraps `main`
	 * from a new capture on the first save rather than reading an old
	 * manifest, so nothing needs a loaded blob to be present immediately.
	 */
	krudd_idb_blob_open();
	return 0;
}

void cas_idb_shutdown(struct cas *s)
{
	struct cas_idb *m;
	uint32_t         i;

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

void cas_idb_poll(struct cas *s)
{
	struct cas_idb *m;
	int              state;
	int              len;
	char             hex[CAS_IDB_HASH_LEN + 1u];
	uint32_t         size = 0;

	if (!s || !s->backing.ctx)
		return;
	m = s->backing.ctx;
	if (m->loaded)
		return;

	state = krudd_idb_blob_state();
	if (state == 0)
		return;   /* cursor walk still in flight */

	if (state == 1) {
		while ((len = krudd_idb_blob_peek(hex, sizeof(hex),
						  &size)) >= 0) {
			cas_hash_t h = hex_to_hash(hex);
			void      *buf = NULL;

			if (len > 0) {
				buf = m->mem->alloc((size_t)len);
				if (!buf) {
					/* OOM: drop this record rather than
					 * spin retrying it forever. */
					krudd_idb_blob_pop(NULL);
					continue;
				}
			}
			krudd_idb_blob_pop(buf);
			(void)idb_table_absorb(m, h, buf, (uint32_t)len);
		}
	}
	/* state == 2 (unavailable): nothing to load; the table stays empty
	 * and put()/drop() keep working purely in memory for this session. */
	m->loaded = 1;
}
