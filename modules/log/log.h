/* SPDX-License-Identifier: MIT */
#ifndef LOG_H
#define LOG_H

enum log_level {
	LOG_DEBUG = 0,
	LOG_INFO,
	LOG_WARN,
	LOG_ERROR,
};

void log_init(void);
void log_shutdown(void);
void log_set_level(enum log_level level);
void log_write(enum log_level level, const char *fmt, ...);

#define log_debug(fmt, ...) log_write(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define log_info(fmt, ...)  log_write(LOG_INFO,  fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...)  log_write(LOG_WARN,  fmt, ##__VA_ARGS__)
#define log_error(fmt, ...) log_write(LOG_ERROR, fmt, ##__VA_ARGS__)

#endif /* LOG_H */
