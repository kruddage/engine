/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * audio_core — the thread-seam logic in isolation, driven single-threaded (the
 * ring is exercised by producing then consuming in one thread, which is a valid
 * schedule of the SPSC protocol). Checks the load-bearing behaviours the audio
 * thread depends on: a sound is baked once and cached, playing enqueues without
 * touching the mixer, render drains the ring into the mixer and sums, and a play
 * past a full ring is dropped rather than stalling. The Emscripten worklet glue
 * that runs render on the real audio thread is not tested here — it can't be,
 * off a browser — but everything it calls is.
 */
#include "audio_core.h"

#include "memory.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>

#define EPS 1.0e-4f

static const struct memory_api test_mem_impl = {
	mem_alloc, mem_alloc_zero, mem_free,
	mem_pool_create, mem_pool_alloc, mem_pool_free, mem_pool_destroy,
};
static const struct memory_api *g_mem = &test_mem_impl;

/* A baker that hands back a mono constant-1.0 blob and counts its calls, so a
 * test can prove the cache bakes each id exactly once. It also asserts the core
 * asks for the rate and channel count the contract promises. */
struct stub_baker {
	int      calls;
	uint32_t last_rate;
	uint32_t last_channels;
};

static struct sound_blob *stub_bake(void *ctx, uint32_t asset_id,
				    uint32_t sample_rate, uint32_t channels)
{
	struct stub_baker *sb = ctx;
	struct sound_blob *b;
	float             *s;
	uint32_t           i, frames = 8;

	(void)asset_id;
	sb->calls++;
	sb->last_rate     = sample_rate;
	sb->last_channels = channels;

	b = g_mem->alloc(sound_blob_size(frames, channels));
	assert(b != NULL);
	b->magic       = SOUND_BLOB_MAGIC;
	b->frame_count = frames;
	b->sample_rate = sample_rate;
	b->channels    = channels;
	b->format      = SOUND_FORMAT_F32;
	s = (float *)(void *)(b + 1);
	for (i = 0; i < frames * channels; i++)
		s[i] = 1.0f;
	return b;
}

static void test_bakes_once_and_caches(void)
{
	struct stub_baker  sb = { 0, 0, 0 };
	struct audio_core *c  = audio_core_create(g_mem, 48000, stub_bake, &sb);

	assert(c != NULL);
	assert(audio_core_play_asset(c, 7, 1.0f, 0.0f, 1.0f) == 1);
	assert(audio_core_play_asset(c, 7, 1.0f, 0.0f, 1.0f) == 1);
	assert(sb.calls == 1);                       /* second play hit cache */
	assert(sb.last_rate == 48000);               /* device rate requested */
	assert(sb.last_channels == SOUND_CHANNELS_MONO); /* mono, so pan works */
	/* Play does not touch the mixer — the audio thread hasn't drained yet. */
	assert(audio_core_active(c) == 0);
	audio_core_destroy(c);
}

static void test_render_drains_and_mixes(void)
{
	struct stub_baker  sb = { 0, 0, 0 };
	struct audio_core *c  = audio_core_create(g_mem, 48000, stub_bake, &sb);
	float              out[8];

	assert(audio_core_play_asset(c, 1, 1.0f, 0.0f, 1.0f) == 1);
	assert(audio_core_play_asset(c, 2, 1.0f, 0.0f, 1.0f) == 1);
	assert(sb.calls == 2); /* two distinct ids -> two bakes */

	audio_core_render(c, out, 4);
	assert(audio_core_active(c) == 2); /* both commands became voices */
	/* Two centre-panned mono voices at 1.0: 2 * 1/sqrt(2) ~ 1.414. */
	assert(fabsf(out[0] - 2.0f * 0.70710678f) <= EPS);
	audio_core_destroy(c);
}

static void test_play_blob_direct(void)
{
	struct audio_core *c = audio_core_create(g_mem, 48000, NULL, NULL);
	struct sound_blob *b = stub_bake(&(struct stub_baker){ 0, 0, 0 }, 0,
					 48000, SOUND_CHANNELS_MONO);
	float              out[8];

	/* No baker, but a caller-owned blob plays directly. */
	assert(audio_core_play_asset(c, 5, 1.0f, 0.0f, 1.0f) == 0); /* no baker */
	assert(audio_core_play_blob(c, b, 1.0f, -1.0f, 1.0f) == 1);
	audio_core_render(c, out, 4);
	assert(fabsf(out[0] - 1.0f) <= EPS); /* hard left */
	audio_core_destroy(c);
	g_mem->free(b);
}

static void test_ring_full_drops(void)
{
	struct audio_core *c = audio_core_create(g_mem, 48000, NULL, NULL);
	struct sound_blob *b = stub_bake(&(struct stub_baker){ 0, 0, 0 }, 0,
					 48000, SOUND_CHANNELS_MONO);
	uint32_t           i;

	/* Fill the ring without draining: capacity succeeds, the next drops. */
	for (i = 0; i < AUDIO_CORE_QUEUE; i++)
		assert(audio_core_play_blob(c, b, 1.0f, 0.0f, 1.0f) == 1);
	assert(audio_core_play_blob(c, b, 1.0f, 0.0f, 1.0f) == 0);
	audio_core_destroy(c);
	g_mem->free(b);
}

static void test_bad_args_and_null(void)
{
	struct audio_core *c = audio_core_create(g_mem, 48000, NULL, NULL);
	struct sound_blob  empty = { SOUND_BLOB_MAGIC, 0, 48000,
				     SOUND_CHANNELS_MONO, SOUND_FORMAT_F32 };
	float              out[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
	uint32_t           i;

	assert(audio_core_play_blob(c, NULL, 1.0f, 0.0f, 1.0f) == 0);
	assert(audio_core_play_blob(c, &empty, 1.0f, 0.0f, 1.0f) == 0);
	assert(audio_core_create(NULL, 48000, NULL, NULL) == NULL);
	assert(audio_core_create(g_mem, 0, NULL, NULL) == NULL);
	/* A NULL core renders silence, not a crash. */
	audio_core_render(NULL, out, 4);
	for (i = 0; i < 8; i++)
		assert(fabsf(out[i]) <= EPS);
	audio_core_destroy(c);
}

int main(void)
{
	mem_init();

	test_bakes_once_and_caches();
	test_render_drains_and_mixes();
	test_play_blob_direct();
	test_ring_full_drops();
	test_bad_args_and_null();

	printf("audio_core tests passed\n");
	return 0;
}
