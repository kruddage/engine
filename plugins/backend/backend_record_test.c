/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "backend_record.h"
#include "backend_api.h"

#include <assert.h>
#include <stdio.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Capability state tests                                              */
/* ------------------------------------------------------------------ */

static void test_caps_default_set(void)
{
	/* Fresh link: PROJECT_PERSIST must be set */
	uint32_t caps = backend_record_get_caps();

	assert(caps & BACKEND_CAP_PROJECT_PERSIST);
	printf("PASS: caps default set\n");
}

static void test_caps_cleared_on_idb_unavailable(void)
{
	uint32_t caps;

	backend_record_mark_idb_unavailable();
	caps = backend_record_get_caps();
	assert(!(caps & BACKEND_CAP_PROJECT_PERSIST));
	printf("PASS: caps cleared after idb unavailable\n");
}

static void test_caps_auth_messaging_never_set(void)
{
	/* Local provider never asserts AUTH or MESSAGING */
	uint32_t caps = backend_record_get_caps();

	assert(!(caps & BACKEND_CAP_AUTH));
	assert(!(caps & BACKEND_CAP_MESSAGING));
	printf("PASS: AUTH/MESSAGING never set by local provider\n");
}

/* ------------------------------------------------------------------ */
/* Oversize rejection                                                  */
/* ------------------------------------------------------------------ */

/*
 * We need PROJECT_PERSIST to be set for oversize to be the rejection
 * reason rather than the caps check.  Since test_caps_cleared_on_idb_unavailable
 * runs before this, we need a helper to re-enable caps for the sake of
 * subsequent tests.  We reset via a fresh call knowing the internal
 * static starts set; we can't reset from outside, so run oversize
 * tests before the caps-clear test in a separate function ordering.
 *
 * The test binary runs all functions in the order listed in main().
 * We keep caps-clear LAST so oversize/validation tests see caps set.
 */

static void test_validate_oversize(void)
{
	/* caps are set at this point (run before mark_unavailable) */
	static const uint8_t dummy[1] = { 0 };
	int32_t rc;

	rc = backend_record_validate(1u, "test.txt", dummy,
				     BACKEND_RECORD_MAX + 1u);
	assert(rc == -1);
	printf("PASS: oversize rejected\n");

	rc = backend_record_validate(1u, "test.txt", dummy,
				     BACKEND_RECORD_MAX);
	assert(rc == 0);
	printf("PASS: exact max size accepted\n");
}

static void test_validate_zero_id(void)
{
	static const uint8_t dummy[1] = { 0 };

	assert(backend_record_validate(0u, "test.txt", dummy, 1u) == -1);
	printf("PASS: id==0 rejected\n");
}

static void test_validate_null_path(void)
{
	static const uint8_t dummy[1] = { 0 };

	assert(backend_record_validate(1u, NULL, dummy, 1u) == -1);
	printf("PASS: null path rejected\n");
}

static void test_validate_null_bytes_nonzero_size(void)
{
	assert(backend_record_validate(1u, "test.txt", NULL, 1u) == -1);
	printf("PASS: null bytes with nonzero size rejected\n");
}

static void test_validate_null_bytes_zero_size(void)
{
	/* Empty record is valid (zero-length asset). */
	assert(backend_record_validate(1u, "test.txt", NULL, 0u) == 0);
	printf("PASS: null bytes with zero size accepted\n");
}

/* ------------------------------------------------------------------ */
/* Version skip logic                                                  */
/* ------------------------------------------------------------------ */

static void test_version_known(void)
{
	assert(backend_record_check_version(BACKEND_RECORD_VERSION) == 0);
	printf("PASS: known version accepted\n");
}

static void test_version_unknown(void)
{
	assert(backend_record_check_version(0u) == -1);
	assert(backend_record_check_version(BACKEND_RECORD_VERSION + 1u) == -1);
	assert(backend_record_check_version(0xFFFFFFFFu) == -1);
	printf("PASS: unknown versions skipped\n");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
	/*
	 * Order matters: caps-clear test must come after validation tests
	 * that require BACKEND_CAP_PROJECT_PERSIST to be set.
	 */
	test_caps_default_set();
	test_validate_oversize();
	test_validate_zero_id();
	test_validate_null_path();
	test_validate_null_bytes_nonzero_size();
	test_validate_null_bytes_zero_size();
	test_version_known();
	test_version_unknown();

	/* Clear caps — must be last as it has global side-effect */
	test_caps_cleared_on_idb_unavailable();
	test_caps_auth_messaging_never_set();

	printf("All backend_record tests passed\n");
	return 0;
}
