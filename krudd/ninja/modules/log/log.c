/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "log.h"
#include "ring_buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static enum log_level current_level = LOG_LEVEL_INFO;

static const char *level_prefix[] = {
	[LOG_LEVEL_DEBUG] = "DEBUG",
	[LOG_LEVEL_INFO]  = "INFO",
	[LOG_LEVEL_WARN]  = "WARN",
	[LOG_LEVEL_ERROR] = "ERROR",
};

static struct log_message history_storage[LOG_HISTORY_CAP];
static struct ring_buf    history;

void log_init(void)
{
	current_level = LOG_LEVEL_INFO;
	ring_buf_init(&history, history_storage,
		      sizeof(*history_storage), LOG_HISTORY_CAP);
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
	struct log_message msg;
	FILE              *out;
	va_list            args;

	if (level < current_level)
		return;

	va_start(args, fmt);
	vsnprintf(msg.text, sizeof(msg.text), fmt, args);
	va_end(args);

	msg.level = level;
	ring_buf_push(&history, &msg);

	out = (level >= LOG_LEVEL_WARN) ? stderr : stdout;
	fprintf(out, "[%s] %s\n", level_prefix[level], msg.text);
}

uint32_t log_get_history(struct log_message *out, uint32_t max)
{
	uint32_t count;
	uint32_t i;

	count = (uint32_t)ring_buf_len(&history);
	if (count > max)
		count = max;
	for (i = 0; i < count; i++)
		ring_buf_peek(&history, (size_t)i, &out[i]);
	return count;
}
