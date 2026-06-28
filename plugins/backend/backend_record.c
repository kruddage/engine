/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "backend_record.h"
#include "backend_api.h"

#include <stddef.h>

/*
 * Live capability bitmask.  Starts with PROJECT_PERSIST set; cleared
 * if IndexedDB is unavailable (private-browsing, security error, etc.).
 */
static uint32_t g_caps = BACKEND_CAP_PROJECT_PERSIST;

uint32_t backend_record_get_caps(void)
{
	return g_caps;
}

void backend_record_mark_idb_unavailable(void)
{
	g_caps &= ~(uint32_t)BACKEND_CAP_PROJECT_PERSIST;
}

int32_t backend_record_validate(uint32_t id, const char *path,
				const void *bytes, uint32_t size)
{
	if (!(g_caps & BACKEND_CAP_PROJECT_PERSIST))
		return -1;
	if (id == 0)
		return -1;
	if (!path)
		return -1;
	if (!bytes && size > 0)
		return -1;
	if (size > BACKEND_RECORD_MAX)
		return -1;
	return 0;
}

int32_t backend_record_check_version(uint32_t version)
{
	return (version == BACKEND_RECORD_VERSION) ? 0 : -1;
}
