/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef AUDIO_SCRIPT_H
#define AUDIO_SCRIPT_H

/*
 * audio_script — the bridge between a game's Scheme and the audio_api the
 * device backend registers.
 *
 * It registers (audio-play! path [vol pan rate]) against the shared s7
 * interpreter: a path-based capability any game's rules can reach for, owned
 * by the module that owns audio rather than by the editor chrome (ui/kruddgui
 * had the only such primitive before this, kgui-play-sound, an asset-id call
 * scoped to the gui). Resolving the "audio" service through subsystem_manager
 * (rather than linking a backend directly) is what keeps this file the same
 * on a build with a device backend and on one without: no backend registered
 * means the lookup keeps returning NULL and every call stays a silent no-op,
 * exactly as an unresolved audio api was a no-op for the game plugins that
 * reached the backend through the vtable directly.
 */

struct subsystem_manager;

/*
 * Register audio-play! against the shared interpreter and remember MGR, used
 * to resolve the "audio" service lazily on each call (it may not be
 * registered yet at the point this runs). Idempotent; a no-op if the
 * interpreter is unavailable.
 */
void audio_script_init(struct subsystem_manager *mgr);

#endif /* AUDIO_SCRIPT_H */
