/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUBSYSTEM_H
#define SUBSYSTEM_H

#include <stdint.h>

/*
 * A subsystem entry describes one engine module's lifecycle hooks.
 * tick may be NULL for modules that have no per-frame work.
 * Terminate the table with a zeroed entry (name == NULL).
 */
struct subsystem {
	const char *name;
	const void *api;       /* optional service vtable; NULL if unused */
	void (*init)(void);
	void (*tick)(void);
	void (*shutdown)(void);
	uint32_t wasm_size;    /* WASM module size in bytes; 0 if unknown */
};

void subsystem_init_all(const struct subsystem *table);
void subsystem_tick_all(const struct subsystem *table);
void subsystem_shutdown_all(const struct subsystem *table);

#endif /* SUBSYSTEM_H */
