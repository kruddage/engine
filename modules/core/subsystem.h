/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef SUBSYSTEM_H
#define SUBSYSTEM_H

/*
 * A subsystem entry describes one engine module's lifecycle hooks.
 * tick may be NULL for modules that have no per-frame work.
 * Terminate the table with a zeroed entry (name == NULL).
 */
struct subsystem {
	const char *name;
	const void *api;      /* optional service vtable; NULL if unused */
	void (*init)(void);
	void (*tick)(void);
	void (*shutdown)(void);
};

void subsystem_init_all(const struct subsystem *table);
void subsystem_tick_all(const struct subsystem *table);
void subsystem_shutdown_all(const struct subsystem *table);

#endif /* SUBSYSTEM_H */
