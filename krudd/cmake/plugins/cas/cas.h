/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CAS_H
#define CAS_H

#include <stdint.h>

/*
 * Content-addressed, copy-on-write store — the keystone storage primitive for
 * the project branching & snapshots epic (#214).  Content is stored as
 * immutable blobs keyed by a hash of their bytes: identical content hashes
 * equal and is stored once, so a whole-project state that shares content with
 * another costs only its changed blobs.  Forking and snapshotting become
 * O(changes) instead of O(project).
 *
 * This header is the substrate-agnostic core: all addressing and manifest
 * logic lives over a pluggable byte backing (struct cas_backing).  The
 * in-memory backing (cas_mem.h) is the native / unit-test implementation; the
 * IndexedDB binding supplies the same backing in the browser.  No branch or
 * snapshot semantics live here — those build on top of this store.
 */

struct memory_api;

/*
 * 64-bit FNV-1a content hash.  A fast, dependency-free non-crypto hash is
 * sufficient for content addressing at project scale (thousands of blobs); the
 * cas layer additionally verifies bytes on a hash match, so an (astronomically
 * unlikely) 64-bit collision is detected and refused, never silently conflated.
 * CAS_HASH_NONE (0) is reserved as "no hash" and is never produced by cas_hash.
 */
typedef uint64_t cas_hash_t;
#define CAS_HASH_NONE 0ull

/* Hash a byte range (FNV-1a, 64-bit).  Deterministic; never returns 0. */
cas_hash_t cas_hash(const void *bytes, uint32_t size);

/*
 * Byte backing: maps a content hash to immutable bytes with a reference count.
 * A store never holds two different byte strings under one hash — the cas layer
 * detects that collision and refuses it — so put() may assume a hash it already
 * holds refers to identical bytes and only bumps the refcount.  Reference
 * counts exist for a future GC; reclamation itself is deferred.
 */
struct cas_backing {
	void *ctx;
	/*
	 * Store size bytes under h (copying them); if h is already present,
	 * just increment its refcount.  Returns 0 on success, -1 on OOM.
	 */
	int32_t     (*put)(void *ctx, cas_hash_t h,
			   const void *bytes, uint32_t size);
	/*
	 * Borrow the bytes stored under h, or NULL if absent.  *out_size (may
	 * be NULL) receives the length.  A present zero-length blob returns a
	 * non-NULL pointer with size 0.  Valid until the entry drops to zero
	 * references.
	 */
	const void *(*get)(void *ctx, cas_hash_t h, uint32_t *out_size);
	/* Non-zero iff h is present. */
	int32_t     (*has)(void *ctx, cas_hash_t h);
	/* Drop one reference to h, freeing the bytes at zero; absent h is a no-op. */
	void        (*drop)(void *ctx, cas_hash_t h);
	/* Number of distinct blobs currently stored (for CoW/dedup assertions). */
	uint32_t    (*count)(void *ctx);
};

struct cas {
	struct cas_backing       backing;
	const struct memory_api *mem;   /* scratch for manifest (de)serialization */
};

/*
 * Store a blob.  Computes its content hash, dedups against existing content,
 * and writes the bytes at most once.  *out_hash (may be NULL) receives the
 * hash.  Returns 0 on success, -1 on OOM or a hash collision with different
 * bytes.
 */
int32_t cas_put_blob(struct cas *s, const void *bytes, uint32_t size,
		     cas_hash_t *out_hash);

/* Borrow a blob's bytes by hash, or NULL if absent.  *out_size may be NULL. */
const void *cas_get_blob(struct cas *s, cas_hash_t h, uint32_t *out_size);

/* Drop one reference to a blob (or to a manifest, which is itself a blob). */
void cas_drop_blob(struct cas *s, cas_hash_t h);

/*
 * A manifest is a whole-project state: a flat, id-keyed list of the content
 * each scene / asset / catalog entry resolves to.  A manifest is itself stored
 * as a blob, so a state is just a manifest hash and copy-on-write falls out —
 * changing one entry rewrites one content blob plus one manifest blob, and
 * everything else is shared by hash.
 */
struct cas_entry {
	uint32_t   id;    /* stable id (#187): the map key */
	uint32_t   kind;  /* ASSET_TYPE_* / caller-defined content discriminator */
	cas_hash_t hash;  /* content hash of this entry's blob */
};

/* Upper bound on entries in one manifest (the flat v1 shape). */
#define CAS_MANIFEST_MAX 4096u

/*
 * Serialize n entries into a canonical manifest blob and store it.  Entries are
 * emitted sorted by id, so two manifests with the same (id, kind, hash) set
 * hash equal regardless of caller order — equal states dedup.  *out_hash (may
 * be NULL) receives the manifest hash.  Returns 0 on success, -1 on bad args,
 * too many entries, or OOM.
 */
int32_t cas_put_manifest(struct cas *s, const struct cas_entry *ents,
			 uint32_t n, cas_hash_t *out_hash);

/*
 * Decode a manifest blob by hash into a caller-provided array (sorted by id).
 * Returns the entry count (<= max) on success, or -1 if the hash is absent, the
 * blob is not a valid manifest, or it holds more than max entries.
 */
int32_t cas_get_manifest(struct cas *s, cas_hash_t h,
			 struct cas_entry *out, uint32_t max);

#endif /* CAS_H */
