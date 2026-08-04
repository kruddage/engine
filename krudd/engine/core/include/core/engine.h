/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ENGINE_H
#define ENGINE_H

#include <stdint.h>

void engine_init(void);
void engine_tick(void);
void engine_shutdown(void);

#ifdef __EMSCRIPTEN__
/*
 * The loop handover (#991). engine.c defines these as EMSCRIPTEN_KEEPALIVE
 * exports for a host outside the module — the page, or a JS runtime embedding
 * it — and that is still their main caller, which is why they carry the
 * krudd_ prefix the module's other exports do rather than an engine_ one.
 *
 * They are declared here as well because a host can also be a module linked
 * into this same image: one that owns a frame source of its own and hands out
 * per-frame data only through its own callback, so the browser's rAF loop
 * must be out of the way for as long as it is running (#993 is the first).
 * Such a caller reaches them in C, and a header is where it should find them
 * rather than by re-declaring an extern of its own and hoping it matches.
 *
 * Browser-only, because emscripten_pause_main_loop() is: the native build has
 * no loop to suspend, and a native caller should fail to link rather than
 * silently do nothing.
 */
void krudd_suspend_loop(void);
void krudd_resume_loop(void);
void krudd_driven_tick(void);
#endif

#endif /* ENGINE_H */
