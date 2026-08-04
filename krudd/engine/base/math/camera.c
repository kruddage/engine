/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math/camera.h>

void camera_update(struct camera *cam)
{
	if (cam->supplied)
		return;

	mat4_look_at(&cam->view, cam->eye, cam->target, cam->up);
	mat4_perspective(&cam->proj, cam->fov_y, cam->aspect,
			 cam->near, cam->far);
	mat4_mul(&cam->view_proj, &cam->proj, &cam->view);
	cam->position[0] = cam->eye[0];
	cam->position[1] = cam->eye[1];
	cam->position[2] = cam->eye[2];
}

void camera_set_view_proj(struct camera *cam, const struct mat4 *view,
			  const struct mat4 *proj, const float eye[3])
{
	cam->view = *view;
	cam->proj = *proj;
	mat4_mul(&cam->view_proj, &cam->proj, &cam->view);
	cam->position[0] = eye[0];
	cam->position[1] = eye[1];
	cam->position[2] = eye[2];
	cam->supplied = 1;
}

void camera_clear_view_proj(struct camera *cam)
{
	cam->supplied = 0;
}
