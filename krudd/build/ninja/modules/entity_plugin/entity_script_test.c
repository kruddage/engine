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

int main(void)
{
	log_init();
	script_init();        /* loads the embedded entity_script.scm image */
	entity_script_init(); /* registers the entity-* host primitives */

	test_bind_semantics();
	test_builtin_behavior();
	test_runtime_rebind();
	test_clone_name_clash();

	printf("entity_script_test: ok\n");
	return 0;
}
