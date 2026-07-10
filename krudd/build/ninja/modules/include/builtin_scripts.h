/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BUILTIN_SCRIPTS_H
#define BUILTIN_SCRIPTS_H

/*
 * Built-in entity scripts, seeded as read-only ASSET_TYPE_SCRIPT assets (see
 * asset_plugin.c) and bound to the demo scene (see scene_renderer.c). Each is a
 * single (script NAME ...) form in the same S7 Scheme the shader DSL uses —
 * the runtime image's (script ...) macro registers it and the entity-script
 * driver calls its clauses each frame.
 *
 * The scripts are stateless (the design choice for v1): a bound entity's pose
 * is a pure function of the clock `t` (seconds since boot) and the entity's
 * authored rest transform. A clause READS the rest pose via entity-base-* and
 * WRITES the animated render pose via entity-set-*!, so the authored transform
 * is preserved frame to frame and the animation never drifts.
 *
 * Kept here as one shared source of truth so the asset seeder and the native
 * entity_script oracle test compile the exact same script text.
 */

/* spinner — spin steadily about the Y axis, one full turn every 4 seconds. */
#define SPINNER_SCRIPT_SRC                                                    \
	"(script spinner\n"                                                   \
	"  (on-begin (self)\n"                                               \
	"    (krudd-log 1 \"spinner: awake\"))\n"                            \
	"  (on-tick (self t)\n"                                              \
	"    (entity-set-euler! self 0 (* t 90) 0)))\n"

/* bounce — hop along Y above the rest position, a rectified sine. */
#define BOUNCE_SCRIPT_SRC                                                     \
	"(script bounce\n"                                                    \
	"  (on-tick (self t)\n"                                              \
	"    (let ((b (entity-base-position self)))\n"                       \
	"      (entity-set-position! self\n"                                 \
	"        (car b)\n"                                                  \
	"        (+ (cadr b) (abs (* 0.6 (sin (* t 3.0)))))\n"              \
	"        (caddr b)))))\n"

/* wobble — rock gently on the X and Z axes, like a top losing balance. */
#define WOBBLE_SCRIPT_SRC                                                     \
	"(script wobble\n"                                                    \
	"  (on-tick (self t)\n"                                              \
	"    (entity-set-euler! self\n"                                      \
	"      (* 15 (sin (* t 2.0)))\n"                                     \
	"      0\n"                                                          \
	"      (* 15 (cos (* t 2.7))))))\n"

/*
 * pulse — breathe in and out about the rest scale. Unlike the three above it
 * carries authored parameters: `amp` (how far the scale swings) and `rate` (how
 * fast), declared in a (params ...) clause and read through (param ...). Both
 * default to 0 (a neutral, still pose), so the effect is dialed in per entity by
 * overriding the parameters on the entity — the CPU-side twin of a material
 * instance overriding a shader's Material block. Still stateless: the pose is a
 * pure function of the clock, the rest scale, and the authored params.
 */
#define PULSE_SCRIPT_SRC                                                      \
	"(script pulse\n"                                                    \
	"  (params (amp  float (edit range 0 2))\n"                         \
	"          (rate float (edit range 0 10)))\n"                       \
	"  (on-tick (self t)\n"                                             \
	"    (let ((b (entity-base-scale self))\n"                         \
	"          (s (+ 1.0 (* (param 'amp) (sin (* t (param 'rate)))))))\n" \
	"      (entity-set-scale! self\n"                                   \
	"        (* (car b) s) (* (cadr b) s) (* (caddr b) s)))))\n"

/*
 * orbit-camera — proof-of-life camera behavior: circle the origin at a
 * fixed radius/height, no input. Bound to the world's dedicated camera entity
 * the same way spinner/bounce/wobble bind to a mesh entity; scene_renderer
 * reads this entity's animated world_xform each tick and feeds it into the
 * camera's eye, so "camera behavior" is just another entity script.
 */
#define ORBIT_CAMERA_SCRIPT_SRC                                              \
	"(script orbit-camera\n"                                             \
	"  (on-tick (self t)\n"                                              \
	"    (entity-set-position! self\n"                                   \
	"      (* 5.0 (cos (* t 0.4)))\n"                                    \
	"      2.5\n"                                                        \
	"      (* 5.0 (sin (* t 0.4))))))\n"

#endif /* BUILTIN_SCRIPTS_H */
