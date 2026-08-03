/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * audio_script — registers (audio-play! path [vol pan rate]) against the
 * shared s7 interpreter (see audio_script.h for the seam this closes).
 */
#include "audio_script.h"

#include <abi/audio_api.h>
#include <core/script.h>
#include <core/subsystem_manager.h>

#include "s7.h"

/*
 * Defaults mirror kgui-play-sound's hardcoded call (ui/kruddgui/kruddgui.cpp)
 * -- the only generic (path-independent policy) playback call site before
 * this primitive existed. chess.c's chess_play_sound passes its own
 * per-outcome gain instead; that mapping is chess policy and stays put.
 */
#define AUDIO_SCRIPT_DEFAULT_VOL  0.8
#define AUDIO_SCRIPT_DEFAULT_PAN  0.0
#define AUDIO_SCRIPT_DEFAULT_RATE 1.0

/* Resolved lazily: "audio" may not be registered yet when audio_script_init
 * runs (it registers itself), so the first call after boot does the lookup. A
 * build with no audio backend never finds one, so g_audio stays NULL and
 * every call below is a no-op. */
static struct subsystem_manager *g_mgr;
static const struct audio_api   *g_audio;

/* The (0-based) Nth element of REST past the required PATH arg, or DEF when
 * REST is shorter than that (the argument was omitted) or holds a non-number
 * there. */
static double opt_real(s7_scheme *sc, s7_pointer rest, int32_t n, double def)
{
	s7_pointer p = rest;
	int32_t    i;

	for (i = 0; i < n && s7_is_pair(p); i++)
		p = s7_cdr(p);
	return (s7_is_pair(p) && s7_is_number(s7_car(p)))
		? s7_number_to_real(sc, s7_car(p)) : def;
}

/*
 * (audio-play! path [vol pan rate]) -> unspecified. A game's rules call this
 * directly -- no gui module involved. Never a crash: a non-string PATH, no
 * audio backend (a build with none, or the lookup not yet resolved), and a
 * path naming no loaded asset (play_path itself reports that by returning 0)
 * are all silent no-ops.
 */
static s7_pointer sp_audio_play(s7_scheme *sc, s7_pointer args)
{
	s7_pointer path = s7_car(args);
	s7_pointer rest = s7_cdr(args);
	double     vol, pan, rate;

	if (!g_audio && g_mgr)
		g_audio = subsystem_manager_get_api(g_mgr, "audio");
	if (!g_audio || !g_audio->play_path || !s7_is_string(path))
		return s7_unspecified(sc);

	vol  = opt_real(sc, rest, 0, AUDIO_SCRIPT_DEFAULT_VOL);
	pan  = opt_real(sc, rest, 1, AUDIO_SCRIPT_DEFAULT_PAN);
	rate = opt_real(sc, rest, 2, AUDIO_SCRIPT_DEFAULT_RATE);
	g_audio->play_path(s7_string(path), (float)vol, (float)pan,
			   (float)rate);
	return s7_unspecified(sc);
}

void audio_script_init(struct subsystem_manager *mgr)
{
	static int registered;
	s7_scheme *sc;

	if (registered)
		return;
	sc = script_s7();
	if (!sc)
		return;
	g_mgr = mgr;
	s7_define_function(sc, "audio-play!", sp_audio_play, 1, 3, false,
			   "(audio-play! path [vol pan rate]) play a "
			   "catalog sound by path");
	registered = 1;
}
