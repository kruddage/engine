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

#include <stddef.h>

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
