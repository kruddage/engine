/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BACKEND_API_H
#define BACKEND_API_H

#include <stdint.h>

/*
 * Capability flags.  get_caps() returns a live OR-set of these.
 * The Local provider sets BACKEND_CAP_PROJECT_PERSIST at startup and
 * clears it if IndexedDB is unavailable (private-browsing, open error).
 * AUTH and MESSAGING are reserved for a future Remote provider.
 */
#define BACKEND_CAP_PROJECT_PERSIST	(1u << 0)
#define BACKEND_CAP_AUTH		(1u << 1)
#define BACKEND_CAP_MESSAGING		(1u << 2)
/*
 * Project branching & snapshots (#213).  Set by the Local provider once the
 * branching host is up; a provider without branching leaves it clear and
 * branching() returns NULL, so consumers degrade safely.
 */
#define BACKEND_CAP_BRANCHING		(1u << 3)

struct branch_api;

/*
 * Backend subsystem API.  Obtain via
 * subsystem_manager_get_api(mgr, "backend").
 *
 * The subsystem is registered async; the api pointer is populated
 * before async_init is called, so get_api() returns a valid pointer
 * immediately — but persist_asset / delete_asset should only be called
 * after the system is ready (once IndexedDB open completes).  The
 * editor (#190) drives this via subsystem_manager_on_ready().
 */
struct backend_api {
	/*
	 * Return the live capability bitmask.  BACKEND_CAP_PROJECT_PERSIST
	 * is cleared at runtime if IndexedDB is unavailable.
	 */
	uint32_t (*get_caps)(void);

	/*
	 * Insert-or-update a persisted record for an authored asset.
	 * id must be non-zero.  size must be <= BACKEND_RECORD_MAX.
	 * Returns 0 on success, -1 on failure (no capability, oversize,
	 * null args, or IndexedDB error).
	 */
	int32_t (*persist_asset)(uint32_t id, const char *path,
				 int32_t type, const void *bytes,
				 uint32_t size);

	/*
	 * Remove a persisted record by id.
	 * Returns 0 on success, -1 on failure (no capability or bad id).
	 */
	int32_t (*delete_asset)(uint32_t id);

	/*
	 * The branching capability (#213), or NULL when BACKEND_CAP_BRANCHING is
	 * clear.  Gives the editor and the live-save wiring branches, snapshots,
	 * switch, and fork without knowing IndexedDB or the cas store.  See
	 * branch_api.h.
	 */
	const struct branch_api *(*branching)(void);
};

#endif /* BACKEND_API_H */
