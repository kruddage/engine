/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "branch_host.h"
#include "branch_api.h"
#include "memory_api.h"
#include "memory.h"

#include <assert.h>
#include <stdio.h>
#include <stddef.h>

/*
 * Gate smoke test for the branching host (#213).  It exercises the vtable
 * wiring and the debounce clock over the STUB serialize/ingest (which report
 * "nothing captured"), so it asserts the scaffold is sound without asserting
 * the end-to-end capture — Track B/C add those once the stubs are filled.
 */

static const struct memory_api test_mem = {
	mem_alloc, mem_alloc_zero, mem_free,
	mem_pool_create, mem_pool_alloc, mem_pool_free, mem_pool_destroy,
};

int main(void)
{
	const struct branch_api *br;
	int                      i;

	/* No api before init. */
	assert(branch_host_api() == NULL);

	assert(branch_host_init(NULL, &test_mem) == 0);
	br = branch_host_api();
	assert(br != NULL);

	/* A fresh host is pre-bootstrap: no branches, no HEAD, merge disabled. */
	assert(br->branch_count() == 0);
	assert(br->branch_active() == -1);
	assert(br->snapshot_count() == 0);
	assert(br->merge_supported() == 0);
	printf("PASS: host stands up empty with merge disabled\n");

	/*
	 * Dirty + tick past the debounce window.  With the serialize stub
	 * returning -1, the flush captures nothing, so no branch is bootstrapped
	 * and nothing crashes — the clock and flush path are what we're checking.
	 */
	br->mark_dirty();
	for (i = 0; i < 200; i++)
		branch_host_tick();
	assert(br->branch_count() == 0);
	printf("PASS: debounce flush is safe over the serialize stub\n");

	/* Bad handles are rejected, not dereferenced. */
	assert(br->branch_get(0, NULL) == -1);
	assert(br->branch_switch(0) == -1);
	assert(br->snapshot_restore(0) == -1);
	printf("PASS: invalid handles rejected\n");

	branch_host_shutdown();
	assert(branch_host_api() == NULL);
	printf("PASS: shutdown clears the api\n");

	printf("ALL branch_host TESTS PASSED\n");
	return 0;
}
