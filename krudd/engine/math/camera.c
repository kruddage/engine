/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "camera.h"

void camera_update(struct camera *cam)
{
	struct mat4 view, proj;

	mat4_look_at(&view, cam->eye, cam->target, cam->up);
	mat4_perspective(&proj, cam->fov_y, cam->aspect,
			 cam->near, cam->far);
	mat4_mul(&cam->view_proj, &proj, &view);
}
