/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SCRIPT_H
#define SCRIPT_H

/*
 * script — the s7 Scheme runtime embedded in the engine.
 *
 * This is the seam where the engine's substrate inverts: the intent is for
 * Scheme, not C, to own the program over time. Today the runtime hosts a
 * single primitive (krudd-log, backed by the log subsystem) and a (tick) hook
 * the engine calls each frame; the engine ABI will grow more primitives here
 * so game and engine logic can move into the image. s7 is the same interpreter
 * krudd uses at build time, now linked into the runtime WASM as well.
 */

/* Start the interpreter and register the engine primitives. Idempotent. */
void script_init(void);

/* Evaluate a Scheme source string. Returns 0 on success, -1 if not started. */
int script_eval(const char *src);

/* Call the Scheme (tick) procedure if the image defines one. */
void script_tick(void);

/* Tear the interpreter down. */
void script_shutdown(void);

#endif /* SCRIPT_H */
