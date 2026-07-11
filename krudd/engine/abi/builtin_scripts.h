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

/*
 * spinner — spin steadily about the Y axis. `speed` (degrees per second)
 * defaults to 90 — one full turn every 4 seconds, the rate it always spun at —
 * so an un-tuned spinner is unchanged; drop it to 0 to stop or up to whirl.
 */
#define SPINNER_SCRIPT_SRC                                                    \
	"(script spinner\n"                                                   \
	"  (params (speed float (default 90) (edit range 0 720)))\n"         \
	"  (on-begin (self)\n"                                               \
	"    (krudd-log 1 \"spinner: awake\"))\n"                            \
	"  (on-tick (self t)\n"                                              \
	"    (entity-set-euler! self 0 (* t (param 'speed)) 0)))\n"

/*
 * bounce — hop along Y above the rest position, a rectified sine. `height` (how
 * high) and `rate` (how fast) default to the values it always hopped at, so an
 * un-tuned bounce looks exactly as before yet slides down to a still 0 or up.
 */
#define BOUNCE_SCRIPT_SRC                                                     \
	"(script bounce\n"                                                    \
	"  (params (height float (default 0.6) (edit range 0 3))\n"          \
	"          (rate   float (default 3)   (edit range 0 10)))\n"        \
	"  (on-tick (self t)\n"                                              \
	"    (let ((b (entity-base-position self)))\n"                       \
	"      (entity-set-position! self\n"                                 \
	"        (car b)\n"                                                  \
	"        (+ (cadr b) (abs (* (param 'height) (sin (* t (param 'rate))))))\n" \
	"        (caddr b)))))\n"

/*
 * wobble — rock gently on the X and Z axes, like a top losing balance. `amp`
 * (tilt in degrees) and `rate` (a speed multiplier over the fixed 2.0:2.7 axis
 * ratio) default to the tilt/speed it always rocked at, so an un-tuned wobble is
 * unchanged; drop amp to 0 to still it or up to lean harder.
 */
#define WOBBLE_SCRIPT_SRC                                                     \
	"(script wobble\n"                                                    \
	"  (params (amp  float (default 15) (edit range 0 45))\n"           \
	"          (rate float (default 1)  (edit range 0 3)))\n"           \
	"  (on-tick (self t)\n"                                              \
	"    (entity-set-euler! self\n"                                      \
	"      (* (param 'amp) (sin (* t 2.0 (param 'rate))))\n"            \
	"      0\n"                                                          \
	"      (* (param 'amp) (cos (* t 2.7 (param 'rate)))))))\n"

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
 * orbit-camera — circle the origin at an authored radius/height/speed, no
 * input. Bound to the world's dedicated camera entity the same way
 * spinner/bounce/wobble bind to a mesh entity; scene_renderer reads this
 * entity's animated world_xform each tick and feeds it into the camera's eye,
 * so "camera behavior" is just another entity script. `radius`, `height`, and
 * `speed` default to the values it always orbited at, so an un-tuned camera
 * is unchanged. `angle-offset` is the starting angle (radians) added to
 * t*speed, letting an author park the camera wherever they stopped it
 * instead of always starting at angle 0.
 */
#define ORBIT_CAMERA_SCRIPT_SRC                                              \
	"(script orbit-camera\n"                                             \
	"  (params (radius       float (default 5)   (edit range 0 20))\n"  \
	"          (height       float (default 2.5) (edit range 0 10))\n"  \
	"          (speed        float (default 0.4) (edit range 0 3))\n"   \
	"          (angle-offset float (default 0)    (edit range -3.14159265 3.14159265)))\n" \
	"  (on-tick (self t)\n"                                              \
	"    (let ((a (+ (* t (param 'speed)) (param 'angle-offset))))\n"    \
	"      (entity-set-position! self\n"                                 \
	"        (* (param 'radius) (cos a))\n"                              \
	"        (param 'height)\n"                                          \
	"        (* (param 'radius) (sin a))))))\n"

#endif /* BUILTIN_SCRIPTS_H */
