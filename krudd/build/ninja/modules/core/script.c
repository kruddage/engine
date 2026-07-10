/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * script — the s7 Scheme runtime embedded in the engine.
 *
 * The interpreter is process-global for the session. script_init() starts it
 * and binds the engine primitives Scheme can call; script_eval() loads the
 * runtime image; script_tick() hands each frame to the image's (tick). The one
 * primitive today, (krudd-log level text), writes through the log subsystem —
 * proof that a value produced inside the Scheme image crosses back into the
 * engine's C services. More of the plugin ABI is meant to be exposed here.
 */
#include "script.h"

#include "log.h"

#include "s7.h"

#include "shader_scm.h"
#include "entity_script_scm.h"

#include <stddef.h>
#include <string.h>

static s7_scheme *g_s7;

/*
 * (krudd-log level text) -> unspecified. LEVEL is a log_level integer, TEXT the
 * message. Non-integer / non-string arguments degrade gracefully rather than
 * trap, so a malformed call in the image cannot take the frame down.
 */
static s7_pointer sp_krudd_log(s7_scheme *sc, s7_pointer args)
{
	s7_pointer     level = s7_car(args);
	s7_pointer     text  = s7_cadr(args);
	enum log_level lv    = LOG_LEVEL_INFO;

	if (s7_is_integer(level))
		lv = (enum log_level)s7_integer(level);
	if (s7_is_string(text))
		log_write(lv, "%s", s7_string(text));
	return s7_unspecified(sc);
}

void script_init(void)
{
	if (g_s7)
		return;
	g_s7 = s7_init();
	if (!g_s7) {
		log_write(LOG_LEVEL_ERROR, "script: s7 init failed");
		return;
	}
	s7_define_function(g_s7, "krudd-log", sp_krudd_log, 2, 0, false,
			   "(krudd-log level text) write text to the engine log");
	/*
	 * Load the shader DSL transpiler into the image so shader-transpile is
	 * defined wherever the interpreter is up — the runtime lowers shader
	 * assets to GLSL through it at bind time.
	 */
	script_eval(SHADER_SCM);
	/*
	 * Load the entity-script dispatcher: the (script ...) form and the
	 * per-entity tick. The entity plugin registers the entity-* host
	 * primitives its clauses call; those are only invoked at tick time, so
	 * loading this image before they exist is fine.
	 */
	script_eval(ENTITY_SCRIPT_SCM);
}

s7_scheme *script_s7(void)
{
	if (!g_s7)
		script_init();
	return g_s7;
}

/*
 * Evaluate every top-level form in SRC. s7_eval_c_string reads only the first
 * form, so an image with more than one definition would silently drop the
 * rest; read form-by-form until EOF instead, the way loading a file behaves.
 */
int script_eval(const char *src)
{
	s7_pointer port, eof, form;

	if (!g_s7 || !src)
		return -1;
	port = s7_open_input_string(g_s7, src);
	eof  = s7_eof_object(g_s7);
	while ((form = s7_read(g_s7, port)) != eof)
		s7_eval(g_s7, form, s7_nil(g_s7));
	s7_close_input_port(g_s7, port);
	return 0;
}

/*
 * Call the image's (shader-transpile SRC STAGE). The Scheme returns a GLSL
 * string or #f (no such stage); copy the string into one of two rotating
 * buffers so a caller can hold a vertex and a fragment result at once, and
 * return NULL for #f, a down interpreter, or an oversized result.
 */
const char *script_shader_transpile(const char *src, const char *stage)
{
	static char g_glsl[2][16384];
	static int  slot;
	s7_pointer  fn, res;
	const char *out;
	size_t      n;
	char       *buf;

	if (!g_s7 || !src || !stage)
		return NULL;
	fn = s7_name_to_value(g_s7, "shader-transpile");
	if (!s7_is_procedure(fn))
		return NULL;
	res = s7_call(g_s7, fn,
		      s7_list(g_s7, 2, s7_make_string(g_s7, src),
			      s7_make_string(g_s7, stage)));
	if (!s7_is_string(res))
		return NULL;
	out = s7_string(res);
	n   = strlen(out);
	buf = g_glsl[slot];
	if (n + 1 > sizeof(g_glsl[0]))
		return NULL;
	memcpy(buf, out, n + 1);
	slot = (slot + 1) & 1;
	return buf;
}

/* Copy a Scheme string field into a fixed C buffer, always NUL-terminated. */
static void copy_field(char *dst, size_t cap, s7_pointer s)
{
	const char *src = s7_is_string(s) ? s7_string(s) : "";
	size_t      n   = strlen(src);

	if (n >= cap)
		n = cap - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static uint32_t field_u32(s7_pointer v)
{
	return s7_is_integer(v) ? (uint32_t)s7_integer(v) : 0u;
}

static float field_real(s7_pointer v)
{
	return s7_is_number(v) ? (float)s7_number_to_real(g_s7, v) : 0.0f;
}

/*
 * Call the image's (shader-material-params SRC), which returns
 * (TOTAL-SIZE (NAME TYPE OFFSET SIZE COMPONENTS EDIT-KIND EDIT-MIN EDIT-MAX)
 * ...), and marshal it into the caller's shader_param array. The layout math
 * (std140 offsets/sizes) lives in Scheme; this only walks the result.
 */
int script_shader_material_params(const char *src, struct shader_param *out,
				  uint32_t max, uint32_t *total_size)
{
	s7_pointer fn, res, rest;
	uint32_t   count = 0;

	if (total_size)
		*total_size = 0;
	if (!g_s7 || !src)
		return -1;
	fn = s7_name_to_value(g_s7, "shader-material-params");
	if (!s7_is_procedure(fn))
		return -1;
	res = s7_call(g_s7, fn, s7_list(g_s7, 1, s7_make_string(g_s7, src)));
	if (!s7_is_pair(res))
		return -1;
	if (total_size)
		*total_size = field_u32(s7_car(res));

	for (rest = s7_cdr(res); s7_is_pair(rest) && (out ? count < max : 0);
	     rest = s7_cdr(rest)) {
		s7_pointer           f = s7_car(rest);
		struct shader_param *p = &out[count];

		if (!s7_is_pair(f))
			continue;
		copy_field(p->name, sizeof(p->name), s7_list_ref(g_s7, f, 0));
		copy_field(p->type, sizeof(p->type), s7_list_ref(g_s7, f, 1));
		p->offset     = field_u32(s7_list_ref(g_s7, f, 2));
		p->size       = field_u32(s7_list_ref(g_s7, f, 3));
		p->components = field_u32(s7_list_ref(g_s7, f, 4));
		copy_field(p->edit, sizeof(p->edit), s7_list_ref(g_s7, f, 5));
		p->edit_min   = field_real(s7_list_ref(g_s7, f, 6));
		p->edit_max   = field_real(s7_list_ref(g_s7, f, 7));
		count++;
	}
	return (int)count;
}

void script_tick(void)
{
	s7_pointer tick;

	if (!g_s7)
		return;
	tick = s7_name_to_value(g_s7, "tick");
	if (s7_is_procedure(tick))
		s7_call(g_s7, tick, s7_nil(g_s7));
}

void script_shutdown(void)
{
	if (!g_s7)
		return;
	s7_free(g_s7);
	g_s7 = NULL;
}
