/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LOG_API_H
#define LOG_API_H

/*
 * Plugin-facing log interface. Plugins obtain a pointer to this struct via
 * subsystem_manager_get_api(mgr, "log") and call through it — no direct
 * import of log_write from the main module.
 */

#include "log_level.h"

#include <stdarg.h>
#include <stdint.h>

#define LOG_HISTORY_CAP     256
#define LOG_MESSAGE_MAX_LEN 256

struct log_message {
	enum log_level level;
	char           text[LOG_MESSAGE_MAX_LEN];
};

struct log_api {
	void     (*write)(enum log_level level, const char *fmt, ...);
	uint32_t (*get_history)(struct log_message *out, uint32_t max);
};

#endif /* LOG_API_H */
