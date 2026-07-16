/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIXER_H
#define MIXER_H

#include "sound.h"
#include "memory_api.h"

#include <stdint.h>

/*
 * mixer — the playback core: one baked sound_blob, many cheap voices. A blob is
 * a sound's identity (its baked waveform, see asset/sound_script.c); a voice is
 * one playing instance of it, carrying only the live variation — gain, pan, and
 * playback rate — that must never trigger a re-bake. The same beep blob feeds a
 * dozen voices at different volumes and pans for the cost of a dozen playheads,
 * exactly as one texture's baked texels feed many draws at different tints.
 *
 * The mixer sums its active voices into an interleaved stereo float buffer. It
 * is pure arithmetic over pre-baked samples — no Scheme, no allocation, no
 * locks in mixer_render — precisely so it can run from a real-time audio
 * callback (the WebAudio AudioWorklet backend, a separate change) without ever
 * touching the interpreter. This layer is the substrate that backend drives.
 */

/* Voices that can sound at once; mixer_play past this fails rather than steal. */
#define MIXER_MAX_VOICES 32u

struct mixer;

/*
 * Create a mixer rendering at SAMPLE_RATE Hz into stereo. A blob baked at a
 * different rate still plays at correct pitch and duration — the voice steps its
 * playhead by the rate ratio. Returns NULL on a bad rate or allocation failure;
 * the caller frees it with mixer_destroy.
 */
struct mixer *mixer_create(const struct memory_api *mem, uint32_t sample_rate);
void          mixer_destroy(struct mixer *m);

/*
 * Start a one-shot voice playing B. GAIN (clamped to [0,1]) scales it; PAN in
 * [-1,1] places a mono blob across the stereo field with a constant-power law
 * (-1 hard left, 0 centre, +1 hard right) — a stereo blob plays its channels
 * straight and ignores PAN. RATE is a playback-speed multiplier (> 0; 1.0 =
 * native pitch, 2.0 an octave up and half as long). B must outlive the voice.
 *
 * Returns a nonzero handle for mixer_stop, or 0 when B is NULL/empty, RATE is
 * non-positive, or all MIXER_MAX_VOICES slots are busy. The handle carries a
 * generation, so a stale handle to a finished-and-reused slot is inert.
 */
uint32_t mixer_play(struct mixer *m, const struct sound_blob *b,
		    float gain, float pan, float rate);

/* Stop a voice early. A stale, finished, or never-issued handle is a no-op. */
void mixer_stop(struct mixer *m, uint32_t voice);

/* How many voices are currently sounding. */
uint32_t mixer_active(const struct mixer *m);

/*
 * Render FRAMES interleaved stereo frames into OUT (FRAMES*2 floats), summing
 * every active voice, advancing each playhead, and retiring voices that run past
 * their blob. OUT is overwritten, not accumulated into. The mix bus is left
 * unclipped — the device (or a limiter ahead of it) owns the final clamp to
 * [-1,1], so summed voices can legitimately exceed it here.
 */
void mixer_render(struct mixer *m, float *out, uint32_t frames);

#endif /* MIXER_H */
