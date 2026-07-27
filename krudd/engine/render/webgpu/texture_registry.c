/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "texture_registry.h"

#include <stddef.h>

struct texreg_slot {
	void    *obj; /* NULL when free */
	uint16_t gen;
};

static struct texreg_slot g_slots[TEXREG_CAPACITY];

static uint32_t slot_to_id(uint32_t slot, uint16_t gen)
{
	return ((uint32_t)gen << 16) | (slot + 1u);
}

uint32_t texreg_intern(void *obj)
{
	uint32_t i;
	uint32_t free_slot = TEXREG_CAPACITY;

	if (!obj)
		return 0;

	for (i = 0; i < TEXREG_CAPACITY; i++) {
		if (g_slots[i].obj == obj)
			return slot_to_id(i, g_slots[i].gen);
		if (!g_slots[i].obj && free_slot == TEXREG_CAPACITY)
			free_slot = i;
	}

	if (free_slot == TEXREG_CAPACITY)
		return 0;

	g_slots[free_slot].obj = obj;
	return slot_to_id(free_slot, g_slots[free_slot].gen);
}

void *texreg_resolve(uint32_t id)
{
	uint32_t slot;
	uint16_t gen;

	if (id == 0)
		return NULL;
	slot = (id & 0xffffu) - 1u;
	gen  = (uint16_t)(id >> 16);
	if (slot >= TEXREG_CAPACITY)
		return NULL;
	if (g_slots[slot].gen != gen)
		return NULL;
	return g_slots[slot].obj;
}

void texreg_forget(void *obj)
{
	uint32_t i;

	if (!obj)
		return;
	for (i = 0; i < TEXREG_CAPACITY; i++) {
		if (g_slots[i].obj == obj) {
			g_slots[i].obj = NULL;
			/*
			 * Bump on release, not on reuse. The slot may sit free
			 * for a long time, and every id naming the old occupant
			 * has to be dead for all of it — not just from whenever
			 * something next claims the slot.
			 */
			g_slots[i].gen++;
			return;
		}
	}
}

void texreg_reset(void)
{
	uint32_t i;

	for (i = 0; i < TEXREG_CAPACITY; i++) {
		g_slots[i].obj = NULL;
		g_slots[i].gen = 0;
	}
}
