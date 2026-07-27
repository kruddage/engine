/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * audio_core — thread seam between the game and the audio device.
 *
 * The main thread bakes and enqueues; the audio thread drains and mixes. The
 * only shared mutable state is a single-producer / single-consumer ring of play
 * commands: the producer (main) writes a slot then releases the tail, the
 * consumer (audio) acquires the tail then reads — so a command is fully written
 * before it is visible, with no lock. Everything the consumer touches after
 * that (the mixer's voices, the immutable baked blobs) is single-threaded or
 * read-only, which is what makes running mixer_render from a real-time worklet
 * callback safe.
 */
#include "audio_core.h"

#include "mixer.h"

#include <stdatomic.h>
#include <stddef.h>

struct audio_cmd {
	const struct sound_blob *blob;
	float gain;
	float pan;
	float rate;
};

struct blob_cache_entry {
	uint32_t                 id;
	const struct sound_blob *blob;
};

struct audio_core {
	const struct memory_api *mem;
	struct mixer            *mixer;
	uint32_t                 sample_rate;
	audio_bake_fn            bake;
	void                    *bake_ctx;

	struct blob_cache_entry  cache[AUDIO_CORE_CACHE];
	uint32_t                 cache_n;

	/* SPSC ring. head advances on the audio thread (consumer), tail on the
	 * main thread (producer); a power-of-two capacity lets the mask wrap. */
	struct audio_cmd         ring[AUDIO_CORE_QUEUE];
	_Atomic uint32_t         head;
	_Atomic uint32_t         tail;
};

struct audio_core *audio_core_create(const struct memory_api *mem,
				     uint32_t sample_rate,
				     audio_bake_fn bake, void *bake_ctx)
{
	struct audio_core *c;

	if (!mem || sample_rate == 0)
		return NULL;
	c = mem->alloc_zero(sizeof(*c));
	if (!c)
		return NULL;
	c->mixer = mixer_create(mem, sample_rate);
	if (!c->mixer) {
		mem->free(c);
		return NULL;
	}
	c->mem         = mem;
	c->sample_rate = sample_rate;
	c->bake        = bake;
	c->bake_ctx    = bake_ctx;
	atomic_store_explicit(&c->head, 0, memory_order_relaxed);
	atomic_store_explicit(&c->tail, 0, memory_order_relaxed);
	return c;
}

void audio_core_destroy(struct audio_core *c)
{
	uint32_t i;

	if (!c)
		return;
	/* The core owns every blob it baked; free them through the same api. */
	for (i = 0; i < c->cache_n; i++)
		c->mem->free((void *)c->cache[i].blob);
	mixer_destroy(c->mixer);
	c->mem->free(c);
}

/* Main thread: the cached blob for ID, baking and caching it on first use.
 * Returns NULL when there is no baker, the bake fails, or the cache is full
 * (in which case the just-baked blob is freed rather than leaked). */
static const struct sound_blob *core_blob(struct audio_core *c, uint32_t id)
{
	struct sound_blob *baked;
	uint32_t           i;

	for (i = 0; i < c->cache_n; i++)
		if (c->cache[i].id == id)
			return c->cache[i].blob;

	if (!c->bake)
		return NULL;
	baked = c->bake(c->bake_ctx, id, c->sample_rate, SOUND_CHANNELS_MONO);
	if (!baked)
		return NULL;
	if (c->cache_n >= AUDIO_CORE_CACHE) {
		c->mem->free(baked);
		return NULL;
	}
	c->cache[c->cache_n].id   = id;
	c->cache[c->cache_n].blob = baked;
	c->cache_n++;
	return baked;
}

/* Main thread (producer): push one command, or fail if the ring is full. The
 * data store is ordered before the tail release so the consumer never reads a
 * half-written command. */
static int ring_push(struct audio_core *c, const struct sound_blob *b,
		     float gain, float pan, float rate)
{
	uint32_t tail = atomic_load_explicit(&c->tail, memory_order_relaxed);
	uint32_t head = atomic_load_explicit(&c->head, memory_order_acquire);

	if (tail - head >= AUDIO_CORE_QUEUE)
		return 0; /* full: drop this play */

	c->ring[tail & (AUDIO_CORE_QUEUE - 1u)].blob = b;
	c->ring[tail & (AUDIO_CORE_QUEUE - 1u)].gain = gain;
	c->ring[tail & (AUDIO_CORE_QUEUE - 1u)].pan  = pan;
	c->ring[tail & (AUDIO_CORE_QUEUE - 1u)].rate = rate;
	atomic_store_explicit(&c->tail, tail + 1u, memory_order_release);
	return 1;
}

int audio_core_play_blob(struct audio_core *c, const struct sound_blob *b,
			 float gain, float pan, float rate)
{
	if (!c || !b || b->frame_count == 0)
		return 0;
	return ring_push(c, b, gain, pan, rate);
}

int audio_core_play_asset(struct audio_core *c, uint32_t asset_id,
			  float gain, float pan, float rate)
{
	const struct sound_blob *b;

	if (!c)
		return 0;
	b = core_blob(c, asset_id);
	if (!b)
		return 0;
	return ring_push(c, b, gain, pan, rate);
}

void audio_core_render(struct audio_core *c, float *out, uint32_t frames)
{
	uint32_t head, tail;

	if (!c) {
		uint32_t i;

		if (out)
			for (i = 0; i < frames * 2u; i++)
				out[i] = 0.0f;
		return;
	}

	/* Consumer: drain every command published so far into the mixer. The
	 * acquire on tail pairs with the producer's release, so each command's
	 * fields are visible before we read them. */
	head = atomic_load_explicit(&c->head, memory_order_relaxed);
	tail = atomic_load_explicit(&c->tail, memory_order_acquire);
	while (head != tail) {
		const struct audio_cmd *cmd =
			&c->ring[head & (AUDIO_CORE_QUEUE - 1u)];

		mixer_play(c->mixer, cmd->blob, cmd->gain, cmd->pan, cmd->rate);
		head++;
	}
	atomic_store_explicit(&c->head, head, memory_order_release);

	mixer_render(c->mixer, out, frames);
}

uint32_t audio_core_active(const struct audio_core *c)
{
	return c ? mixer_active(c->mixer) : 0;
}
