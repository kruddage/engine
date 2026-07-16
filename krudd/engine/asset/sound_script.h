/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SOUND_SCRIPT_H
#define SOUND_SCRIPT_H

#include "sound.h"
#include "memory_api.h"

#include <stdint.h>

/*
 * sound_script — the bridge between an ASSET_TYPE_SOUND asset's Scheme source
 * and a real sound_blob. There is no other kind of sound asset: every built-in
 * or authored sound is a (sound NAME (sample (t) ...)) form, and this is what
 * bakes one into playable PCM.
 *
 * Evaluate SRC — a (sound NAME [(params ...)] (sample (t) ...)) form, see
 * core/sound_script.scm — against the shared s7 image, sampling its sample
 * clause frame by frame, and marshal the interleaved-stereo float32 result into
 * a heap-allocated sound_blob. SAMPLE_RATE is the output rate the caller
 * chooses (the script is rate-independent); 0 defaults to
 * SOUND_SCRIPT_DEFAULT_RATE and the value is clamped to
 * [SOUND_SCRIPT_MIN_RATE, SOUND_SCRIPT_MAX_RATE]. The frame count is derived
 * from the sound's (duration ...) param (SOUND_SCRIPT_DEFAULT_DURATION seconds
 * when the sound declares none) times the rate, clamped to
 * SOUND_SCRIPT_MAX_FRAMES. PARAMS (PLEN bytes, or NULL) is the tight-packed
 * override the sample body reads through (param ...); missing/short params fall
 * back to their declared defaults.
 *
 * The caller owns the returned buffer and frees it with mem->free; *out_size
 * (may be NULL) receives the total byte count. Returns NULL when the
 * interpreter is unavailable, the derived frame count is zero (a non-positive
 * duration), SRC fails to parse/eval, or its sample clause faults — never a
 * partially filled blob.
 */
#define SOUND_SCRIPT_DEFAULT_RATE     48000u
#define SOUND_SCRIPT_MIN_RATE          8000u
#define SOUND_SCRIPT_MAX_RATE        192000u
#define SOUND_SCRIPT_DEFAULT_DURATION   1.0f
/* 10 seconds at the max rate — a hard ceiling on a single procedural SFX bake. */
#define SOUND_SCRIPT_MAX_FRAMES     1920000u

struct sound_blob *sound_script_generate(const char *src,
					 const uint8_t *params,
					 uint32_t plen,
					 uint32_t sample_rate,
					 const struct memory_api *mem,
					 uint32_t *out_size);

#endif /* SOUND_SCRIPT_H */
