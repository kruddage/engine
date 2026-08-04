/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef POINTER3D_H
#define POINTER3D_H

#include <stdint.h>

/*
 * pointer3d — the world-space pointer, and the "pointer3d" subsystem api a
 * game reads it through (#996).
 *
 * kruddgui's pointer is a pixel: one position on a canvas, with a one-frame
 * click edge, which viewport unprojects through the camera to reach the scene.
 * A pointer held IN the world skips that step — it is already an origin and a
 * direction in metres — and there was nowhere in the engine for such a thing
 * to be said. This is that place.
 *
 * Two halves, deliberately facing opposite ways:
 *
 *   the PUSH side   viewport_pointer3d_publish(), called once per frame by
 *                   whatever host has real pointers. Today the only host is
 *                   the XR module's controller reader, which sits ABOVE ui/
 *                   in the tier order and therefore calls DOWN into this —
 *                   ui/ never links, includes or names xr. Nothing in this
 *                   header mentions a headset, a session or a controller,
 *                   which is the initiative's rule (#987) and not an
 *                   accident: a desktop 6-DoF mouse or a scripted test rig
 *                   would publish through exactly this call.
 *
 *   the READ side   the "pointer3d" subsystem api below, resolved the way
 *                   "camera" and "scene" are:
 *
 *                     const struct pointer3d_api *p =
 *                             subsystem_manager_get_api(mgr, "pointer3d");
 *
 *                   A page with no spatial pointer registers it all the same
 *                   and answers count() == 0, so a consumer reads a number
 *                   rather than guarding a NULL api it will nearly always get.
 *
 * Between the two, ui/viewport does with a published ray exactly what it does
 * with a click on the canvas: raycast it against the entity meshes on the
 * click edge and hand the hit to the scene's shared selection, which is how a
 * click becomes a move in a loaded game. A game that only wants "what did the
 * player just point at and press" therefore needs nothing from this header at
 * all — its (on-selected ...) hook already fires. This is for the game that
 * wants the pointer itself.
 */

/*
 * How many world-space pointers a frame can carry. Two, because two hands is
 * what the first host reports and what a player has; the count is read per
 * frame rather than assumed, so zero and one are ordinary answers and not
 * error cases — a player putting one controller down is normal.
 */
#define POINTER3D_MAX 2

/*
 * Which hand a pointer is held in, when the host knows. NONE is a real
 * answer — a tracked pointer that is not handed, or a host that does not say —
 * so a consumer that keys off this must handle it rather than assume a side.
 */
enum pointer3d_hand {
	POINTER3D_HAND_NONE  = 0,
	POINTER3D_HAND_LEFT  = 1,
	POINTER3D_HAND_RIGHT = 2
};

/*
 * One world-space pointer for one frame.
 *
 * origin and dir are the engine's world space, in metres, already composed
 * onto wherever the scene put the viewer — a host that reports poses in some
 * room-local space of its own does that composition before publishing, so a
 * consumer here never has to know one existed. dir is unit length.
 *
 * click is a ONE-FRAME EDGE, not a held button: it is 1 on the frame a press
 * began and 0 on every other frame, including the rest of the same press. That
 * is the shape kruddgui's pointer_clicked() has and the shape every existing
 * consumer reads, and it is the publisher's job to keep — see the note on
 * exactly-once in the XR module's input reader.
 *
 * hit is the entity the click struck, or -1. It is only ever set on a click
 * frame: the pick walks every render entity's generated geometry, which is
 * affordable on a press and is not affordable every frame at a headset's
 * refresh, so this module does not hover-test.
 */
struct pointer3d_ray {
	float   origin[3];
	float   dir[3];
	int32_t hand;   /* enum pointer3d_hand */
	int32_t click;  /* 1 on the frame a press began */
	int32_t hit;    /* entity id the click picked, -1 = none/no click */
};

/*
 * The read side, published as the "pointer3d" subsystem api.
 *
 * Small on purpose: this is the first spatial-input surface in the engine and
 * should not try to be the last. Where a pointer points, which hand holds it,
 * whether it just clicked, and what that click hit — anything more (a held
 * level, a squeeze, a thumbstick, haptics) is a later api's problem and not a
 * field left half-defined in this one.
 */
struct pointer3d_api {
	/* How many pointers exist this frame: 0, 1 or 2. */
	int32_t (*count)(void);
	/*
	 * Copy pointer `index` into *out. Returns 0 on success, -1 if index is
	 * out of range or out is NULL. The copy is the caller's, so it does not
	 * go stale mid-frame the way a borrowed pointer would.
	 */
	int32_t (*get)(int32_t index, struct pointer3d_ray *out);
};

/*
 * The push side: this frame's pointers, in world space, from the host that has
 * them. Called once per frame, BEFORE the tick that reads them.
 *
 * count is clamped to POINTER3D_MAX, and a count of 0 (or a NULL rays) is the
 * ordinary way to say "nothing is pointing right now" — no controller, tracking
 * lost, or a host that has stopped reporting. Only origin, dir, hand and click
 * are read; hit is this module's answer and is overwritten.
 *
 * The published set replaces the previous one entirely rather than merging
 * into it, so a pointer that disappears between frames is gone on the next one
 * with nothing to unregister. That is what makes a controller switching off
 * mid-session a non-event here.
 */
void viewport_pointer3d_publish(const struct pointer3d_ray *rays,
				int32_t count);

/*
 * Drop every pointer and take down anything drawn for them. Equivalent to
 * publishing zero pointers, and spelled out so a host leaving for good says so
 * once rather than publishing emptiness forever.
 */
void viewport_pointer3d_clear(void);

#endif /* POINTER3D_H */
