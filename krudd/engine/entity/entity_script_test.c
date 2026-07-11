/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * entity_script — the entity scripting path end to end, minus the GPU. It boots
 * the real s7 image (which loads the embedded entity_script.scm), registers the
 * entity-* host primitives, binds the three built-in scripts to a hand-built
 * world, and drives one frame the way the scene tick does — then checks that
 * each entity's animated render pose (world_xform) is what the script's clause
 * computes from the clock and the authored rest pose (local).
 *
 * The scripts under test are the exact SPINNER/BOUNCE/WOBBLE sources the asset
 * seeder embeds, pulled from the shared builtin_scripts.h.
 */
#include "world.h"
#include "scene.h"
#include "asset_api.h"
#include "entity_script.h"
#include "builtin_scripts.h"

#include "script.h"
#include "log.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* One world instance reused across the checks; too big for the stack. */
static struct world w;

static int feq(float a, float b)
{
	float d = a - b;

	return (d < 0.0f ? -d : d) < 1e-4f;
}

/*
 * A stand-in asset catalog: script_ref 1/2/3 resolve to the spinner/bounce/
 * wobble source, mirroring what the real asset plugin seeds. Refs 4/5 are two
 * scripts that share a NAME but differ in body — a clone the author has begun
 * editing before renaming it. Only get_data is exercised by the driver.
 */
static const void *fake_get_data(uint32_t id, uint32_t *out_size)
{
	const char *src = NULL;

	switch (id) {
	case 1: src = SPINNER_SCRIPT_SRC; break;
	case 2: src = BOUNCE_SCRIPT_SRC;  break;
	case 3: src = WOBBLE_SCRIPT_SRC;  break;
	/* Same name, distinct bodies — each parks its entity at its own x. */
	case 4: src = "(script twin (on-tick (self t)"
		      " (entity-set-position! self 4 0 0)))"; break;
	case 5: src = "(script twin (on-tick (self t)"
		      " (entity-set-position! self 5 0 0)))"; break;
	case 6: src = PULSE_SCRIPT_SRC; break;   /* the parameterized built-in */
	case 7: src = ORBIT_CAMERA_SCRIPT_SRC; break;
	default: break;
	}
	if (src && out_size)
		*out_size = (uint32_t)strlen(src) + 1;
	return src;
}

static const struct asset_api fake_asset = { .get_data = fake_get_data };

/* Append a root entity at (x,y,z), identity rotation, unit scale. */
static int32_t spawn(float x, float y, float z)
{
	struct transform t;

	memset(&t, 0, sizeof(t));
	t.position[0] = x;
	t.position[1] = y;
	t.position[2] = z;
	t.rotation[3] = 1.0f;             /* identity quaternion */
	t.scale[0] = t.scale[1] = t.scale[2] = 1.0f;
	return world_create_entity(&w, WORLD_NO_PARENT, &t, 0u);
}

/* The set/clear semantics of the script binding, independent of the image. */
static void test_bind_semantics(void)
{
	int32_t e;

	world_reset(&w);
	e = spawn(0.0f, 0.0f, 0.0f);
	assert(e >= 0);

	assert(!(w.mask[e] & COMPONENT_SCRIPT));
	world_set_script_ref(&w, e, 7);
	assert(w.mask[e] & COMPONENT_SCRIPT);
	assert(w.script_ref[e] == 7);

	/* A zero ref unbinds and clears the component (mirrors render/material). */
	world_set_script_ref(&w, e, 0);
	assert(!(w.mask[e] & COMPONENT_SCRIPT));
	assert(w.script_ref[e] == 0);
}

/*
 * Bind the three built-ins, tick at t = 0.5 s, and check each animated pose.
 * A clause reads the rest pose (local) and writes world_xform, so the authored
 * local[] must be untouched afterward.
 */
static void test_builtin_behavior(void)
{
	int32_t cube, sphere, pyramid, cube2;
	float   by;

	world_reset(&w);
	cube    = spawn(-1.5f, 0.0f,  0.0f);   /* spinner */
	sphere  = spawn( 0.0f, 1.0f, -1.0f);   /* bounce  */
	pyramid = spawn( 1.5f, 0.0f,  0.5f);   /* wobble  */
	cube2   = spawn( 3.0f, 0.0f,  0.0f);   /* spinner again — shared name */

	world_set_script_ref(&w, cube,    1);
	world_set_script_ref(&w, sphere,  2);
	world_set_script_ref(&w, pyramid, 3);
	world_set_script_ref(&w, cube2,   1);

	/* world_tick refills world_xform from local before scripts run. */
	world_tick(&w, 16.0f);
	entity_script_tick(&w, &fake_asset, 0.5f);

	/*
	 * spinner: a pure Y rotation of t*90 = 45 deg. The xyzw quaternion is
	 * (0, sin(22.5), 0, cos(22.5)); position and scale stay at the rest pose.
	 */
	assert(feq(w.world_xform[cube].rotation[0], 0.0f));
	assert(feq(w.world_xform[cube].rotation[1], 0.382683f));
	assert(feq(w.world_xform[cube].rotation[2], 0.0f));
	assert(feq(w.world_xform[cube].rotation[3], 0.923880f));
	assert(feq(w.world_xform[cube].position[0], -1.5f));
	/* The authored transform is never clobbered. */
	assert(feq(w.local[cube].position[0], -1.5f));
	assert(feq(w.local[cube].rotation[3], 1.0f));

	/* The second spinner-bound entity animates identically (shared script). */
	assert(feq(w.world_xform[cube2].rotation[1], 0.382683f));

	/*
	 * bounce: y = base + |0.6 * sin(t*3)| = 1 + |0.6*sin(1.5)| ~= 1.5985;
	 * x and z hold at the rest pose and the rotation stays identity.
	 */
	by = 1.0f + (float)fabs(0.6 * sin(1.5));
	assert(feq(w.world_xform[sphere].position[1], by));
	assert(feq(w.world_xform[sphere].position[0], 0.0f));
	assert(feq(w.world_xform[sphere].position[2], -1.0f));
	assert(feq(w.world_xform[sphere].rotation[3], 1.0f));

	/*
	 * wobble: tilt on X and Z, none authored on Y. The quaternion carries
	 * clear x and z components; its y is only the tiny second-order coupling
	 * of the combined X-Z rotation, far smaller than the x tilt.
	 */
	assert(fabs(w.world_xform[pyramid].rotation[0]) > 0.05f);
	assert(fabs(w.world_xform[pyramid].rotation[2]) > 0.01f);
	assert(fabs(w.world_xform[pyramid].rotation[1])
	       < fabs(w.world_xform[pyramid].rotation[0]));

	/*
	 * A second frame must not fault (exercises the on-begin-once cache). At
	 * t = 1 s the spin is 90 deg, so q.y = sin(45 deg) ~= 0.707.
	 */
	world_tick(&w, 16.0f);
	entity_script_tick(&w, &fake_asset, 1.0f);
	assert(feq(w.world_xform[cube].rotation[1], 0.707107f));
}

/*
 * Rebinding a live, already-ticked entity to a different script asset must
 * switch its motion on the very next tick, not keep running the old script's
 * hooks forever. Regression test for a stale per-entity resolve cache that
 * only ever kept whatever script first bound to the id (see entity_script.scm
 * entity-script-resolve): kruddboard's World-tab script dropdown surfaced it
 * by being the first path that rebinds an entity's script at runtime.
 */
static void test_runtime_rebind(void)
{
	int32_t e;
	float   by;

	world_reset(&w);
	e = spawn(0.0f, 1.0f, -1.0f);

	world_set_script_ref(&w, e, 1); /* spinner */
	world_tick(&w, 16.0f);
	entity_script_tick(&w, &fake_asset, 0.5f);
	assert(feq(w.world_xform[e].rotation[1], 0.382683f)); /* spinning */
	assert(feq(w.world_xform[e].position[1], 1.0f));       /* untouched */

	/* Rebind the live entity to a different script asset (bounce) — the
	 * next tick must switch to bounce's motion, not keep spinning.
	 */
	world_set_script_ref(&w, e, 2);
	world_tick(&w, 16.0f);
	entity_script_tick(&w, &fake_asset, 0.5f);

	by = 1.0f + (float)fabs(0.6 * sin(1.5));
	assert(feq(w.world_xform[e].position[1], by));   /* bounce formula   */
	assert(feq(w.world_xform[e].rotation[3], 1.0f)); /* rest, not spinning */
}

/*
 * Two script assets whose (script NAME ...) forms share a NAME but differ in
 * body must each run their own hooks. Regression test for a name-keyed script
 * registry (see entity_script.scm *entity-scripts*): registration was idempotent
 * by NAME, so a clone kept its original's name and — once edited — silently ran
 * the original's hooks. kruddboard's asset "Clone" flow surfaced it: the clone
 * broke until its inner name was changed by hand.
 */
static void test_clone_name_clash(void)
{
	int32_t a, b;

	world_reset(&w);
	a = spawn(0.0f, 0.0f, 0.0f);
	b = spawn(0.0f, 0.0f, 0.0f);

	world_set_script_ref(&w, a, 4); /* (script twin ...) -> x = 4 */
	world_set_script_ref(&w, b, 5); /* (script twin ...) -> x = 5 */
	world_tick(&w, 16.0f);
	entity_script_tick(&w, &fake_asset, 0.5f);

	/* Each entity animates to its OWN script's x, not its name-twin's. */
	assert(feq(w.world_xform[a].position[0], 4.0f));
	assert(feq(w.world_xform[b].position[0], 5.0f));
}

/*
 * The (params ...) clause introspects into the same shape shader params do,
 * tight-packed (no std140 gap): pulse declares amp then rate, two scalar floats
 * at offsets 0 and 4, each an (edit range ...) with its own bounds.
 */
static void test_script_params_introspection(void)
{
	struct shader_param p[8];
	uint32_t            total = 99;
	int                 n;

	n = script_entity_params(PULSE_SCRIPT_SRC, p, 8, &total);
	assert(n == 2);
	assert(total == 8);

	assert(strcmp(p[0].name, "amp") == 0);
	assert(strcmp(p[0].type, "float") == 0);
	assert(p[0].offset == 0 && p[0].size == 4 && p[0].components == 1);
	assert(strcmp(p[0].edit, "range") == 0);
	assert(feq(p[0].edit_min, 0.0f) && feq(p[0].edit_max, 2.0f));

	assert(strcmp(p[1].name, "rate") == 0);
	assert(p[1].offset == 4 && p[1].size == 4);
	assert(feq(p[1].edit_max, 10.0f));

	/*
	 * spinner declares one range param with an authored (default 90): its
	 * un-overridden value is that default, NOT the range's min, which is what
	 * lets a parameterized built-in keep the motion it always had.
	 */
	n = script_entity_params(SPINNER_SCRIPT_SRC, p, 8, &total);
	assert(n == 1 && total == 4);
	assert(strcmp(p[0].name, "speed") == 0);
	assert(strcmp(p[0].edit, "range") == 0 && feq(p[0].edit_min, 0.0f));
	assert(p[0].default_count == 1 && feq(p[0].edit_default[0], 90.0f));

	/* A param with no (default ...) reports default_count 0 and falls back to
	 * its edit hint — pulse's amp, unchanged, still defaults to its range min. */
	n = script_entity_params(PULSE_SCRIPT_SRC, p, 8, &total);
	assert(n == 2 && p[0].default_count == 0);

	/* A script with no params clause reports zero fields, not an error. */
	assert(script_entity_params("(script bare (on-tick (self t) #f))",
				    p, 8, &total) == 0);
	assert(total == 0);

	/*
	 * orbit-camera declares four range params, each with an authored
	 * default matching the fixed radius/height/speed/angle-offset it
	 * always orbited at, so an un-tuned camera keeps its original motion.
	 */
	n = script_entity_params(ORBIT_CAMERA_SCRIPT_SRC, p, 8, &total);
	assert(n == 4 && total == 16);

	assert(strcmp(p[0].name, "radius") == 0);
	assert(p[0].offset == 0 && p[0].default_count == 1);
	assert(feq(p[0].edit_default[0], 5.0f));
	assert(feq(p[0].edit_min, 0.0f) && feq(p[0].edit_max, 20.0f));

	assert(strcmp(p[1].name, "height") == 0);
	assert(p[1].offset == 4 && p[1].default_count == 1);
	assert(feq(p[1].edit_default[0], 2.5f));
	assert(feq(p[1].edit_max, 10.0f));

	assert(strcmp(p[2].name, "speed") == 0);
	assert(p[2].offset == 8 && p[2].default_count == 1);
	assert(feq(p[2].edit_default[0], 0.4f));
	assert(feq(p[2].edit_max, 3.0f));

	assert(strcmp(p[3].name, "angle-offset") == 0);
	assert(p[3].offset == 12 && p[3].default_count == 1);
	assert(feq(p[3].edit_default[0], 0.0f));
	assert(feq(p[3].edit_min, -3.14159265f) && feq(p[3].edit_max, 3.14159265f));
}

/*
 * A bound script reads its authored parameters through (param ...): pulse scales
 * about its rest pose by amp*sin(t*rate). With no per-entity override both
 * params default to 0 (a still pose); a packed override drives the animation.
 */
static void test_param_override(void)
{
	int32_t e;
	float   ov[2];
	float   expect;

	world_reset(&w);
	e = spawn(0.0f, 0.0f, 0.0f);            /* unit rest scale */
	world_set_script_ref(&w, e, 6);         /* pulse */

	/* No override yet: amp defaults to 0, so the scale holds at the base. */
	world_tick(&w, 16.0f);
	entity_script_tick(&w, &fake_asset, 0.5f);
	assert(feq(w.world_xform[e].scale[0], 1.0f));
	assert(feq(w.world_xform[e].scale[1], 1.0f));

	/* Override amp=0.5, rate=2 (tight-packed floats at offsets 0 and 4). */
	ov[0] = 0.5f;
	ov[1] = 2.0f;
	world_set_script_params(&w, e, (const uint8_t *)ov, sizeof(ov));

	world_tick(&w, 16.0f);
	entity_script_tick(&w, &fake_asset, 0.5f);
	expect = 1.0f + 0.5f * (float)sin(1.0);   /* t*rate = 1.0 */
	assert(feq(w.world_xform[e].scale[0], expect));
	assert(feq(w.world_xform[e].scale[2], expect));

	/* Clearing the override returns the script to its still default pose. */
	world_set_script_params(&w, e, NULL, 0);
	world_tick(&w, 16.0f);
	entity_script_tick(&w, &fake_asset, 0.5f);
	assert(feq(w.world_xform[e].scale[0], 1.0f));
}

/*
 * orbit-camera circles the origin at (param 'radius) about the origin, held
 * at (param 'height), sweeping at (param 'speed) radians per second starting
 * from (param 'angle-offset) radians. With no override the defaults
 * reproduce the fixed orbit it always flew; a packed override re-tunes
 * radius/height/speed/angle-offset per the authored order.
 */
static void test_orbit_camera_params(void)
{
	int32_t e;
	float   ov[4];

	world_reset(&w);
	e = spawn(0.0f, 0.0f, 0.0f);
	world_set_script_ref(&w, e, 7);          /* orbit-camera */

	/* Defaults: radius=5, height=2.5, speed=0.4, angle-offset=0 -> the
	 * original fixed orbit. */
	world_tick(&w, 16.0f);
	entity_script_tick(&w, &fake_asset, 0.5f);
	assert(feq(w.world_xform[e].position[0], 5.0f * (float)cos(0.5 * 0.4)));
	assert(feq(w.world_xform[e].position[1], 2.5f));
	assert(feq(w.world_xform[e].position[2], 5.0f * (float)sin(0.5 * 0.4)));

	/* Override radius=10, height=1, speed=1, angle-offset=1.5 (tight-packed
	 * at offsets 0/4/8/12). */
	ov[0] = 10.0f;
	ov[1] = 1.0f;
	ov[2] = 1.0f;
	ov[3] = 1.5f;
	world_set_script_params(&w, e, (const uint8_t *)ov, sizeof(ov));

	world_tick(&w, 16.0f);
	entity_script_tick(&w, &fake_asset, 0.5f);
	assert(feq(w.world_xform[e].position[0], 10.0f * (float)cos(0.5 + 1.5f)));
	assert(feq(w.world_xform[e].position[1], 1.0f));
	assert(feq(w.world_xform[e].position[2], 10.0f * (float)sin(0.5 + 1.5f)));
}

int main(void)
{
	log_init();
	script_init();        /* loads the embedded entity_script.scm image */
	entity_script_init(); /* registers the entity-* host primitives */

	test_bind_semantics();
	test_builtin_behavior();
	test_runtime_rebind();
	test_clone_name_clash();
	test_script_params_introspection();
	test_param_override();
	test_orbit_camera_params();

	printf("entity_script_test: ok\n");
	return 0;
}
