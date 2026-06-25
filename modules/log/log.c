/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>

static enum log_level current_level = LOG_LEVEL_INFO;

static const char *level_prefix[] = {
	[LOG_LEVEL_DEBUG] = "DEBUG",
	[LOG_LEVEL_INFO]  = "INFO",
	[LOG_LEVEL_WARN]  = "WARN",
	[LOG_LEVEL_ERROR] = "ERROR",
};

void log_init(void)
{
	current_level = LOG_LEVEL_INFO;
}

void log_shutdown(void)
{
	fflush(stdout);
	fflush(stderr);
}

void log_set_level(enum log_level level)
{
	current_level = level;
}

void log_write(enum log_level level, const char *fmt, ...)
{
	FILE *out;
	va_list args;

	if (level < current_level)
		return;

	out = (level >= LOG_LEVEL_WARN) ? stderr : stdout;

	fprintf(out, "[%s] ", level_prefix[level]);
	va_start(args, fmt);
	vfprintf(out, fmt, args);
	va_end(args);
	fputc('\n', out);
}
