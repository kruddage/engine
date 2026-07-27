/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mixer — sum active voices over pre-baked sound_blobs into a stereo buffer.
 *
 * A voice holds a borrowed blob, a fractional playhead, and the per-instance
 * gain/pan/step that live playback varies. mixer_render walks each output frame,
 * linearly interpolates each voice's blob at its playhead (so a non-native rate
 * resamples cleanly rather than zippering), applies the voice's channel gains,
 * accumulates, and advances the playhead — retiring the voice when it runs off
 * the end. No allocation, no Scheme, no locks: this is the arithmetic a
 * real-time audio callback can run.
 */
#include "mixer.h"

#include <math.h>
#include <stddef.h>

/* A single playing instance of a blob. */
struct voice {
	const struct sound_blob *blob;
	double   pos;    /* playhead, in source frames */
	double   step;   /* source frames advanced per output frame */
	float    gain_l; /* left  gain (folds in pan for a mono blob) */
	float    gain_r; /* right gain */
	uint32_t gen;    /* generation, for stale-handle rejection */
	int      active;
};

struct mixer {
	const struct memory_api *mem;
	uint32_t     sample_rate;
	struct voice voices[MIXER_MAX_VOICES];
};

/* Split a handle into a slot index and the generation it was minted with. The
 * low 8 bits are the index (MIXER_MAX_VOICES fits), the rest the generation;
 * generation starts at 1 so a valid handle is never 0. */
#define MIXER_HANDLE(index, gen) (((uint32_t)(gen) << 8) | (uint32_t)(index))
#define MIXER_HANDLE_INDEX(h)    ((h) & 0xffu)
#define MIXER_HANDLE_GEN(h)      ((h) >> 8)

static float clampf(float x, float lo, float hi)
{
	if (x < lo)
		return lo;
	if (x > hi)
		return hi;
	return x;
}

struct mixer *mixer_create(const struct memory_api *mem, uint32_t sample_rate)
{
	struct mixer *m;

	if (!mem || sample_rate == 0)
		return NULL;
	m = mem->alloc_zero(sizeof(*m));
	if (!m)
		return NULL;
	m->mem         = mem;
	m->sample_rate = sample_rate;
	return m;
}

void mixer_destroy(struct mixer *m)
{
	if (m)
		m->mem->free(m);
}

uint32_t mixer_play(struct mixer *m, const struct sound_blob *b,
		    float gain, float pan, float rate)
{
	uint32_t i;

	if (!m || !b || b->frame_count == 0 || !(rate > 0.0f))
		return 0;

	for (i = 0; i < MIXER_MAX_VOICES; i++) {
		struct voice *v = &m->voices[i];

		if (v->active)
			continue;

		v->blob = b;
		v->pos  = 0.0;
		/* Advance by the rate multiplier, corrected for any mismatch
		 * between the blob's baked rate and our output rate, so pitch and
		 * duration stay right when they differ. */
		v->step = (double)rate * (double)b->sample_rate
			  / (double)m->sample_rate;

		gain = clampf(gain, 0.0f, 1.0f);
		if (b->channels == SOUND_CHANNELS_MONO) {
			/* Constant-power pan: -1 -> hard left, +1 -> hard right,
			 * 0 -> both at 1/sqrt(2), so a swept source holds even
			 * loudness across the field. */
			float ang = (clampf(pan, -1.0f, 1.0f) + 1.0f)
				    * 0.25f * (float)M_PI;
			v->gain_l = gain * cosf(ang);
			v->gain_r = gain * sinf(ang);
		} else {
			/* Stereo blob: channels play straight, pan ignored. */
			v->gain_l = gain;
			v->gain_r = gain;
		}

		v->gen = v->gen + 1;
		if (v->gen == 0)
			v->gen = 1;
		v->active = 1;
		return MIXER_HANDLE(i, v->gen);
	}
	return 0; /* all voices busy */
}

void mixer_stop(struct mixer *m, uint32_t voice)
{
	uint32_t index, gen;

	if (!m || voice == 0)
		return;
	index = MIXER_HANDLE_INDEX(voice);
	gen   = MIXER_HANDLE_GEN(voice);
	if (index >= MIXER_MAX_VOICES)
		return;
	if (m->voices[index].active && m->voices[index].gen == gen)
		m->voices[index].active = 0;
}

uint32_t mixer_active(const struct mixer *m)
{
	uint32_t i, n = 0;

	if (!m)
		return 0;
	for (i = 0; i < MIXER_MAX_VOICES; i++)
		if (m->voices[i].active)
			n++;
	return n;
}

/* Linearly interpolate channel CH of BLOB at fractional playhead POS. Past the
 * last frame it holds the final sample (the caller retires the voice). */
static float sample_at(const struct sound_blob *b, double pos, uint32_t ch)
{
	const float *s  = sound_blob_samples(b);
	uint32_t     c  = b->channels;
	uint32_t     n  = b->frame_count;
	uint32_t     i0 = (uint32_t)pos;
	float        a, next, frac;

	a = s[(size_t)i0 * c + ch];
	if (i0 + 1 >= n)
		return a;
	next = s[(size_t)(i0 + 1) * c + ch];
	frac = (float)(pos - (double)i0);
	return a + (next - a) * frac;
}

void mixer_render(struct mixer *m, float *out, uint32_t frames)
{
	uint32_t f, i;

	if (!out || frames == 0)
		return;
	for (i = 0; i < frames * 2u; i++)
		out[i] = 0.0f;
	if (!m)
		return;

	for (i = 0; i < MIXER_MAX_VOICES; i++) {
		struct voice *v = &m->voices[i];
		int           mono;

		if (!v->active)
			continue;
		mono = (v->blob->channels == SOUND_CHANNELS_MONO);

		for (f = 0; f < frames; f++) {
			float l, r;

			if (v->pos >= (double)v->blob->frame_count) {
				v->active = 0;
				break;
			}
			if (mono) {
				float s = sample_at(v->blob, v->pos, 0);
				l = s * v->gain_l;
				r = s * v->gain_r;
			} else {
				l = sample_at(v->blob, v->pos, 0) * v->gain_l;
				r = sample_at(v->blob, v->pos, 1) * v->gain_r;
			}
			out[f * 2u]      += l;
			out[f * 2u + 1u] += r;
			v->pos += v->step;
		}
	}
}
