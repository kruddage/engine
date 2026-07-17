/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef AUDIO_API_H
#define AUDIO_API_H

#include <stdint.h>

/*
 * Audio service vtable, published by the WebAudio backend under the name
 * "audio". Obtain via subsystem_manager_get_api(mgr, "audio"). This is how the
 * rest of the engine — and, in time, game scripts — asks for a sound to play:
 * name a baked ASSET_TYPE_SOUND asset and the per-voice gain/pan/rate, and the
 * backend bakes it (once, cached), hands the blob to the mixer, and mixes it on
 * the audio thread. Playback is fire-and-forget; a one-shot voice retires at the
 * end of its blob.
 */
struct audio_api {
	/*
	 * Play sound asset ID once. gain is clamped to [0,1], pan spans [-1,1]
	 * (constant-power across the stereo field), rate is a playback-speed
	 * multiplier (> 0; 1.0 = native pitch). A no-op if audio is unavailable
	 * or the asset can't be baked.
	 */
	void (*play)(uint32_t asset_id, float gain, float pan, float rate);
	/*
	 * Resolve a catalog path (e.g. "builtin://sound/beep") to its id and
	 * play it. Returns the id played, or 0 if no such asset is loaded.
	 */
	uint32_t (*play_path)(const char *path, float gain, float pan,
			      float rate);
};

#endif /* AUDIO_API_H */
