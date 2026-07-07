/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CAS_IDB_H
#define CAS_IDB_H

#include "cas.h"

struct memory_api;

/*
 * IndexedDB-backed content backing for struct cas (#214) — the browser
 * binding of struct cas_backing, standing in for cas_mem.c (cas_mem.h) once
 * the store runs in the browser (plugins/backend/build.scm swaps it in
 * under EMSCRIPTEN; native keeps cas_mem).
 *
 * struct cas_backing's contract is SYNCHRONOUS (put/get/has/drop/count all
 * return immediately) but IndexedDB is asynchronous, so this binding keeps
 * an in-memory open-addressing table — structurally the same table cas_mem
 * uses — as the one and only SYNCHRONOUS SOURCE OF TRUTH.  Every backing
 * call reads or writes that table immediately, exactly as cas_mem does.
 * Durability is layered on top, not woven into the synchronous path:
 *
 *   - put(): inserts into the table first (so the caller's very next get()
 *     already sees it), then — on a genuinely new entry, not a dedup hit —
 *     fires an async krudd_idb_blob_put() that writes through to
 *     IndexedDB in the background.  put() does not wait for it.
 *   - drop(): removes from the table exactly as cas_mem does and, only once
 *     the refcount reaches zero, fires an async krudd_idb_blob_del().
 *   - cas_idb_init(): wires the table (starting EMPTY) and kicks off an
 *     async IndexedDB cursor over every previously-persisted blob; it does
 *     NOT block waiting for it.  cas_idb_poll() must be called
 *     periodically afterward (branch_host_tick() does this under
 *     __EMSCRIPTEN__) to drain the loaded records into the table as they
 *     stream back from the cursor.
 *
 * Eventual-consistency model, spelled out for callers:
 *
 *   - A blob that put() just returned 0 for may not yet be durable — the
 *     write-behind to IndexedDB can still be in flight, and a tab closed in
 *     that window can lose it (same contract as the existing project-asset
 *     persistence bridge in backend_plugin.c).
 *   - A blob written in a PRIOR session is invisible to get()/has() for a
 *     few frames after cas_idb_init(), until cas_idb_poll() finishes
 *     draining the load cursor — the table starts empty, not populated.
 *   - Refcounts do NOT survive a reload: every blob streamed in by the load
 *     cursor is inserted with refs=1, regardless of how many manifests
 *     referenced it in the prior session, because reconstructing the true
 *     refcount would mean walking every branch/snapshot manifest at load
 *     time. This is safe today only because nothing in the branch/
 *     snapshot/backend code calls cas_drop_blob() yet (cas.h: reclamation
 *     is deferred to a future GC) — it becomes load-bearing the day
 *     something does, at which point the refcount needs to be rebuilt from
 *     the live branch/snapshot manifests at load time rather than trusted
 *     from IndexedDB.
 *
 * cas_idb_init mirrors cas_mem_init's signature and contract exactly:
 * returns 0 on success, -1 on bad args or OOM.  cas_idb_shutdown frees the
 * in-memory table only — it deliberately does NOT delete the IndexedDB
 * database, since durability across reloads is the entire point.
 */
int32_t cas_idb_init(struct cas *s, const struct memory_api *mem);
void    cas_idb_shutdown(struct cas *s);

/*
 * Drain the browser-side load queue (staged by the async IndexedDB cursor
 * that cas_idb_init() kicks off) into the synchronous table.  Safe and
 * cheap to call every frame: a no-op before the cursor resolves and after
 * the one-time load has fully drained.  Call this once per frame while the
 * store's backing is cas_idb (branch_host_tick() does so under
 * __EMSCRIPTEN__).
 */
void    cas_idb_poll(struct cas *s);

#endif /* CAS_IDB_H */
