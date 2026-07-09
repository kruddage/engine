/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SCRIPT_H
#define SCRIPT_H

#ifdef __cplusplus
extern "C" {
#endif

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

/*
 * The interpreter handle, starting it on first use. A C shim that hands work
 * to a Scheme image — evaluate its source with script_eval, then call a
 * procedure it defines and walk the result — needs the raw s7 pointer for the
 * s7_call. Returns NULL only if the interpreter failed to start.
 */
struct s7_scheme *script_s7(void);

/* Evaluate a Scheme source string. Returns 0 on success, -1 if not started. */
int script_eval(const char *src);

/*
 * Transpile a krudd shader DSL to GLSL for one stage ("vertex"/"fragment").
 * SRC is the (shader ...) source; the result is GLSL ES 3.00 text valid until
 * the next call (a rotating internal buffer), or NULL when the interpreter is
 * down, the shader has no such stage, or the source is too large. The renderer
 * calls this when it binds a shader; a NULL for a stage it needs is the error.
 */
const char *script_shader_transpile(const char *src, const char *stage);

/* Call the Scheme (tick) procedure if the image defines one. */
void script_tick(void);

/* Tear the interpreter down. */
void script_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SCRIPT_H */
