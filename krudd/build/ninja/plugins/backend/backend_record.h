/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BACKEND_RECORD_H
#define BACKEND_RECORD_H

#include <stdint.h>

/*
 * Maximum size in bytes for a single persisted asset record.
 * persist_asset() rejects any payload larger than this value and logs
 * a warning.  8 MiB is generous for current asset types (text, small
 * meshes) and matches a reasonable IndexedDB write budget.
 */
#define BACKEND_RECORD_MAX	(8u * 1024u * 1024u)

/*
 * Current on-storage record format version.  Records loaded from IDB
 * with a different version field are skipped and logged as unknown.
 */
#define BACKEND_RECORD_VERSION	1u

/*
 * caps_state — tracks whether IndexedDB is available at runtime.
 *
 * Call backend_record_mark_idb_unavailable() when the IDB open fails
 * (e.g. private-browsing mode) to clear BACKEND_CAP_PROJECT_PERSIST
 * from the live bitmask returned by backend_record_get_caps().
 */
uint32_t backend_record_get_caps(void);
void     backend_record_mark_idb_unavailable(void);

/*
 * Validate a persist_asset() call before attempting any IDB write.
 * Returns 0 if the call is acceptable, -1 if it must be rejected:
 *   - caps do not include BACKEND_CAP_PROJECT_PERSIST
 *   - id == 0 (reserved sentinel)
 *   - path is NULL
 *   - bytes is NULL and size > 0
 *   - size exceeds BACKEND_RECORD_MAX
 */
int32_t backend_record_validate(uint32_t id, const char *path,
				const void *bytes, uint32_t size);

/*
 * Validate a stored record's version field on rehydration.
 * Returns 0 if version is recognised, -1 if it should be skipped.
 */
int32_t backend_record_check_version(uint32_t version);

#endif /* BACKEND_RECORD_H */
