/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BRANCH_HOST_H
#define BRANCH_HOST_H

#include "branch_api.h"

#include <stdint.h>

struct subsystem_manager;
struct memory_api;

/*
 * Branching host (#213): the runtime that owns the content-addressed store
 * (#214), the branch set (#215), and the snapshot timelines (#216), and
 * publishes them as struct branch_api.  It lives inside the backend plugin and
 * reaches the world / catalog through subsystem apis (the "scene" entity_api
 * and the "asset"/"asset_mut" apis) looked up on `mgr` — no direct linkage to
 * other side modules.
 *
 * v1 backs the store with the in-memory backing (cas_mem) on every target; the
 * IndexedDB backing (cas_idb) swaps in under __EMSCRIPTEN__ once it lands, at
 * which point branches and snapshots become durable across reloads.
 */

/*
 * Stand up the host over a fresh in-memory store.  `mgr` is retained for
 * capture/ingest; `mem` backs the store's blob table and manifest scratch.
 * Returns 0 on success, -1 on bad args or OOM.  Idempotent-safe: a second call
 * without shutdown is a no-op returning 0.
 */
int32_t branch_host_init(struct subsystem_manager *mgr,
			 const struct memory_api *mem);

/* Tear down the store and clear all branches/snapshots. */
void    branch_host_shutdown(void);

/* The branching vtable — valid only between init and shutdown, else NULL. */
const struct branch_api *branch_host_api(void);

/*
 * Advance the debounce clock one frame.  When a mark_dirty is outstanding and
 * the project has been quiescent for the debounce window, this flushes: it
 * captures the project (branch_serialize_capture), advances the active branch
 * copy-on-write (branches_commit — bootstrapping `main` on the very first
 * flush), and records an auto-snapshot.  Safe to call every frame; cheap when
 * nothing is dirty.  Driven from the backend subsystem tick (one call/frame).
 */
void    branch_host_tick(void);

#endif /* BRANCH_HOST_H */
