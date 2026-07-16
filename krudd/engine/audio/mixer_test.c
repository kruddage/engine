/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mixer — the playback core in isolation: build sound_blobs by hand (no
 * interpreter needed), start voices, render, and check the summed stereo
 * output. The load-bearing properties are the ones a real-time callback depends
 * on — constant-power panning, per-voice gain, honest summing without a hidden
 * clamp, playheads that retire at the end of a blob, rate as pitch/duration, and
 * generation-tagged handles that make a stale stop inert.
 */
#include "mixer.h"

#include "memory.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>

#define EPS      1.0e-4f
#define HALF_SQ2 0.70710678f /* cos(pi/4) == sin(pi/4): centre-pan gain */

static const struct memory_api test_mem_impl = {
	mem_alloc, mem_alloc_zero, mem_free,
	mem_pool_create, mem_pool_alloc, mem_pool_free, mem_pool_destroy,
};
static const struct memory_api *g_mem = &test_mem_impl;

/* Allocate a blob; the caller fills its samples through blob_samples(). */
static struct sound_blob *make_blob(uint32_t frames, uint32_t channels,
				    uint32_t rate)
{
	struct sound_blob *b = g_mem->alloc(sound_blob_size(frames, channels));

	assert(b != NULL);
	b->magic       = SOUND_BLOB_MAGIC;
	b->frame_count = frames;
	b->sample_rate = rate;
	b->channels    = channels;
	b->format      = SOUND_FORMAT_F32;
	return b;
}

static float *blob_samples(struct sound_blob *b)
{
	return (float *)(void *)(b + 1);
}

/* A mono blob whose every sample is VALUE. */
static struct sound_blob *mono_const(uint32_t frames, float value, uint32_t rate)
{
	struct sound_blob *b = make_blob(frames, SOUND_CHANNELS_MONO, rate);
	float             *s = blob_samples(b);
	uint32_t           i;

	for (i = 0; i < frames; i++)
		s[i] = value;
	return b;
}

static void test_idle_is_silent(void)
{
	struct mixer *m = mixer_create(g_mem, 48000);
	float         out[8];
	uint32_t      i;

	assert(m != NULL);
	assert(mixer_active(m) == 0);
	for (i = 0; i < 8; i++)
		out[i] = 999.0f;
	mixer_render(m, out, 4); /* overwrites, does not accumulate */
	for (i = 0; i < 8; i++)
		assert(fabsf(out[i]) <= EPS);
	mixer_destroy(m);
}

static void test_mono_centre_pan(void)
{
	struct mixer      *m = mixer_create(g_mem, 48000);
	struct sound_blob *b = mono_const(16, 1.0f, 48000);
	float              out[8];

	assert(mixer_play(m, b, 1.0f, 0.0f, 1.0f) != 0);
	assert(mixer_active(m) == 1);
	mixer_render(m, out, 4);
	/* Centre pan: both channels at 1/sqrt(2). */
	assert(fabsf(out[0] - HALF_SQ2) <= EPS);
	assert(fabsf(out[1] - HALF_SQ2) <= EPS);
	mixer_destroy(m);
	g_mem->free(b);
}

static void test_mono_hard_left(void)
{
	struct mixer      *m = mixer_create(g_mem, 48000);
	struct sound_blob *b = mono_const(16, 1.0f, 48000);
	float              out[8];

	assert(mixer_play(m, b, 1.0f, -1.0f, 1.0f) != 0);
	mixer_render(m, out, 4);
	assert(fabsf(out[0] - 1.0f) <= EPS); /* all left */
	assert(fabsf(out[1]) <= EPS);        /* nothing right */
	mixer_destroy(m);
	g_mem->free(b);
}

static void test_gain_scales(void)
{
	struct mixer      *m = mixer_create(g_mem, 48000);
	struct sound_blob *b = mono_const(16, 1.0f, 48000);
	float              out[8];

	assert(mixer_play(m, b, 0.5f, 0.0f, 1.0f) != 0);
	mixer_render(m, out, 4);
	assert(fabsf(out[0] - 0.5f * HALF_SQ2) <= EPS);
	mixer_destroy(m);
	g_mem->free(b);
}

static void test_stereo_passthrough(void)
{
	struct mixer      *m = mixer_create(g_mem, 48000);
	struct sound_blob *b = make_blob(16, SOUND_CHANNELS_STEREO, 48000);
	float             *s = blob_samples(b);
	float              out[8];
	uint32_t           i;

	for (i = 0; i < 16; i++) {
		s[i * 2]     = 0.5f;
		s[i * 2 + 1] = -0.25f;
	}
	/* Stereo blob: channels pass straight through, pan ignored. */
	assert(mixer_play(m, b, 1.0f, 0.7f, 1.0f) != 0);
	mixer_render(m, out, 4);
	assert(fabsf(out[0] - 0.5f) <= EPS);
	assert(fabsf(out[1] + 0.25f) <= EPS);
	mixer_destroy(m);
	g_mem->free(b);
}

static void test_summing_is_unclipped(void)
{
	struct mixer      *m = mixer_create(g_mem, 48000);
	struct sound_blob *b = mono_const(16, 1.0f, 48000);
	float              out[8];

	/* Two centre voices sum to 2/sqrt(2) ~ 1.414 — past [-1,1], and left
	 * there: the device owns the final clamp, not the mix bus. */
	assert(mixer_play(m, b, 1.0f, 0.0f, 1.0f) != 0);
	assert(mixer_play(m, b, 1.0f, 0.0f, 1.0f) != 0);
	assert(mixer_active(m) == 2);
	mixer_render(m, out, 4);
	assert(fabsf(out[0] - 2.0f * HALF_SQ2) <= EPS);
	assert(out[0] > 1.0f);
	mixer_destroy(m);
	g_mem->free(b);
}

static void test_voice_retires_at_end(void)
{
	struct mixer      *m = mixer_create(g_mem, 48000);
	struct sound_blob *b = mono_const(4, 1.0f, 48000); /* 4 frames only */
	float              out[16];
	uint32_t           f;

	assert(mixer_play(m, b, 1.0f, -1.0f, 1.0f) != 0);
	mixer_render(m, out, 8); /* render past the blob's length */
	/* Frames 0..3 carry signal, 4..7 fall silent as the voice retires. */
	for (f = 0; f < 4; f++)
		assert(fabsf(out[f * 2] - 1.0f) <= EPS);
	for (f = 4; f < 8; f++)
		assert(fabsf(out[f * 2]) <= EPS);
	assert(mixer_active(m) == 0);
	mixer_destroy(m);
	g_mem->free(b);
}

static void test_rate_is_pitch_and_duration(void)
{
	struct mixer      *m = mixer_create(g_mem, 48000);
	struct sound_blob *b = mono_const(8, 1.0f, 48000);
	float              out[32];
	uint32_t           f;

	/* Rate 2.0 steps two source frames per output frame, so an 8-frame blob
	 * empties in 4 output frames. */
	assert(mixer_play(m, b, 1.0f, -1.0f, 2.0f) != 0);
	mixer_render(m, out, 8);
	for (f = 0; f < 4; f++)
		assert(fabsf(out[f * 2] - 1.0f) <= EPS);
	for (f = 4; f < 8; f++)
		assert(fabsf(out[f * 2]) <= EPS);
	assert(mixer_active(m) == 0);
	mixer_destroy(m);
	g_mem->free(b);
}

static void test_rate_interpolates(void)
{
	struct mixer      *m = mixer_create(g_mem, 48000);
	struct sound_blob *b = make_blob(8, SOUND_CHANNELS_MONO, 48000);
	float             *s = blob_samples(b);
	float              out[16];
	uint32_t           i;

	for (i = 0; i < 8; i++)
		s[i] = 0.1f * (float)i; /* a ramp: 0, 0.1, 0.2, ... */
	/* Rate 0.5 lands frame 1 at source pos 0.5 — the midpoint of 0 and 0.1,
	 * i.e. 0.05, so a non-native rate resamples rather than steps. */
	assert(mixer_play(m, b, 1.0f, -1.0f, 0.5f) != 0);
	mixer_render(m, out, 4);
	assert(fabsf(out[0] - 0.0f) <= EPS);   /* pos 0.0 */
	assert(fabsf(out[2] - 0.05f) <= EPS);  /* pos 0.5 -> midpoint */
	assert(fabsf(out[4] - 0.1f) <= EPS);   /* pos 1.0 */
	mixer_destroy(m);
	g_mem->free(b);
}

static void test_stop_silences(void)
{
	struct mixer      *m = mixer_create(g_mem, 48000);
	struct sound_blob *b = mono_const(16, 1.0f, 48000);
	float              out[8];
	uint32_t           h = mixer_play(m, b, 1.0f, 0.0f, 1.0f);

	assert(h != 0);
	mixer_stop(m, h);
	assert(mixer_active(m) == 0);
	mixer_render(m, out, 4);
	assert(fabsf(out[0]) <= EPS);
	mixer_destroy(m);
	g_mem->free(b);
}

/*
 * A handle carries a generation, so stopping a finished-and-reused slot by an
 * old handle must not stop the new occupant. Play A, stop it, play B into the
 * same slot, then replay A's stale stop: B keeps sounding.
 */
static void test_stale_handle_is_inert(void)
{
	struct mixer      *m = mixer_create(g_mem, 48000);
	struct sound_blob *b = mono_const(16, 1.0f, 48000);
	uint32_t           a = mixer_play(m, b, 1.0f, 0.0f, 1.0f);
	uint32_t           bb;

	mixer_stop(m, a);
	bb = mixer_play(m, b, 1.0f, 0.0f, 1.0f); /* reuses A's slot, new gen */
	assert(bb != 0 && bb != a);
	mixer_stop(m, a);                        /* stale: must be a no-op */
	assert(mixer_active(m) == 1);            /* B still sounding */
	mixer_stop(m, bb);
	assert(mixer_active(m) == 0);
	mixer_destroy(m);
	g_mem->free(b);
}

static void test_voice_exhaustion(void)
{
	struct mixer      *m = mixer_create(g_mem, 48000);
	struct sound_blob *b = mono_const(16, 1.0f, 48000);
	uint32_t           i;

	for (i = 0; i < MIXER_MAX_VOICES; i++)
		assert(mixer_play(m, b, 1.0f, 0.0f, 1.0f) != 0);
	assert(mixer_active(m) == MIXER_MAX_VOICES);
	assert(mixer_play(m, b, 1.0f, 0.0f, 1.0f) == 0); /* full: refuse */
	mixer_destroy(m);
	g_mem->free(b);
}

static void test_bad_play_args(void)
{
	struct mixer      *m = mixer_create(g_mem, 48000);
	struct sound_blob *b = mono_const(16, 1.0f, 48000);
	struct sound_blob *empty = make_blob(0, SOUND_CHANNELS_MONO, 48000);

	assert(mixer_play(m, NULL, 1.0f, 0.0f, 1.0f) == 0); /* no blob */
	assert(mixer_play(m, empty, 1.0f, 0.0f, 1.0f) == 0); /* zero frames */
	assert(mixer_play(m, b, 1.0f, 0.0f, 0.0f) == 0);     /* rate 0 */
	assert(mixer_play(m, b, 1.0f, 0.0f, -1.0f) == 0);    /* rate < 0 */
	assert(mixer_active(m) == 0);
	assert(mixer_create(NULL, 48000) == NULL);
	assert(mixer_create(g_mem, 0) == NULL);
	mixer_destroy(m);
	g_mem->free(b);
	g_mem->free(empty);
}

int main(void)
{
	mem_init();

	test_idle_is_silent();
	test_mono_centre_pan();
	test_mono_hard_left();
	test_gain_scales();
	test_stereo_passthrough();
	test_summing_is_unclipped();
	test_voice_retires_at_end();
	test_rate_is_pitch_and_duration();
	test_rate_interpolates();
	test_stop_silences();
	test_stale_handle_is_inert();
	test_voice_exhaustion();
	test_bad_play_args();

	printf("mixer tests passed\n");
	return 0;
}
