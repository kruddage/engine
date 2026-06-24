/* SPDX-License-Identifier: MIT */
#ifndef ENGINE_H
#define ENGINE_H

#include <stdint.h>

/*
 * engine_ping - proof-of-life; returns 1 so the host can confirm the
 * module loaded and the ABI resolves correctly.
 */
int32_t engine_ping(void);

#endif /* ENGINE_H */
