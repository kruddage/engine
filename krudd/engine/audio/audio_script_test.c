/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * audio_script — the primitive end to end against a real s7 interpreter and a
 * hand-built subsystem_manager. A stub audio_api stands in for a device
 * backend so the test can check both what a game's rules can rely on (the
 * defaults, the optional overrides) and the crash-safety a build with no
 * backend or an unknown path depends on.
 */
#include "audio_script.h"

#include <abi/audio_api.h>
#include <core/script.h>
#include <core/subsystem_manager.h>

#include "s7.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define EPS 1.0e-4f

static int feq(float a, float b) { return fabsf(a - b) <= EPS; }

/* Records the last play_path call so a test can check what the primitive
 * forwarded; "found" mimics the real backend's contract of returning 0 for a
 * path naming no loaded asset. */
static struct {
	int      calls;
	char     path[128];
	float    gain, pan, rate;
	uint32_t found_id;
} stub;

static void stub_reset(void)
{
	memset(&stub, 0, sizeof(stub));
}

static uint32_t stub_play_path(const char *path, float gain, float pan,
			       float rate)
{
	stub.calls++;
	strncpy(stub.path, path ? path : "", sizeof(stub.path) - 1);
	stub.gain = gain;
	stub.pan  = pan;
	stub.rate = rate;
	return stub.found_id;
}

static const struct audio_api stub_audio_api = { .play_path = stub_play_path };

static const struct subsystem empty_table[] = { { NULL } };

/*
 * One manager for the whole run, exactly like the engine's: audio_script_init
 * is idempotent and remembers whichever manager it first saw (see
 * audio_script.c), so a second, different manager per test would leave it
 * holding a stale pointer once that test's manager went out of scope. Tests
 * run in the order below -- the no-backend case first, then "audio"
 * registers once and stays up for the rest.
 */
static struct subsystem_manager g_mgr;

/*
 * Before any "audio" service is registered -- a build with no audio backend,
 * or the window before one registers -- (audio-play! ...) must be a silent
 * no-op: no crash, and (since there is nothing to call) the stub sees
 * nothing either.
 */
static void test_no_backend_is_silent_noop(void)
{
	s7_pointer result;

	stub_reset();

	result = s7_eval_c_string(script_s7(),
				  "(audio-play! \"builtin://sound/blip\")");
	assert(s7_is_unspecified(script_s7(), result));
	assert(stub.calls == 0);
}

/*
 * Once "audio" registers, the same call reaches play_path with today's
 * defaults (0.8 gain, 0.0 pan, 1.0 rate) -- the values kgui-play-sound
 * hardcodes, generalized rather than invented.
 */
static void test_defaults_match_kgui_play_sound(void)
{
	stub_reset();
	stub.found_id = 7;

	s7_eval_c_string(script_s7(), "(audio-play! \"builtin://sound/blip\")");

	assert(stub.calls == 1);
	assert(strcmp(stub.path, "builtin://sound/blip") == 0);
	assert(feq(stub.gain, 0.8f));
	assert(feq(stub.pan, 0.0f));
	assert(feq(stub.rate, 1.0f));
}

/* Explicit vol/pan/rate override the defaults and reach play_path untouched. */
static void test_optional_args_override_defaults(void)
{
	stub_reset();

	s7_eval_c_string(script_s7(),
			 "(audio-play! \"builtin://sound/beep\" 0.3 -0.5 2.0)");

	assert(stub.calls == 1);
	assert(strcmp(stub.path, "builtin://sound/beep") == 0);
	assert(feq(stub.gain, 0.3f));
	assert(feq(stub.pan, -0.5f));
	assert(feq(stub.rate, 2.0f));
}

/*
 * A path naming no loaded asset is exactly as silent as no backend at all:
 * play_path itself reports "not found" by returning 0, and the primitive
 * never inspects that return value, so nothing here can crash on it.
 */
static void test_unknown_path_no_crash(void)
{
	stub_reset();
	stub.found_id = 0; /* "not found" */

	s7_eval_c_string(script_s7(), "(audio-play! \"no/such/path\")");

	/* Forwarded regardless -- resolving the path is play_path's job. */
	assert(stub.calls == 1);
	assert(strcmp(stub.path, "no/such/path") == 0);
}

/* A non-string path (a stray number, say) degrades to a no-op rather than a
 * type error taking the interpreter down. */
static void test_non_string_path_no_crash(void)
{
	stub_reset();

	s7_eval_c_string(script_s7(), "(audio-play! 42)");

	assert(stub.calls == 0);
}

int main(void)
{
	struct subsystem audio_desc = { .name = "audio", .api = &stub_audio_api };

	script_init(); /* boots the shared s7 interpreter */
	subsystem_manager_init(&g_mgr, empty_table);
	audio_script_init(&g_mgr); /* registers audio-play!; no "audio" yet */

	test_no_backend_is_silent_noop();

	subsystem_manager_register(&g_mgr, &audio_desc);
	test_defaults_match_kgui_play_sound();
	test_optional_args_override_defaults();
	test_unknown_path_no_crash();
	test_non_string_path_no_crash();

	printf("audio_script tests passed\n");
	return 0;
}
