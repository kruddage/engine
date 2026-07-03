/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BRANCH_H
#define BRANCH_H

#include "cas.h"

#include <stdint.h>

/*
 * Branch model over the content-addressed store (#215): named branches, an
 * active-branch (HEAD) pointer, live-save that advances the active branch
 * copy-on-write, and the switch mechanism that resolves a branch into the
 * working set the engine ingests.
 *
 * A branch is NOT a commit log — it is a named, continuously-mutated whole-
 * project state ("HEAD + my diffs off it").  Its state is a single manifest
 * hash in the store (#214); advancing it is O(changes) because unchanged
 * content is shared by hash.  This is pure logic over struct cas, native-unit-
 * testable; the runtime wiring (ingest scenes into the world, swap the catalog,
 * publish as a backend capability) layers on top.
 */

#define BRANCH_MAX      64u
#define BRANCH_NAME_MAX 48u    /* including the NUL terminator */
#define BRANCH_NONE     (-1)

struct branch_info {
	char       name[BRANCH_NAME_MAX];
	cas_hash_t manifest;   /* current working state */
	cas_hash_t base;       /* fork-base snapshot manifest; NONE for main */
};

struct branches {
	struct cas        *store;
	struct branch_info list[BRANCH_MAX];
	uint32_t           count;
	int32_t            active;   /* HEAD; BRANCH_NONE before bootstrap */
};

/* Initialize an empty branch set over an existing store. */
void branches_init(struct branches *b, struct cas *store);

/*
 * Live-save: advance the active branch's working state to `manifest`.  On an
 * empty set (no branches yet) this bootstraps `main` pointing at `manifest` and
 * makes it active — main is born from the first save, not pre-seeded.  Returns
 * the active branch index, or BRANCH_NONE on error.
 */
int32_t branches_commit(struct branches *b, cas_hash_t manifest);

/*
 * Fork a new branch named `name` whose working state starts at `manifest` and
 * whose fork base is `base` (CAS_HASH_NONE for none).  Forking from head passes
 * head's manifest; forking from a snapshot passes that snapshot's manifest as
 * both `manifest` and `base`.  Does not change HEAD.  Returns the new branch
 * index, or BRANCH_NONE on error (reserved/duplicate/empty name, or full).
 */
int32_t branches_create(struct branches *b, const char *name,
			cas_hash_t manifest, cas_hash_t base);

/* Set HEAD to `index`.  Returns 0 on success, -1 if index is invalid. */
int32_t branches_set_active(struct branches *b, int32_t index);

/* Current HEAD index (BRANCH_NONE before the first save). */
int32_t branches_active(const struct branches *b);

/* Number of branches. */
uint32_t branches_count(const struct branches *b);

/* Resolve a branch index to its info (borrowed), or NULL if out of range. */
const struct branch_info *branches_get(const struct branches *b, int32_t index);

/* Find a branch by name, or BRANCH_NONE. */
int32_t branches_find(const struct branches *b, const char *name);

/*
 * Switch mechanism: resolve a branch's working manifest into its entries — the
 * working set the engine ingests (scenes → world_ingest_scene, assets →
 * catalog).  A thin, native-testable wrapper over cas_get_manifest on the
 * branch's manifest.  Returns the entry count (<= max), or -1 on a bad index or
 * decode failure.
 */
int32_t branches_working_set(struct branches *b, int32_t index,
			     struct cas_entry *out, uint32_t max);

#endif /* BRANCH_H */
