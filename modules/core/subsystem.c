/* SPDX-License-Identifier: MIT */
#include "subsystem.h"

static int table_len(const struct subsystem *table)
{
	int n = 0;

	while (table[n].name)
		n++;
	return n;
}

void subsystem_init_all(const struct subsystem *table)
{
	int i;

	for (i = 0; table[i].name; i++) {
		if (table[i].init)
			table[i].init();
	}
}

void subsystem_tick_all(const struct subsystem *table)
{
	int i;

	for (i = 0; table[i].name; i++) {
		if (table[i].tick)
			table[i].tick();
	}
}

/* shutdown runs in reverse order — last initialized, first torn down */
void subsystem_shutdown_all(const struct subsystem *table)
{
	int i = table_len(table);

	while (i-- > 0) {
		if (table[i].shutdown)
			table[i].shutdown();
	}
}
