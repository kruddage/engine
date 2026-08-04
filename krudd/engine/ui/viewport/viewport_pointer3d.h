/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VIEWPORT_POINTER3D_H
#define VIEWPORT_POINTER3D_H

struct subsystem_manager;

/*
 * The world-space pointer's private seam onto viewport.c: one call, made from
 * the plugin entry, that resolves the services the pointer picks and draws
 * with and registers the "pointer3d" subsystem.
 *
 * Separate from viewport.c because the two jobs are separate: viewport.c is
 * the canvas bridge (aspect sync, click-to-pick through the kruddgui pointer)
 * and this is the bridge for a pointer that never touches the canvas. They
 * share the raycast — viewport_pick.c — and nothing else.
 */
void viewport_pointer3d_register(struct subsystem_manager *mgr);

#endif /* VIEWPORT_POINTER3D_H */
