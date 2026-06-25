/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef LOG_API_H
#define LOG_API_H

/*
 * Plugin-facing log interface. Plugins obtain a pointer to this struct via
 * subsystem_manager_get_api(mgr, "log") and call through it — no direct
 * import of log_write from the main module.
 */

#include "log_level.h"

#include <stdarg.h>

struct log_api {
	void (*write)(enum log_level level, const char *fmt, ...);
};

#endif /* LOG_API_H */
