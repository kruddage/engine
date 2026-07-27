/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sound_script — the sound scripting path end to end: boot the real s7 image
 * (which loads the embedded sound_script.scm), call sound_script_generate()
 * against a hand-authored (sound ...) source at two sample rates and both
 * channel counts, and check the returned sound_blob's header and samples. The
 * load-bearing property is sample-rate independence: the same script yields the
 * same amplitude at the same wall-clock time whether it bakes at 8k or 16k,
 * because sample is a pure function of the time t in seconds and never sees a
 * rate. The texture test's resolution independence, now for audio — plus the
 * channel count, which is the caller's choice, not the sound's.
 */
#include "sound_script.h"
#include "builtin_sound_scripts.h"

#include "script.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define EPS 1.0e-4f

static const struct memory_api test_mem_impl = {
	mem_alloc, mem_alloc_zero, mem_free,
	mem_pool_create, mem_pool_alloc, mem_pool_free, mem_pool_destroy,
};
static const struct memory_api *g_mem = &test_mem_impl;

/*
 * A ramp whose amplitude equals the time in seconds — the audio twin of the
 * texture test's hand-authored checker: a literal sample body reading its one
 * (duration ...) param through the bake, not through (param ...). Because the
 * value at frame i is exactly i/rate, every sample is a checkable number and the
 * same wall-clock time lands on the same amplitude at any rate. duration 0.5
 * keeps the ramp inside the [-1,1] clamp.
 */
static const char *RAMP_SRC =
	"(sound ramp\n"
	"  (params (duration float (edit range 0.02 4) (default 0.5)))\n"
	"  (sample (t) t))\n";

/* A stereo frame: left +0.5, right -0.5, constant — checks the (list L R) path
 * and that the two channels land where authored, not swapped or collapsed. Its
 * mono downmix is (0.5 + -0.5)/2 = 0, which the mono-bake test relies on. */
static const char *STEREO_SRC =
	"(sound stereo\n"
	"  (params (duration float (edit range 0.02 4) (default 0.25)))\n"
	"  (sample (t) (list 0.5 -0.5)))\n";

/* A one-element sample list: right must default to left (0.25), the audio twin
 * of a short pixel list defaulting its missing texture channels. */
static const char *SHORT_SRC =
	"(sound short\n"
	"  (params (duration float (edit range 0.02 4) (default 0.25)))\n"
	"  (sample (t) (list 0.25)))\n";

/* The interleaved float frame at index i (channels samples wide). */
static const float *frame(const struct sound_blob *b, uint32_t i)
{
	return sound_blob_samples(b) + (size_t)i * b->channels;
}

/* Frames a bake of DUR seconds at RATE produces: round(rate*dur), the same
 * half-up the driver computes, so the test predicts frame_count exactly. */
static uint32_t frames_for(uint32_t rate, float dur)
{
	return (uint32_t)((double)rate * (double)dur + 0.5);
}

static void test_hand_authored_ramp(void)
{
	struct sound_blob *b;
	uint32_t           size = 0, expect = frames_for(8000, 0.5f), i;

	b = sound_script_generate(RAMP_SRC, NULL, 0, 8000,
				  SOUND_CHANNELS_STEREO, g_mem, &size);
	assert(b != NULL);
	assert(size == sound_blob_size(expect, SOUND_CHANNELS_STEREO));
	assert(b->magic       == SOUND_BLOB_MAGIC);
	assert(b->frame_count == expect);
	assert(b->sample_rate == 8000);
	assert(b->channels    == SOUND_CHANNELS_STEREO);
	assert(b->format      == SOUND_FORMAT_F32);

	/* Every frame: value == i/rate, mono duplicated into both channels. */
	for (i = 0; i < b->frame_count; i++) {
		float t = (float)i / 8000.0f;
		assert(fabsf(frame(b, i)[0] - t) <= EPS);
		assert(fabsf(frame(b, i)[1] - t) <= EPS); /* L == R */
	}
	g_mem->free(b);
}

/*
 * The heart of rate independence: one source, two rates, one waveform. The
 * frame count scales with the rate, but the same wall-clock time (t = 0.1 s)
 * lands on the same amplitude — frame 800 at 8k and frame 1600 at 16k both read
 * 0.1, because sample is a pure function of seconds.
 */
static void test_rate_independence(void)
{
	struct sound_blob *lo, *hi;

	lo = sound_script_generate(RAMP_SRC, NULL, 0, 8000,
				   SOUND_CHANNELS_STEREO, g_mem, NULL);
	hi = sound_script_generate(RAMP_SRC, NULL, 0, 16000,
				   SOUND_CHANNELS_STEREO, g_mem, NULL);
	assert(lo != NULL && hi != NULL);
	assert(hi->frame_count == lo->frame_count * 2); /* 16k over 8k, same dur */

	/* t = 0.1 s: frame 800 at 8k, frame 1600 at 16k, both amplitude 0.1. */
	assert(fabsf(frame(lo, 800)[0] - 0.1f) <= EPS);
	assert(fabsf(frame(hi, 1600)[0] - 0.1f) <= EPS);
	assert(fabsf(frame(lo, 800)[0] - frame(hi, 1600)[0]) <= EPS);

	g_mem->free(lo);
	g_mem->free(hi);
}

static void test_stereo_and_short_frames(void)
{
	struct sound_blob *st, *sh;

	st = sound_script_generate(STEREO_SRC, NULL, 0, 8000,
				   SOUND_CHANNELS_STEREO, g_mem, NULL);
	sh = sound_script_generate(SHORT_SRC, NULL, 0, 8000,
				   SOUND_CHANNELS_STEREO, g_mem, NULL);
	assert(st != NULL && sh != NULL);

	/* Authored (list 0.5 -0.5): left and right land distinct, as written. */
	assert(fabsf(frame(st, 0)[0] - 0.5f) <= EPS);
	assert(fabsf(frame(st, 0)[1] + 0.5f) <= EPS);
	/* Authored (list 0.25): right defaults to left. */
	assert(fabsf(frame(sh, 0)[0] - 0.25f) <= EPS);
	assert(fabsf(frame(sh, 0)[1] - 0.25f) <= EPS);

	g_mem->free(st);
	g_mem->free(sh);
}

/*
 * Channel count is the caller's choice, not the sound's. A mono bake of the
 * ramp is one sample per frame (channels == 1, half the bytes), still valued
 * i/rate — proof the same source bakes either way and the header carries the
 * count the caller asked for.
 */
static void test_mono_bake(void)
{
	struct sound_blob *b;
	uint32_t           size = 0, expect = frames_for(8000, 0.5f), i;

	b = sound_script_generate(RAMP_SRC, NULL, 0, 8000,
				  SOUND_CHANNELS_MONO, g_mem, &size);
	assert(b != NULL);
	assert(b->channels    == SOUND_CHANNELS_MONO);
	assert(b->frame_count == expect);
	assert(size == sound_blob_size(expect, SOUND_CHANNELS_MONO));

	for (i = 0; i < b->frame_count; i++) {
		float t = (float)i / 8000.0f;
		assert(fabsf(frame(b, i)[0] - t) <= EPS); /* one sample / frame */
	}
	g_mem->free(b);
}

/*
 * A mono bake of a stereo-authored sound downmixes each frame to (L+R)/2, so a
 * spatialized point source keeps both sides rather than dropping one. The
 * stereo (0.5 -0.5) collapses to exactly 0.
 */
static void test_mono_downmix(void)
{
	struct sound_blob *b;

	b = sound_script_generate(STEREO_SRC, NULL, 0, 8000,
				  SOUND_CHANNELS_MONO, g_mem, NULL);
	assert(b != NULL);
	assert(b->channels == SOUND_CHANNELS_MONO);
	assert(fabsf(frame(b, 0)[0]) <= EPS); /* (0.5 + -0.5) / 2 == 0 */
	g_mem->free(b);
}

/*
 * The ramp declares a (params (duration ...)) clause. It must report one
 * tight-packed float field defaulting to 0.5 with a range edit hint — the same
 * shape script_texture_params reports, so one marshaller and one set of editor
 * widgets serve sounds too.
 */
static void test_duration_param_declared(void)
{
	struct shader_param p[8];
	uint32_t            total = 0;
	int                 n;

	n = script_sound_params(RAMP_SRC, p, 8, &total);
	assert(n == 1);
	assert(total == sizeof(float)); /* tight-packed */
	assert(strcmp(p[0].name, "duration") == 0);
	assert(p[0].components == 1 && p[0].offset == 0);
	assert(strcmp(p[0].edit, "range") == 0);
	assert(fabsf(p[0].edit_min - 0.02f) <= EPS);
	assert(fabsf(p[0].edit_max - 4.0f) <= EPS);
	assert(p[0].default_count == 1 &&
	       fabsf(p[0].edit_default[0] - 0.5f) <= EPS);
}

/*
 * The (duration ...) param sizes the bake: a duration override of 2.0 s makes a
 * clip four times the default 0.5 s, so the host reads the param, derives the
 * frame count, and the waveform (value == i/rate) is otherwise unchanged. Proof
 * the override reaches both the frame-count math and the sample scope.
 */
static void test_duration_override_resizes(void)
{
	const float        two[1] = { 2.0f };
	struct sound_blob *b;

	b = sound_script_generate(RAMP_SRC, (const uint8_t *)two, sizeof(two),
				  8000, SOUND_CHANNELS_STEREO, g_mem, NULL);
	assert(b != NULL);
	assert(b->frame_count == frames_for(8000, 2.0f));
	assert(fabsf(frame(b, 800)[0] - 0.1f) <= EPS); /* waveform unchanged */
	g_mem->free(b);
}

/*
 * A source that isn't a well-formed (sound ...) form, or whose sample body
 * faults, must not crash the caller — sound-script-generate's catch degrades to
 * #f, which the host marshals to "no sound" (NULL), never a partial blob. A
 * non-positive duration is likewise NULL, not a zero-frame allocation.
 */
static void test_malformed_source_yields_no_sound(void)
{
	const float neg[1] = { -1.0f };

	assert(sound_script_generate("(not-a-sound-form)", NULL, 0, 8000,
				     SOUND_CHANNELS_STEREO, g_mem, NULL) == NULL);
	assert(sound_script_generate(
		       "(sound broken (sample (t) (car '())))",
		       NULL, 0, 8000, SOUND_CHANNELS_STEREO, g_mem, NULL) == NULL);
	assert(sound_script_generate(NULL, NULL, 0, 8000,
				     SOUND_CHANNELS_STEREO, g_mem, NULL) == NULL);
	/* A negative duration override -> no frames -> NULL. */
	assert(sound_script_generate(RAMP_SRC, (const uint8_t *)neg, sizeof(neg),
				     8000, SOUND_CHANNELS_STEREO, g_mem, NULL)
	       == NULL);
}

/*
 * Every shipped built-in must bake cleanly at a real audio rate with no override
 * (defaults only), in both channel counts — beep, blip, and the value-noise
 * burst whose sin/floor hash is the most likely to fault. A valid blob whose
 * every sample sits in [-1,1] is the bar; the waveform itself is the author's
 * business.
 */
static void bake_builtin_ok(const char *src, uint32_t channels)
{
	struct sound_blob *b;
	const float       *s;
	uint32_t           i, n, size = 0;

	b = sound_script_generate(src, NULL, 0, 48000, channels, g_mem, &size);
	assert(b != NULL);
	assert(b->frame_count > 0);
	assert(b->channels == channels);
	assert(size == sound_blob_size(b->frame_count, channels));
	assert(b->sample_rate == 48000);
	s = sound_blob_samples(b);
	n = sound_blob_sample_count(b->frame_count, b->channels);
	for (i = 0; i < n; i++)
		assert(s[i] >= -1.0f && s[i] <= 1.0f); /* in range everywhere */
	g_mem->free(b);
}

static void test_builtins_bake(void)
{
	bake_builtin_ok(BEEP_SOUND_SCRIPT_SRC, SOUND_CHANNELS_STEREO);
	bake_builtin_ok(BLIP_SOUND_SCRIPT_SRC, SOUND_CHANNELS_STEREO);
	bake_builtin_ok(NOISE_BURST_SOUND_SCRIPT_SRC, SOUND_CHANNELS_STEREO);
	bake_builtin_ok(BEEP_SOUND_SCRIPT_SRC, SOUND_CHANNELS_MONO);
	bake_builtin_ok(BLIP_SOUND_SCRIPT_SRC, SOUND_CHANNELS_MONO);
	bake_builtin_ok(NOISE_BURST_SOUND_SCRIPT_SRC, SOUND_CHANNELS_MONO);
}

int main(void)
{
	mem_init();
	log_init();
	script_init(); /* loads the embedded sound_script.scm image */

	test_hand_authored_ramp();
	test_rate_independence();
	test_stereo_and_short_frames();
	test_mono_bake();
	test_mono_downmix();
	test_duration_param_declared();
	test_duration_override_resizes();
	test_malformed_source_yields_no_sound();
	test_builtins_bake();

	printf("sound_script tests passed\n");
	return 0;
}
