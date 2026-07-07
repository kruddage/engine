/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "scheme.h"
#include "s7.h"
#include "subsystem.h"
#include "subsystem_manager.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * One interpreter per process. Phase 0 needs exactly one; keeping it static
 * sidesteps any allocation of our own (s7 owns its heap internally). Epic B,
 * which adds engine bindings, is where a managed lifetime becomes worthwhile.
 */
static s7_scheme *g_s7;

static int scheme_boot(void)
{
	if (!g_s7)
		g_s7 = s7_init();
	return g_s7 ? 0 : -1;
}

/*
 * Evaluate src with the error port redirected into a string port. A Scheme
 * error then leaves text in the capture (detectable) instead of printing to
 * stderr, and the interpreter stays usable for the next call. Sets *errored
 * to 1 on any failure to even run, 0 only on a clean eval.
 */
static s7_pointer eval_capture(const char *src, int *errored)
{
	s7_pointer  port, saved, result;
	const char *text;

	*errored = 1;
	if (!src || scheme_boot() != 0)
		return NULL;

	port   = s7_open_output_string(g_s7);
	saved  = s7_set_current_error_port(g_s7, port);
	result = s7_eval_c_string(g_s7, src);
	text   = s7_get_output_string(g_s7, port);
	*errored = (text && text[0] != '\0') ? 1 : 0;
	s7_set_current_error_port(g_s7, saved);
	s7_close_output_port(g_s7, port);
	return result;
}

static int scheme_eval_int(const char *src, int64_t *out)
{
	int        err;
	s7_pointer r = eval_capture(src, &err);

	if (err || !s7_is_integer(r))
		return -1;
	if (out)
		*out = (int64_t)s7_integer(r);
	return 0;
}

static int scheme_eval_string(const char *src, char *buf, uint32_t buf_len)
{
	int         err;
	s7_pointer  r = eval_capture(src, &err);
	const char *s;
	size_t      n;

	if (err || !s7_is_string(r) || !buf || buf_len == 0)
		return -1;
	s = s7_string(r);
	n = strlen(s);
	if (n >= buf_len)
		return -1;
	memcpy(buf, s, n + 1);
	return 0;
}

static int scheme_eval_ok(const char *src)
{
	int err;

	(void)eval_capture(src, &err);
	return err ? -1 : 0;
}

static const struct scheme_api g_scheme_api = {
	.eval_int    = scheme_eval_int,
	.eval_string = scheme_eval_string,
	.eval_ok     = scheme_eval_ok,
};

const struct scheme_api *scheme_native_api(void)
{
	return &g_scheme_api;
}

/* --- Subsystem lifecycle ------------------------------------------------ */

static void scheme_init(void)
{
	int64_t v = 0;

	if (scheme_boot() != 0) {
		printf("scheme: s7 init failed\n");
		return;
	}
	/* Prove the interpreter is live at load: (+ 1 2) => 3. */
	if (scheme_eval_int("(+ 1 2)", &v) == 0)
		printf("scheme: s7 " S7_VERSION " ready, (+ 1 2) = %lld\n",
		       (long long)v);
	else
		printf("scheme: s7 smoke eval failed\n");
}

static void scheme_shutdown(void)
{
	if (g_s7) {
		s7_free(g_s7);
		g_s7 = NULL;
	}
}

static const struct subsystem scheme_desc = {
	.name     = "scheme",
	.api      = &g_scheme_api,
	.init     = scheme_init,
	.shutdown = scheme_shutdown,
};

#ifdef __EMSCRIPTEN__
void plugin_entry(struct subsystem_manager *mgr)
#else
void scheme_plugin_entry(struct subsystem_manager *mgr)
#endif
{
	subsystem_manager_register(mgr, &scheme_desc);
}
