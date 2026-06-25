/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef STATS_API_H
#define STATS_API_H

#include <stdint.h>

struct stats_api {
	float    last_frame_ms;
	float    fps_avg;
	uint32_t frame_count;
};

#endif /* STATS_API_H */
