/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FETCH_API_H
#define FETCH_API_H

/*
 * Async HTTP fetch API for plugins.
 *
 * Registered by the main module as the "fetch" subsystem; retrieve via
 *   subsystem_manager_get_api(mgr, "fetch")
 *
 * Wraps Emscripten's fetch API so plugins do not call emscripten_fetch*
 * directly — those are JS library functions unavailable in side modules.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef void (*fetch_success_fn)(void *userdata, const void *data,
				 uint32_t size);
typedef void (*fetch_error_fn)(void *userdata, int http_status);

struct fetch_api {
	void (*fetch)(const char *url, void *userdata,
		      fetch_success_fn on_success, fetch_error_fn on_error);
};

#ifdef __cplusplus
}
#endif

#endif /* FETCH_API_H */
