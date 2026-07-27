/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * texture_registry — the id algebra behind the WebGPU backend's texture-handle.
 *
 * The case worth the test is the last one: a slot reused after its occupant was
 * forgotten must not honour ids minted for the old occupant. That failure is
 * silent — the UI composites a real texture, just the wrong one — so nothing
 * downstream would report it.
 */
#include "texture_registry.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
	/* Stand-ins for gpu_texture pointers; only their addresses matter. */
	int a, b, c;
	uint32_t ida, idb, idc;
	uint32_t i;

	texreg_reset();

	/* An id names its object, and interning is idempotent. */
	ida = texreg_intern(&a);
	idb = texreg_intern(&b);
	assert(ida != 0 && idb != 0);
	assert(ida != idb);
	assert(texreg_intern(&a) == ida); /* stable across repeat calls */
	assert(texreg_resolve(ida) == &a);
	assert(texreg_resolve(idb) == &b);

	/* 0 is never a valid id, in either direction. */
	assert(texreg_intern(NULL) == 0);
	assert(texreg_resolve(0) == NULL);

	/* A malformed or out-of-range id resolves to nothing rather than
	 * indexing off the end of the table. */
	assert(texreg_resolve(0xffffffffu) == NULL);
	assert(texreg_resolve(TEXREG_CAPACITY + 1u) == NULL);

	/* A forgotten object's id goes dead; its neighbours are untouched. */
	texreg_forget(&a);
	assert(texreg_resolve(ida) == NULL);
	assert(texreg_resolve(idb) == &b);

	/* Forgetting something never interned is a no-op, not a corruption. */
	texreg_forget(&c);
	assert(texreg_resolve(idb) == &b);

	/*
	 * THE ONE THAT MATTERS: the freed slot gets reused. The new occupant
	 * gets a working id, and the old id stays dead — it must not resolve to
	 * the newcomer just because it landed in the same slot.
	 */
	idc = texreg_intern(&c);
	assert(idc != 0);
	assert(idc != ida);              /* generation moved it on */
	assert(texreg_resolve(idc) == &c);
	assert(texreg_resolve(ida) == NULL); /* the stale id is still dead */

	/*
	 * A full table returns 0 rather than evicting a live entry — the caller
	 * loses its picture, which beats corrupting someone else's id.
	 */
	texreg_reset();
	{
		static int filler[TEXREG_CAPACITY];
		int overflow;

		for (i = 0; i < TEXREG_CAPACITY; i++)
			assert(texreg_intern(&filler[i]) != 0);
		assert(texreg_intern(&overflow) == 0);

		/* Everything already interned still resolves. */
		for (i = 0; i < TEXREG_CAPACITY; i++)
			assert(texreg_resolve(texreg_intern(&filler[i]))
			       == &filler[i]);

		/* Free one and the table takes a new object again. */
		texreg_forget(&filler[0]);
		assert(texreg_intern(&overflow) != 0);
	}

	printf("texture_registry tests passed\n");
	return 0;
}
