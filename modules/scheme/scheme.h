/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SCHEME_H
#define SCHEME_H

#include <stdint.h>

/*
 * Phase-0 Scheme runtime surface (#271). Evaluates self-contained expressions
 * on a process-wide s7 interpreter; bound to nothing engine-aware yet. Richer
 * bindings — entity/scene marshalling, a live REPL — arrive with the full
 * scheme_api vtable in Epic B (#270).
 */
struct scheme_api {
	/*
	 * Eval src; on an integer result store it in *out and return 0.
	 * Returns -1 on a Scheme-level error or a non-integer result.
	 */
	int (*eval_int)(const char *src, int64_t *out);
	/*
	 * Eval src; on a string result copy it (NUL-terminated) into buf and
	 * return 0. Returns -1 on error, a non-string result, or if buf is too
	 * small to hold the string.
	 */
	int (*eval_string)(const char *src, char *buf, uint32_t buf_len);
	/*
	 * Eval src for effect; return 0 if it evaluated without a Scheme-level
	 * error, -1 otherwise.
	 */
	int (*eval_ok)(const char *src);
};

/*
 * Service vtable for native callers and tests. Boots the interpreter on first
 * use; never returns NULL.
 */
const struct scheme_api *scheme_native_api(void);

/* Native subsystem entry point. The WASM side module exports plugin_entry. */
struct subsystem_manager;
void scheme_plugin_entry(struct subsystem_manager *mgr);

#endif /* SCHEME_H */
