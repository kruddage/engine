/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IMGUI_API_H
#define IMGUI_API_H

#define IMGUI_MAX_PANELS 16

struct imgui_api {
	void (*register_panel)(const char *name,
			       void (*draw_fn)(void *userdata),
			       void *userdata);
};

#endif /* IMGUI_API_H */
