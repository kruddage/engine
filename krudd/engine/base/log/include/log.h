/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LOG_H
#define LOG_H

#include "log_api.h"

void     log_init(void);
void     log_shutdown(void);
void     log_set_level(enum log_level level);
void     log_write(enum log_level level, const char *fmt, ...);
uint32_t log_get_history(struct log_message *out, uint32_t max);

#define LOG_DEBUG(...) log_write(LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  log_write(LOG_LEVEL_INFO,  __VA_ARGS__)
#define LOG_WARN(...)  log_write(LOG_LEVEL_WARN,  __VA_ARGS__)
#define LOG_ERROR(...) log_write(LOG_LEVEL_ERROR, __VA_ARGS__)

#endif /* LOG_H */
