/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "xr_bridge.h"

#include <viewport/pointer3d.h>
#include <xr/xr.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

/*
 * The controllers: what a frame's XRInputSources mean once they are in the
 * engine's world (#996). The half of input that is not the browser, so a
 * native run can hold it to account — the XRInputSource reading itself is
 * xr_session.c's glue, and everything it observes crosses into here through
 * the staging buffer below.
 *
 * WHAT THIS MODULE DOES WITH A CONTROLLER, AND WHAT IT DOES NOT.
 *
 * It does two things. It composes the aim ray onto the stage, so the ray lands
 * in the same world the eyes are looking at (#994 established that translation
 * and this reuses it rather than deriving a second one). And it turns a run of
 * `select` events into a ONE-FRAME CLICK EDGE, because that is the shape every
 * existing consumer reads — kruddgui clears its pointer edge right after the
 * overlays run, and a game reads an edge, not a level.
 *
 * It does not pick, and it does not draw. Both belong to ui/viewport, which
 * already owns the raycast a canvas click goes through, and ui/ sits BELOW xr/
 * in the module order — so this calls DOWN into it and ui/ never learns that a
 * headset exists. That direction is the initiative's rule (#987) and it is
 * checked mechanically: resolve-check-tiers fails generation on a link edge
 * that inverts the order in kruddmake/manifest.scm.
 *
 * Not in scope, and deliberately: hand tracking, haptics, squeeze/grab,
 * teleport, thumbsticks. This is the first XR input surface and it should not
 * try to be the last.
 */

/*
 * The staging buffer and the composed rays it becomes. Both static: the glue
 * is handed the stage's address once, at install time, and the ray array is
 * scratch that is refilled in place every frame rather than allocated at a
 * headset's refresh rate.
 */
static union xr_stage_slot  g_in_stage[XR_MAX_INPUT_SOURCES * XR_IN_STRIDE];
static struct pointer3d_ray g_rays[XR_MAX_INPUT_SOURCES];

union xr_stage_slot *xr_input_stage(void)
{
	return g_in_stage;
}

/*
 * The two caps are set independently — one is what a WebXR runtime may report,
 * the other is what the engine's spatial pointer holds — so the smaller of them
 * is what a frame can actually carry. Written as a fold rather than an
 * assertion because neither module owns the other's number, and the honest
 * behaviour if they ever diverge is to publish what fits.
 */
#define XR_INPUT_MAX \
	(XR_MAX_INPUT_SOURCES < POINTER3D_MAX ? XR_MAX_INPUT_SOURCES \
					      : POINTER3D_MAX)

/*
 * One staged input source, composed into a world-space pointer.
 *
 * The origin is a point and the stage is a translation, so the stage is ADDED
 * to it — the same composition compose_view() applies to an eye, and the reason
 * it must be the same one is that a ray and the eyes looking along it have to
 * agree about where the world is. The direction is NOT composed: translating
 * the world does not turn anything, and rotating an aim ray by a stage offset
 * would bend it away from the hand holding it.
 *
 * The direction is renormalised rather than trusted. It arrives as a column of
 * a pose matrix and should already be unit length, but "should" is a statement
 * about a runtime this repo cannot run, and pointer3d.h promises its consumers
 * a unit direction.
 */
static int32_t compose_ray(struct pointer3d_ray *out,
			   const union xr_stage_slot *s, const float stage[3])
{
	float   len;
	int32_t i;

	memset(out, 0, sizeof(*out));
	out->hit = -1;

	for (i = 0; i < 3; i++) {
		out->origin[i] = s[XR_IN_ORIGIN + i].f + stage[i];
		out->dir[i]    = s[XR_IN_DIR + i].f;
	}

	len = sqrtf(out->dir[0] * out->dir[0] + out->dir[1] * out->dir[1] +
		    out->dir[2] * out->dir[2]);
	/*
	 * A source with no direction is not a pointer. Dropped rather than
	 * published as a degenerate ray, which the pick would refuse anyway and
	 * the rod would be drawn from an undefined rotation.
	 */
	if (!(len > 1e-6f))
		return 0;
	for (i = 0; i < 3; i++)
		out->dir[i] /= len;

	out->hand = s[XR_IN_HAND].i;
	if (out->hand != POINTER3D_HAND_LEFT &&
	    out->hand != POINTER3D_HAND_RIGHT)
		out->hand = POINTER3D_HAND_NONE;

	/*
	 * The edge. The glue hands over how many presses landed since the last
	 * frame and zeroes its own tally as it does, so this is 1 on exactly
	 * one frame per press: a press cannot be reported twice (the tally is
	 * spent) and cannot be missed (it is a count, not a flag that a second
	 * press would find already set).
	 */
	out->click = s[XR_IN_SELECT].i > 0 ? 1 : 0;
	return 1;
}

void xr_input_publish(int32_t count)
{
	float   stage[3];
	int32_t i, n = 0;

	/*
	 * Outside a session there are no controllers. Guarded here rather than
	 * trusted to the caller because the glue is not the only caller a page
	 * could ever have, and a pointer published with no session behind it
	 * would put a rod in the flat page's scene with nothing moving it.
	 */
	if (!xr_session_active())
		count = 0;
	if (count < 0)
		count = 0;
	if (count > XR_INPUT_MAX)
		count = XR_INPUT_MAX;

	xr_stage_offset(stage);
	for (i = 0; i < count; i++) {
		if (compose_ray(&g_rays[n], g_in_stage + i * XR_IN_STRIDE,
				stage))
			n++;
	}

	/*
	 * Down into ui/viewport, every frame, including the frames with nothing
	 * to report — publishing zero is how a controller that was put down, or
	 * a frame that lost tracking, stops pointing. The set replaces rather
	 * than merges, so a disconnect needs no message of its own.
	 */
	viewport_pointer3d_publish(g_rays, n);
}

void xr_input_reset(void)
{
	memset(g_rays, 0, sizeof(g_rays));
	viewport_pointer3d_clear();
}
