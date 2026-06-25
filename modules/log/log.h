/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LOG_H
#define LOG_H

#include "log_api.h"

void log_init(void);
void log_shutdown(void);
void log_set_level(enum log_level level);
void log_write(enum log_level level, const char *fmt, ...);

#define LOG_DEBUG(fmt, ...) log_write(LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  log_write(LOG_LEVEL_INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  log_write(LOG_LEVEL_WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_write(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)

#endif /* LOG_H */
