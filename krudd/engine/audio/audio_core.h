/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef AUDIO_CORE_H
#define AUDIO_CORE_H

#include "sound.h"
#include "memory_api.h"

#include <stdint.h>

/*
 * audio_core — the thread seam between the game (main thread) and the audio
 * device (the AudioWorklet render thread). It owns the mixer and hands work
 * across the boundary through a single-producer / single-consumer command ring,
 * so the two threads never touch the mixer's voice array at the same time.
 *
 *   main thread            audio (worklet) thread
 *   ───────────            ──────────────────────
 *   audio_core_play_*  →   audio_core_render
 *     bake (S7) + cache      drain ring → mixer_play
 *     enqueue a blob ptr     mixer_render into the device buffer
 *
 * The split is load-bearing: baking runs the S7 interpreter and allocates, so
 * it MUST stay on the main thread; the render thread only ever reads immutable
 * baked blobs and sums them. The ring carries a plain blob pointer plus its
 * gain/pan/rate — no allocation, no locks, no interpreter on the audio thread.
 *
 * Playback is fire-and-forget: a one-shot voice retires itself at the end of
 * its blob (see mixer.c). Stop-by-handle across the thread boundary is a later
 * concern; this layer is the minimum that makes a baked sound audible.
 *
 * audio_core is browser-agnostic — the mixer and ring are pure C, and the baker
 * is injected — so it is exercised natively by audio_core_test.c. The Emscripten
 * AudioWorklet glue that drives it lives in audio_webaudio.c.
 */

/* Commands the ring can hold before the audio thread drains them; a play past a
 * full ring is dropped (the sound is skipped, never a stall). Power of two. */
#define AUDIO_CORE_QUEUE 64u

/* Distinct sounds whose baked blobs the core caches for the session. */
#define AUDIO_CORE_CACHE 32u

struct audio_core;

/*
 * Resolve a sound asset id to a freshly baked, heap-allocated sound_blob at the
 * given rate and channel count, or NULL on failure. The core takes ownership of
 * the returned blob, caches it, and frees it (through the same memory_api) on
 * destroy — so a baker allocates and forgets. Called only on the main thread.
 */
typedef struct sound_blob *(*audio_bake_fn)(void *ctx, uint32_t asset_id,
					    uint32_t sample_rate,
					    uint32_t channels);

/*
 * Create a core rendering at SAMPLE_RATE Hz (the device's rate). BAKE/BAKE_CTX
 * resolve asset ids to blobs on demand; either may be NULL to run blob-only
 * (audio_core_play_blob still works, audio_core_play_asset always fails).
 * Returns NULL on a bad rate or allocation failure.
 */
struct audio_core *audio_core_create(const struct memory_api *mem,
				     uint32_t sample_rate,
				     audio_bake_fn bake, void *bake_ctx);
void               audio_core_destroy(struct audio_core *c);

/*
 * Main thread: play sound asset ID once at GAIN/PAN/RATE (see mixer_play). The
 * blob is baked mono on first use and cached, so pan places it in the stereo
 * field. Returns 1 if the command was enqueued, 0 if the asset can't be baked
 * or the ring is full.
 */
int audio_core_play_asset(struct audio_core *c, uint32_t asset_id,
			  float gain, float pan, float rate);

/*
 * Main thread: play an already-baked blob the caller owns (it must outlive the
 * voice). Bypasses the bake cache — the seam the native test drives directly.
 * Returns 1 if enqueued, 0 if B is NULL/empty or the ring is full.
 */
int audio_core_play_blob(struct audio_core *c, const struct sound_blob *b,
			 float gain, float pan, float rate);

/*
 * Audio thread: drain the command ring into the mixer, then render FRAMES
 * interleaved stereo frames into OUT (FRAMES*2 floats). This is the only
 * function the worklet callback calls — no allocation, no interpreter, safe to
 * run in real time. Draining here (not in play) is what keeps mixer_play on a
 * single thread.
 */
void audio_core_render(struct audio_core *c, float *out, uint32_t frames);

/* Diagnostic: voices currently sounding. Racy across threads by nature. */
uint32_t audio_core_active(const struct audio_core *c);

#endif /* AUDIO_CORE_H */
