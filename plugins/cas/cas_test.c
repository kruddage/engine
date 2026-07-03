/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cas.h"
#include "cas_mem.h"
#include "memory_api.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *t_alloc_zero(size_t n)
{
	return calloc(1, n);
}

static const struct memory_api test_mem = {
	malloc, t_alloc_zero, free, NULL, NULL, NULL, NULL,
};

/* Hash is deterministic, content-sensitive, and never 0. */
static void test_hash(void)
{
	assert(cas_hash("hello", 5) == cas_hash("hello", 5));
	assert(cas_hash("hello", 5) != cas_hash("world", 5));
	assert(cas_hash("", 0) != CAS_HASH_NONE);
	printf("PASS: hash deterministic + nonzero\n");
}

/* Identical content stores once; the blob reads back byte-for-byte. */
static void test_put_get_dedup(void)
{
	struct cas s;
	cas_hash_t h1 = 0, h2 = 0;
	const void *got;
	uint32_t    n = 0;

	assert(cas_mem_init(&s, &test_mem) == 0);

	assert(cas_put_blob(&s, "abc", 3, &h1) == 0);
	assert(cas_put_blob(&s, "abc", 3, &h2) == 0);   /* dedup */
	assert(h1 == h2);
	assert(s.backing.count(s.backing.ctx) == 1u);   /* stored once */

	got = cas_get_blob(&s, h1, &n);
	assert(got && n == 3 && memcmp(got, "abc", 3) == 0);
	assert(cas_get_blob(&s, cas_hash("nope", 4), NULL) == NULL);

	/* Two dedup puts took two refs: one drop keeps it, the second frees it. */
	cas_drop_blob(&s, h1);
	assert(cas_get_blob(&s, h1, NULL) != NULL);
	cas_drop_blob(&s, h1);
	assert(cas_get_blob(&s, h1, NULL) == NULL);
	assert(s.backing.count(s.backing.ctx) == 0u);

	cas_mem_shutdown(&s);
	printf("PASS: put/get/dedup + refcount\n");
}

/* An empty blob is a present, non-NULL, zero-length entry. */
static void test_empty_blob(void)
{
	struct cas s;
	cas_hash_t h = 0;
	const void *got;
	uint32_t    n = 7;

	assert(cas_mem_init(&s, &test_mem) == 0);
	assert(cas_put_blob(&s, NULL, 0, &h) == 0);
	got = cas_get_blob(&s, h, &n);
	assert(got != NULL && n == 0u);
	cas_mem_shutdown(&s);
	printf("PASS: empty blob present with size 0\n");
}

/*
 * Copy-on-write: two whole-project states differing by one asset share every
 * other blob.  Going from state A to state B costs exactly one content blob
 * plus one manifest blob.
 */
static void test_cow(void)
{
	struct cas       s;
	struct cas_entry a[3], b[3];
	cas_hash_t       ha = 0, hb = 0;
	uint32_t         before, after;

	assert(cas_mem_init(&s, &test_mem) == 0);

	/* State A: three assets with distinct content. */
	a[0].id = 1; a[0].kind = 7;
	assert(cas_put_blob(&s, "aaaa", 4, &a[0].hash) == 0);
	a[1].id = 2; a[1].kind = 7;
	assert(cas_put_blob(&s, "bbbb", 4, &a[1].hash) == 0);
	a[2].id = 3; a[2].kind = 7;
	assert(cas_put_blob(&s, "cccc", 4, &a[2].hash) == 0);
	assert(cas_put_manifest(&s, a, 3, &ha) == 0);

	before = s.backing.count(s.backing.ctx);   /* 3 blobs + 1 manifest = 4 */
	assert(before == 4u);

	/* State B: edit asset id 2 only; ids 1 and 3 keep their content. */
	b[0] = a[0];
	b[2] = a[2];
	b[1].id = 2; b[1].kind = 7;
	assert(cas_put_blob(&s, "BBBB", 4, &b[1].hash) == 0);   /* one new blob */
	assert(cas_put_manifest(&s, b, 3, &hb) == 0);           /* one new manifest */

	after = s.backing.count(s.backing.ctx);
	assert(hb != ha);
	assert(after - before == 2u);   /* CoW: only the change, not the project */

	/* Unchanged content is genuinely shared, not re-stored. */
	assert(b[0].hash == a[0].hash && b[2].hash == a[2].hash);

	cas_mem_shutdown(&s);
	printf("PASS: copy-on-write (delta = 1 blob + 1 manifest)\n");
}

/* Manifest encoding is canonical: entry order does not affect the hash. */
static void test_manifest_canonical(void)
{
	struct cas       s;
	struct cas_entry fwd[3], rev[3];
	cas_hash_t       h1 = 0, h2 = 0;
	uint32_t         count_after_first;

	assert(cas_mem_init(&s, &test_mem) == 0);

	fwd[0].id = 5; fwd[0].kind = 1; fwd[0].hash = 0xAAAAull;
	fwd[1].id = 1; fwd[1].kind = 2; fwd[1].hash = 0xBBBBull;
	fwd[2].id = 3; fwd[2].kind = 3; fwd[2].hash = 0xCCCCull;
	rev[0] = fwd[2];
	rev[1] = fwd[1];
	rev[2] = fwd[0];

	assert(cas_put_manifest(&s, fwd, 3, &h1) == 0);
	count_after_first = s.backing.count(s.backing.ctx);
	assert(cas_put_manifest(&s, rev, 3, &h2) == 0);

	assert(h1 == h2);   /* order-independent */
	/* Same canonical bytes → deduped, no second manifest blob. */
	assert(s.backing.count(s.backing.ctx) == count_after_first);

	cas_mem_shutdown(&s);
	printf("PASS: manifest canonical (order-independent + dedup)\n");
}

/* A stored manifest decodes back to its entries, sorted by id. */
static void test_manifest_roundtrip(void)
{
	struct cas       s;
	struct cas_entry in[3], out[8];
	cas_hash_t       h = 0;
	int32_t          n;

	assert(cas_mem_init(&s, &test_mem) == 0);

	in[0].id = 5; in[0].kind = 10; in[0].hash = 0x111ull;
	in[1].id = 1; in[1].kind = 20; in[1].hash = 0x222ull;
	in[2].id = 3; in[2].kind = 30; in[2].hash = 0x333ull;
	assert(cas_put_manifest(&s, in, 3, &h) == 0);

	n = cas_get_manifest(&s, h, out, 8);
	assert(n == 3);
	assert(out[0].id == 1 && out[0].kind == 20 && out[0].hash == 0x222ull);
	assert(out[1].id == 3 && out[1].kind == 30 && out[1].hash == 0x333ull);
	assert(out[2].id == 5 && out[2].kind == 10 && out[2].hash == 0x111ull);

	/* Too small an output array is rejected, not truncated. */
	assert(cas_get_manifest(&s, h, out, 2) == -1);
	/* An unknown hash is a miss. */
	assert(cas_get_manifest(&s, 0xDEADull, out, 8) == -1);

	/* Empty manifest round-trips to zero entries. */
	assert(cas_put_manifest(&s, NULL, 0, &h) == 0);
	assert(cas_get_manifest(&s, h, out, 8) == 0);

	cas_mem_shutdown(&s);
	printf("PASS: manifest round-trip (sorted)\n");
}

/* The table grows past its initial capacity and keeps every blob resolvable. */
static void test_grow(void)
{
	struct cas s;
	cas_hash_t hashes[64];
	char       buf[16];
	uint32_t   i, sz;
	const void *got;

	assert(cas_mem_init(&s, &test_mem) == 0);
	for (i = 0; i < 64u; i++) {
		int len = snprintf(buf, sizeof(buf), "blob-%u", i);

		assert(cas_put_blob(&s, buf, (uint32_t)len, &hashes[i]) == 0);
	}
	assert(s.backing.count(s.backing.ctx) == 64u);

	for (i = 0; i < 64u; i++) {
		int len = snprintf(buf, sizeof(buf), "blob-%u", i);

		got = cas_get_blob(&s, hashes[i], &sz);
		assert(got && sz == (uint32_t)len && memcmp(got, buf, sz) == 0);
	}

	/* Drop half, then confirm the survivors still resolve (probe chains ok). */
	for (i = 0; i < 64u; i += 2u)
		cas_drop_blob(&s, hashes[i]);
	assert(s.backing.count(s.backing.ctx) == 32u);
	for (i = 1; i < 64u; i += 2u)
		assert(cas_get_blob(&s, hashes[i], NULL) != NULL);

	cas_mem_shutdown(&s);
	printf("PASS: grow + backshift deletion\n");
}

/* A hash hit with different bytes is a collision and must be refused. */
static int32_t stub_put(void *c, cas_hash_t h, const void *b, uint32_t n)
{
	(void)c; (void)h; (void)b; (void)n;
	return 0;
}
static const void *stub_get(void *c, cas_hash_t h, uint32_t *n)
{
	static const char other[3] = { 'x', 'y', 'z' };

	(void)c; (void)h;
	if (n)
		*n = 3u;
	return other;
}
static int32_t stub_has(void *c, cas_hash_t h)
{
	(void)c; (void)h;
	return 1;   /* pretend every hash is already present */
}
static void stub_drop(void *c, cas_hash_t h) { (void)c; (void)h; }
static uint32_t stub_count(void *c) { (void)c; return 0u; }

static void test_collision_refused(void)
{
	struct cas s;

	memset(&s, 0, sizeof(s));
	s.mem          = &test_mem;
	s.backing.put  = stub_put;
	s.backing.get  = stub_get;   /* returns "xyz" for any hash */
	s.backing.has  = stub_has;   /* claims every hash present */
	s.backing.drop = stub_drop;
	s.backing.count = stub_count;

	/* "abc" hashes to some h the stub claims to hold as "xyz" → refuse. */
	assert(cas_put_blob(&s, "abc", 3, NULL) == -1);
	printf("PASS: collision with different bytes refused\n");
}

int main(void)
{
	test_hash();
	test_put_get_dedup();
	test_empty_blob();
	test_cow();
	test_manifest_canonical();
	test_manifest_roundtrip();
	test_grow();
	test_collision_refused();

	printf("cas tests passed\n");
	return 0;
}
