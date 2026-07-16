/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sound_script — host side of the sound scripting layer.
 *
 * Reads a bound ASSET_TYPE_SOUND asset's (duration ...) param to size the bake,
 * calls the shared s7 image's (sound-script-generate src params rate frames) —
 * see core/sound_script.scm — and copies its float-vector result (frame_count
 * interleaved stereo samples) into a sound_blob. Mirrors texture_script.c's
 * host/image seam: the synthesis lives in Scheme, this file only resolves the
 * param override, derives the frame count from the rate, and marshals the flat
 * buffer into the wire format (s7 float-vectors hold C doubles; the blob stores
 * float32, so the copy narrows each sample).
 */
#include "sound_script.h"

#include "script.h"

#include "s7.h"

#include <stddef.h>
#include <string.h>

#define SOUND_SCRIPT_MAX_PARAMS 32

/* The default of param component K, honouring an authored (default V ...) first
 * and otherwise the field's edit hint — the same resolution ts_param_default
 * makes for textures, so an un-overridden sound and the editor agree. */
static float ss_param_default(const struct shader_param *p, uint32_t k)
{
	if (k < p->default_count)
		return p->edit_default[k];
	if (strcmp(p->edit, "color") == 0)
		return 1.0f;
	if (strcmp(p->edit, "range") == 0)
		return p->edit_min;
	return 0.0f;
}

/*
 * Resolve sound SRC's declared params against its override BLOB (blen bytes, or
 * NULL for none) into an ((name . value) ...) alist — a real for a scalar param,
 * a list of reals for a vector. Each field reads from the override where present
 * and long enough, else from its default. This is the override ⊕ defaults the
 * sample clause sees through (param ...); it mirrors texture_script.c's
 * ts_param_values so a sound and a texture resolve params identically.
 */
static s7_pointer ss_param_values(s7_scheme *sc, const char *src,
				  const uint8_t *blob, uint32_t blen)
{
	struct shader_param p[SOUND_SCRIPT_MAX_PARAMS];
	int                 n, i;
	s7_pointer          out = s7_nil(sc);

	if (!src)
		return out;
	n = script_sound_params(src, p, SOUND_SCRIPT_MAX_PARAMS, NULL);
	for (i = n - 1; i >= 0; i--) {
		uint32_t   c = p[i].components > 4 ? 4 : p[i].components;
		float      v[4];
		uint32_t   k;
		s7_pointer val;

		for (k = 0; k < c; k++)
			v[k] = ss_param_default(&p[i], k);
		if (blob && p[i].offset + c * sizeof(float) <= blen)
			memcpy(v, blob + p[i].offset, c * sizeof(float));
		if (c == 1) {
			val = s7_make_real(sc, v[0]);
		} else {
			val = s7_nil(sc);
			for (k = c; k > 0; k--)
				val = s7_cons(sc, s7_make_real(sc, v[k - 1]),
					      val);
		}
		out = s7_cons(sc,
			      s7_cons(sc, s7_make_symbol(sc, p[i].name), val),
			      out);
	}
	return out;
}

/*
 * The sound's length in seconds: its (duration ...) param resolved against the
 * override BLOB (its default otherwise), or SOUND_SCRIPT_DEFAULT_DURATION when
 * the sound declares no such param. The bake reads only this scalar to size
 * itself — every other param the sample clause reads for itself in Scheme.
 */
static float ss_duration(const char *src, const uint8_t *blob, uint32_t blen)
{
	struct shader_param p[SOUND_SCRIPT_MAX_PARAMS];
	int                 n, i;

	if (!src)
		return SOUND_SCRIPT_DEFAULT_DURATION;
	n = script_sound_params(src, p, SOUND_SCRIPT_MAX_PARAMS, NULL);
	for (i = 0; i < n; i++) {
		if (strcmp(p[i].name, "duration") != 0)
			continue;
		if (blob && p[i].offset + sizeof(float) <= blen) {
			float v;
			memcpy(&v, blob + p[i].offset, sizeof(float));
			return v;
		}
		return ss_param_default(&p[i], 0);
	}
	return SOUND_SCRIPT_DEFAULT_DURATION;
}

struct sound_blob *sound_script_generate(const char *src,
					 const uint8_t *params,
					 uint32_t plen,
					 uint32_t sample_rate,
					 const struct memory_api *mem,
					 uint32_t *out_size)
{
	s7_scheme         *sc;
	s7_pointer         fn, res;
	const s7_double   *samples;
	float             *dst;
	float              duration;
	uint32_t           frames, sample_count, total, i;
	struct sound_blob *b;

	if (!src || !mem)
		return NULL;
	if (sample_rate == 0)
		sample_rate = SOUND_SCRIPT_DEFAULT_RATE;
	if (sample_rate < SOUND_SCRIPT_MIN_RATE)
		sample_rate = SOUND_SCRIPT_MIN_RATE;
	if (sample_rate > SOUND_SCRIPT_MAX_RATE)
		sample_rate = SOUND_SCRIPT_MAX_RATE;

	duration = ss_duration(src, params, plen);
	if (!(duration > 0.0f))
		return NULL;
	{
		double f = (double)sample_rate * (double)duration + 0.5;
		if (f < 1.0)
			return NULL;
		if (f > (double)SOUND_SCRIPT_MAX_FRAMES)
			f = (double)SOUND_SCRIPT_MAX_FRAMES;
		frames = (uint32_t)f;
	}

	sc = script_s7();
	if (!sc)
		return NULL;
	fn = s7_name_to_value(sc, "sound-script-generate");
	if (!s7_is_procedure(fn))
		return NULL;

	res = s7_call(sc, fn,
		      s7_list(sc, 4, s7_make_string(sc, src),
			      ss_param_values(sc, src, params, plen),
			      s7_make_integer(sc, sample_rate),
			      s7_make_integer(sc, frames)));
	if (!s7_is_float_vector(res))
		return NULL;

	sample_count = sound_blob_sample_count(frames, SOUND_BLOB_CHANNELS);
	if ((uint32_t)s7_vector_length(res) != sample_count)
		return NULL;

	total = sound_blob_size(frames, SOUND_BLOB_CHANNELS);
	b = mem->alloc(total);
	if (!b)
		return NULL;
	b->magic       = SOUND_BLOB_MAGIC;
	b->frame_count = frames;
	b->sample_rate = sample_rate;
	b->channels    = SOUND_BLOB_CHANNELS;
	b->format      = SOUND_FORMAT_F32;

	/* s7 float-vectors are C doubles; the blob is float32, so narrow each
	 * sample as it copies rather than memcpy the wrong width. */
	samples = s7_float_vector_elements(res);
	dst     = (float *)(void *)(b + 1);
	for (i = 0; i < sample_count; i++)
		dst[i] = (float)samples[i];

	if (out_size)
		*out_size = total;
	return b;
}
