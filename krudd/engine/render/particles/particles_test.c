/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "particles.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/*
 * The simulation is pure CPU state, so it tests without a device: burst fills
 * the pool, update ages particles out, and the cap holds under an overflowing
 * burst. The render path (billboard build + cmd_draw) is exercised through the
 * scene_renderer test against the null backend, not here.
 */

static void test_burst_and_expire(void)
{
	const float pos[3] = { 0.0f, 0.0f, 0.0f };
	const float rgb[3] = { 1.0f, 0.2f, 0.2f };
	int         i;

	assert(particles_live_count() == 0);

	particles_burst(pos, rgb, 40);
	assert(particles_live_count() == 40);

	/* A zero/negative dt advances nothing. */
	particles_update(0.0f);
	assert(particles_live_count() == 40);

	/* Lifetimes are all under a second (PARTICLE_LIFE_MAX), so a couple of
	 * seconds of stepping ages every particle out. */
	for (i = 0; i < 200; i++)
		particles_update(1.0f / 60.0f);
	assert(particles_live_count() == 0);
}

static void test_cap_is_bounded(void)
{
	const float pos[3] = { 1.0f, 2.0f, 3.0f };
	const float rgb[3] = { 0.2f, 0.4f, 1.0f };
	uint32_t    n;
	int         i;

	/* Drain anything left from the previous test. */
	for (i = 0; i < 300; i++)
		particles_update(1.0f / 60.0f);
	assert(particles_live_count() == 0);

	/* Ask for far more than the pool holds; the count must clamp, not grow
	 * past the cap or wrap. */
	for (i = 0; i < 100; i++)
		particles_burst(pos, rgb, 1000);
	n = particles_live_count();
	assert(n > 0);
	assert(n <= 512); /* PARTICLE_MAX */

	/* A burst with no room left adds nothing and does not corrupt the count. */
	particles_burst(pos, rgb, 1000);
	assert(particles_live_count() == n);
}

int main(void)
{
	test_burst_and_expire();
	test_cap_is_bounded();

	printf("particles tests passed\n");
	return 0;
}
