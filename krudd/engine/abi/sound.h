/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SOUND_H
#define SOUND_H

#include <stddef.h>
#include <stdint.h>

/*
 * On-the-wire sound blob. A sound is a run of PCM audio, but the asset catalog
 * delivers a single opaque byte buffer, so we pack the layout and the samples
 * behind this header: the header, then frame_count * channels float32 samples,
 * interleaved (L R L R ... for stereo). Native and WASM share endianness
 * (little) and float32 layout, so the bytes travel verbatim — the texture_blob
 * story for pixels, now for audio.
 *
 * A sound is baked from a (sound NAME (sample (t) ...)) Scheme script (see
 * core/sound_script.scm); the sample clause is a pure function of the time t in
 * seconds, so the same script bakes the same waveform at any sample rate.
 * sample_rate and frame_count are an output setting the baker supplies — the
 * host picks the rate (its audio context's) and derives the frame count from
 * the sound's (duration ...) param — never baked into the script itself.
 *
 * Float32 is the only stored format: it is what Web Audio's AudioBuffer and a
 * float mixer consume, so a baked blob plays with no per-sample conversion.
 *
 * channels is 1 (mono) or 2 (stereo interleaved, L R per frame) — the count the
 * baker was asked for, not a property of the sound. The same (sound ...) source
 * bakes either way: a mono request downmixes a stereo (list L R) frame to
 * (L+R)/2, a stereo request duplicates a mono (real) frame into both channels
 * (as a short pixel list defaults its missing texture channels). The caller
 * picks per use — a spatialized point source bakes mono because Web Audio's
 * PannerNode only accepts mono input, while a UI or music clip bakes stereo.
 */
#define SOUND_BLOB_MAGIC 0x30444e53u /* 'SND0' sentinel, not a version */

/* format discriminator; 0 = F32 interleaved (the only kind today). */
#define SOUND_FORMAT_F32 0u

/* Interleave widths a blob may store. */
#define SOUND_CHANNELS_MONO   1u
#define SOUND_CHANNELS_STEREO 2u

struct sound_blob {
	uint32_t magic;       /* SOUND_BLOB_MAGIC */
	uint32_t frame_count; /* frames (samples per channel) */
	uint32_t sample_rate; /* Hz, e.g. 48000 */
	uint32_t channels;    /* interleave width; 1 (mono) or 2 (stereo) */
	uint32_t format;      /* sound_format; 0 = F32 interleaved */
};

/* Borrow the interleaved float sample array packed after the header. */
static inline const float *sound_blob_samples(const struct sound_blob *b)
{
	return (const float *)(const void *)(b + 1);
}

/* Count of interleaved float samples for the given frames/channels. */
static inline uint32_t sound_blob_sample_count(uint32_t frame_count,
					       uint32_t channels)
{
	return (uint32_t)((size_t)frame_count * channels);
}

/* Byte size of the interleaved sample array for the given frames/channels. */
static inline uint32_t sound_blob_samples_size(uint32_t frame_count,
					       uint32_t channels)
{
	return (uint32_t)((size_t)frame_count * channels * sizeof(float));
}

/* Total byte size of a blob holding the given frames/channels of float PCM. */
static inline uint32_t sound_blob_size(uint32_t frame_count, uint32_t channels)
{
	return (uint32_t)(sizeof(struct sound_blob)
			  + sound_blob_samples_size(frame_count, channels));
}

#endif /* SOUND_H */
