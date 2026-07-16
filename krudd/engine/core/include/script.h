/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SCRIPT_H
#define SCRIPT_H

#include <stdint.h>

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

/*
 * One editable parameter of a source-declared parameter block — a shader's
 * Material uniform block (script_shader_material_params) or a script's params
 * clause (script_entity_params); both report this same shape. `components` is
 * how many floats the editor exposes (float=1 .. vec4=4; 0 for a type it does
 * not edit as scalars, e.g. a matrix). `edit` is the field's authored hint:
 * "none", "color", or "range" (with edit_min/edit_max meaningful only for
 * "range"). `offset`/`size` are bytes within the block; the packing is the
 * source's — std140 for a shader, tight for a script.
 *
 * `edit_default[]`/`default_count` carry an optional authored default: a field
 * may declare `(default V ...)` to seed its un-overridden value independently of
 * its edit hint. default_count is how many leading components it supplies (0 =
 * none, falling back to the edit-hint default); components past it fall back too.
 */
struct shader_param {
	char     name[64];
	char     type[16];
	uint32_t offset;      /* byte offset within the block (packing per source) */
	uint32_t size;        /* bytes the field consumes                          */
	uint32_t components;
	char     edit[16];
	float    edit_min;
	float    edit_max;
	float    edit_default[4];
	uint32_t default_count;
};

/*
 * Introspect a shader's Material uniform block as editable parameters. Fills
 * out[] with up to `max` fields (in declaration order), writes the std140-packed
 * block size to *total_size (may be NULL), and returns the field count (>= 0).
 * Returns -1 when the interpreter is down, the query proc is missing, or SRC is
 * not a usable shader form. A shader with no Material block yields 0 fields and
 * total_size 0 — not an error.
 */
int script_shader_material_params(const char *src, struct shader_param *out,
				  uint32_t max, uint32_t *total_size);

/*
 * Introspect an entity script's (params ...) clause as editable parameters, the
 * CPU-side twin of script_shader_material_params. Fills out[] with up to `max`
 * fields (in declaration order), writes the tight-packed block size to
 * *total_size (may be NULL), and returns the field count (>= 0), or -1 on the
 * same failure conditions. A script with no params yields 0 fields — not an
 * error. SRC is the (script ...) source.
 */
int script_entity_params(const char *src, struct shader_param *out,
			 uint32_t max, uint32_t *total_size);

/*
 * Introspect a mesh script's (params ...) clause as editable parameters, the
 * geometry-side twin of script_entity_params: same tight packing, same reported
 * shape, so one marshaller and one set of editor widgets serve meshes too.
 * Fills out[] with up to `max` fields (in declaration order), writes the
 * tight-packed block size to *total_size (may be NULL), and returns the field
 * count (>= 0), or -1 on the same failure conditions. A mesh with no params
 * yields 0 fields — not an error. SRC is the (mesh ...) source.
 */
int script_mesh_params(const char *src, struct shader_param *out,
		       uint32_t max, uint32_t *total_size);

/*
 * Introspect a texture script's (params ...) clause as editable parameters, the
 * pixel-side twin of script_mesh_params: same tight packing, same reported
 * shape, so one marshaller and one set of editor widgets serve textures too.
 * Fills out[] with up to `max` fields (in declaration order), writes the
 * tight-packed block size to *total_size (may be NULL), and returns the field
 * count (>= 0), or -1 on the same failure conditions. A texture with no params
 * yields 0 fields — not an error. SRC is the (texture ...) source.
 */
int script_texture_params(const char *src, struct shader_param *out,
			  uint32_t max, uint32_t *total_size);

/*
 * Introspect a sound script's (params ...) clause as editable parameters, the
 * audio-side twin of script_texture_params: same tight packing, same reported
 * shape, so one marshaller and one set of editor widgets serve sounds too. The
 * sound_script driver also reads the (duration ...) field through this to size
 * a bake. Fills out[] with up to `max` fields (in declaration order), writes the
 * tight-packed block size to *total_size (may be NULL), and returns the field
 * count (>= 0), or -1 on the same failure conditions. A sound with no params
 * yields 0 fields — not an error. SRC is the (sound ...) source.
 */
int script_sound_params(const char *src, struct shader_param *out,
			uint32_t max, uint32_t *total_size);

/* Call the Scheme (tick) procedure if the image defines one. */
void script_tick(void);

/* Tear the interpreter down. */
void script_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SCRIPT_H */
