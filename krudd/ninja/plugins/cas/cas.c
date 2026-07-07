/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cas.h"
#include "memory_api.h"

#include <string.h>

/* Canonical manifest layout: 12-byte header + 16-byte little-endian entries. */
#define MANIFEST_HDR 12u
#define MANIFEST_ENT 16u

cas_hash_t cas_hash(const void *bytes, uint32_t size)
{
	const uint8_t *p = bytes;
	uint64_t       h = 14695981039346656037ull;   /* FNV-1a offset basis */
	uint32_t       i;

	for (i = 0; i < size; i++) {
		h ^= p[i];
		h *= 1099511628211ull;                 /* FNV-1a prime */
	}
	return h ? h : 1ull;   /* never alias CAS_HASH_NONE */
}

int32_t cas_put_blob(struct cas *s, const void *bytes, uint32_t size,
		     cas_hash_t *out_hash)
{
	cas_hash_t h;

	if (!s || (size && !bytes))
		return -1;

	h = cas_hash(bytes, size);

	/* On a hash hit, verify bytes: a genuine collision must be refused, an
	 * identical blob deduped. */
	if (s->backing.has(s->backing.ctx, h)) {
		uint32_t    esize = 0;
		const void *ex = s->backing.get(s->backing.ctx, h, &esize);

		if (!ex || esize != size
		    || (size && memcmp(ex, bytes, size) != 0))
			return -1;
	}

	if (s->backing.put(s->backing.ctx, h, bytes, size) != 0)
		return -1;
	if (out_hash)
		*out_hash = h;
	return 0;
}

const void *cas_get_blob(struct cas *s, cas_hash_t h, uint32_t *out_size)
{
	if (!s)
		return NULL;
	return s->backing.get(s->backing.ctx, h, out_size);
}

void cas_drop_blob(struct cas *s, cas_hash_t h)
{
	if (s)
		s->backing.drop(s->backing.ctx, h);
}

/* ------------------------------------------------------------------ */
/* Manifest serialization — canonical little-endian, order-independent */
/* ------------------------------------------------------------------ */

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void put_u64(uint8_t *p, uint64_t v)
{
	uint32_t i;

	for (i = 0; i < 8u; i++)
		p[i] = (uint8_t)(v >> (8u * i));
}

static uint32_t get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
	     | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p)
{
	uint64_t v = 0;
	uint32_t i;

	for (i = 0; i < 8u; i++)
		v |= (uint64_t)p[i] << (8u * i);
	return v;
}

/* Insertion sort by id — canonicalizes entry order so equal states dedup. */
static void manifest_sort(struct cas_entry *a, uint32_t n)
{
	uint32_t i, j;

	for (i = 1; i < n; i++) {
		struct cas_entry key = a[i];

		j = i;
		while (j > 0 && a[j - 1].id > key.id) {
			a[j] = a[j - 1];
			j--;
		}
		a[j] = key;
	}
}

int32_t cas_put_manifest(struct cas *s, const struct cas_entry *ents,
			 uint32_t n, cas_hash_t *out_hash)
{
	struct cas_entry *sorted = NULL;
	uint8_t          *buf;
	uint32_t          i, off;
	size_t            blob;
	int32_t           rc;

	if (!s || (n && !ents) || n > CAS_MANIFEST_MAX)
		return -1;

	if (n) {
		sorted = s->mem->alloc(n * sizeof(*sorted));
		if (!sorted)
			return -1;
		memcpy(sorted, ents, n * sizeof(*sorted));
		manifest_sort(sorted, n);
	}

	blob = MANIFEST_HDR + (size_t)n * MANIFEST_ENT;
	buf  = s->mem->alloc(blob);
	if (!buf) {
		if (sorted)
			s->mem->free(sorted);
		return -1;
	}

	memcpy(buf, "KMFT", 4);
	put_u32(buf + 4, 1u);     /* version */
	put_u32(buf + 8, n);
	off = MANIFEST_HDR;
	for (i = 0; i < n; i++) {
		put_u32(buf + off + 0, sorted[i].id);
		put_u32(buf + off + 4, sorted[i].kind);
		put_u64(buf + off + 8, sorted[i].hash);
		off += MANIFEST_ENT;
	}

	rc = cas_put_blob(s, buf, (uint32_t)blob, out_hash);

	s->mem->free(buf);
	if (sorted)
		s->mem->free(sorted);
	return rc;
}

int32_t cas_get_manifest(struct cas *s, cas_hash_t h,
			 struct cas_entry *out, uint32_t max)
{
	const uint8_t *b;
	uint32_t       size = 0, n, i, off;

	if (!s || !out)
		return -1;

	b = cas_get_blob(s, h, &size);
	if (!b || size < MANIFEST_HDR)
		return -1;
	if (memcmp(b, "KMFT", 4) != 0 || get_u32(b + 4) != 1u)
		return -1;

	n = get_u32(b + 8);
	if (n > CAS_MANIFEST_MAX || n > max)
		return -1;
	if ((size_t)size != MANIFEST_HDR + (size_t)n * MANIFEST_ENT)
		return -1;

	off = MANIFEST_HDR;
	for (i = 0; i < n; i++) {
		out[i].id   = get_u32(b + off + 0);
		out[i].kind = get_u32(b + off + 4);
		out[i].hash = get_u64(b + off + 8);
		off += MANIFEST_ENT;
	}
	return (int32_t)n;
}
