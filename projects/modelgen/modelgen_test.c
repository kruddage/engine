/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * modelgen — the viewer end to end against the real image and the real asset
 * catalog.
 *
 * modelgen.scm is one (project ...) form, so this drives it the way the engine
 * does: game/project_test brings up the s7 image, the launcher registry and a
 * world behind a stand-in "scene" api, project_host_eval registers the form,
 * and game_load runs the host's own load path. The FAT half of the harness,
 * because everything this project is about goes through the catalog — six
 * meshes, a material and a turntable script the source registers as it is
 * evaluated, and a mesh re-bound onto a live entity every time the dropdown
 * moves.
 *
 * What it checks, which is the whole of what "proof of life" means here:
 *
 *   - the stage builds and the load hook spawns the display entity onto it,
 *     wearing this project's material and its turntable script;
 *   - every model in the list binds real geometry — the entity's render_ref is
 *     the catalog id of that model's path, for all six, and the six are
 *     distinct;
 *   - a pick out of range clamps to an end of the list rather than binding
 *     nothing or faulting;
 *   - the turntable turns: ticking the script drives the model's rendered yaw
 *     to where modelgen-spin-deg says it should be at that clock, and leaves
 *     the authored transform alone, so the model turns without wandering off
 *     its plinth;
 *   - loading installs the viewer's panel in the host's panel slot, and calling
 *     that slot the way the gui host does draws the dropdown's owner.
 *
 * The panel is the one thing here that cannot be driven end to end natively:
 * what it draws with is kruddgui's vocabulary, and kruddgui is a wasm-only
 * module. What IS checkable is the seam either side of it — that a load
 * installs a procedure, and that it is the viewer's own — and that is what the
 * last check below asks. The seam's own behaviour (installed on load, cleared
 * by the next one, survivable when it faults) belongs to the host that owns it
 * and is checked there, in game/project/project_host_test.c.
 */
#include <project_test/project_test.h>

#include <project/project_host.h>

#include <abi/asset_api.h>
#include <core/script.h>
#include <entity/world.h>
#include <host/game.h>

#include "s7.h"

#include "modelgen_scm.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The harness's world and catalog, borrowed for the life of the process. */
static struct world          *w;
static const struct asset_api *assets;

/* The launcher slot modelgen.scm took, from project_host_eval in main. */
static int32_t g_modelgen;

/*
 * The stage the (scene ...) clause authors — camera, ground, plinth — plus the
 * one entity the load hook spawns onto it. Its fingerprint: it moves only when
 * modelgen.scm gains or loses an entity.
 */
#define MODELGEN_ENTITY_COUNT 4

/* The models the dropdown offers, in the order modelgen.scm lists them. */
static const char *MODEL_PATHS[] = {
	"project://mesh/modelgen-pawn",
	"project://mesh/modelgen-knight",
	"project://mesh/modelgen-rook",
	"project://mesh/modelgen-bishop",
	"project://mesh/modelgen-queen",
	"project://mesh/modelgen-king",
};

#define MODEL_COUNT ((int32_t)(sizeof(MODEL_PATHS) / sizeof(MODEL_PATHS[0])))

/* Read one of the viewer's counters back: a poll, not a call. */
static int32_t poll(const char *fn)
{
	return project_test_call(fn, 0);
}

/* The display entity the load hook spawned. */
static int32_t model_id(void)
{
	return poll("modelgen-entity");
}

/* The stable catalog id at PATH, or 0 when nothing is there. */
static uint32_t asset_id_at(const char *path)
{
	struct asset_info info;
	uint32_t          i, n = assets->count();

	for (i = 0; i < n; i++) {
		if (assets->info(i, &info) == 0 && info.path
		    && strcmp(info.path, path) == 0)
			return info.id;
	}
	return 0;
}

/* The value of an image variable, for reading the panel slot back out. */
static s7_pointer image_value(const char *name)
{
	return s7_name_to_value(script_s7(), name);
}

/*
 * A source with no C behind it registers a launcher entry and nothing else;
 * choosing it clears the world, builds the stage, and runs the load hook, which
 * is where the display entity comes from.
 */
static void test_load_builds_the_stage(void)
{
	int32_t model;

	game_load(g_modelgen);
	assert(game_active_index() == g_modelgen);
	assert(w->count == MODELGEN_ENTITY_COUNT);
	assert(project_test_entity("Camera") >= 0);
	assert(project_test_entity("ground") >= 0);
	assert(project_test_entity("plinth") >= 0);

	/* The model is spawned rather than authored, so the rules hold its id
	 * and the world knows it by the name they gave it. */
	model = model_id();
	assert(model >= 0);
	assert(project_test_entity("model") == model);
	assert(w->mask[model] & COMPONENT_MATERIAL);
	assert(w->mask[model] & COMPONENT_SCRIPT);
	assert(w->material_ref[model]
	       == asset_id_at("project://material/modelgen-clay"));
	assert(w->script_ref[model]
	       == asset_id_at("project://script/modelgen-turntable"));

	/* On the plinth's top, which is where a model's foot goes. */
	assert(fabsf(w->local[model].position[0]) < 1e-4f);
	assert(fabsf(w->local[model].position[1] + 0.6f) < 1e-4f);
	assert(fabsf(w->local[model].position[2]) < 1e-4f);
}

/* A fresh load opens on the first model, bound to its real geometry. */
static void test_first_model_is_bound(void)
{
	int32_t model = model_id();

	assert(poll("modelgen-index") == 0);
	assert(poll("modelgen-models-n") == MODEL_COUNT);
	assert(w->mask[model] & COMPONENT_RENDER);
	assert(w->render_ref[model] != 0);
	assert(w->render_ref[model] == asset_id_at(MODEL_PATHS[0]));
}

/*
 * The whole of what choosing a model does: the pick is remembered and that
 * model's geometry lands on the display entity. Checked for every row in the
 * list, and the six refs are distinct — a dropdown that bound the same mesh
 * under six labels would pass any one of these checks on its own.
 */
static void test_every_model_binds_its_own_geometry(void)
{
	uint32_t seen[MODEL_COUNT];
	int32_t  i, j, model = model_id();

	for (i = 0; i < MODEL_COUNT; i++) {
		assert(project_test_call("modelgen-show!", i) == i);
		assert(poll("modelgen-index") == i);
		seen[i] = w->render_ref[model];
		assert(seen[i] != 0);
		assert(seen[i] == asset_id_at(MODEL_PATHS[i]));
	}
	for (i = 0; i < MODEL_COUNT; i++)
		for (j = i + 1; j < MODEL_COUNT; j++)
			assert(seen[i] != seen[j]);
}

/*
 * An index off either end of the list clamps to that end. The dropdown can only
 * hand back a row that exists, so this is the guard on everything else that
 * ever calls it — and a clamp keeps a stale pick from a reload pointing at a
 * model rather than at nothing.
 */
static void test_a_pick_off_the_end_clamps(void)
{
	int32_t model = model_id();

	assert(project_test_call("modelgen-show!", 99) == MODEL_COUNT - 1);
	assert(w->render_ref[model] == asset_id_at(MODEL_PATHS[MODEL_COUNT - 1]));

	assert(project_test_call("modelgen-show!", -3) == 0);
	assert(w->render_ref[model] == asset_id_at(MODEL_PATHS[0]));
}

/*
 * The turntable, end to end: the script is in the catalog because the source
 * put it there, the spawn bound that path, and the entity-script driver ticks
 * it. What is asserted is what is on screen — the rendered yaw at clock t is
 * the yaw modelgen-spin-deg names for t, as a quaternion about Y — and what is
 * NOT on screen: the authored transform, which never moves, so a model turns in
 * place however long it is left running.
 *
 * The expected angle is asked of the rule rather than restated here: a whole
 * number of seconds so it can go through dispatch_scm (integer in, integer
 * out), which is also why the check cannot agree with the turntable by being a
 * copy of its rate.
 */
static void test_the_turntable_turns(void)
{
	int32_t model = model_id();
	float   t     = 3.0f;
	float   half  = (float)project_test_call("modelgen-spin-deg", (int32_t)t)
			* 3.14159265f / 180.0f * 0.5f;

	project_test_script_tick(t);

	assert(fabsf(w->world_xform[model].rotation[0]) < 1e-4f);
	assert(fabsf(w->world_xform[model].rotation[2]) < 1e-4f);
	assert(fabsf(w->world_xform[model].rotation[1] - sinf(half)) < 1e-3f);
	assert(fabsf(fabsf(w->world_xform[model].rotation[3]) - cosf(half))
	       < 1e-3f);

	/* Turning is a render pose: the authored one is untouched, so the model
	 * still stands on the plinth rather than drifting off it. */
	assert(fabsf(w->local[model].position[1] + 0.6f) < 1e-4f);
	assert(fabsf(w->local[model].rotation[1]) < 1e-4f);

	/* And it is still turning a while later, not parked at its first pose. */
	project_test_script_tick(t + 1.0f);
	assert(fabsf(w->world_xform[model].rotation[1] - sinf(half)) > 1e-3f);
}

/*
 * Loading installs the viewer's panel — the dropdown's owner — in the slot the
 * gui host draws from, and it is this project's own procedure rather than
 * whatever happened to be there: the load empties the slot before the hook
 * runs, so an install is something this project did on this load.
 */
static void test_load_installs_the_panel(void)
{
	game_load(g_modelgen);
	assert(s7_is_procedure(image_value("project-panel")));
	assert(image_value("project-panel") == image_value("modelgen-panel-draw"));
}

int main(void)
{
	/*
	 * The FAT half of the harness: this project defines assets and binds
	 * them, so the real catalog is what the checks above read the bindings
	 * out of, and its script driver is what turns the turntable.
	 */
	project_test_init_with_catalog();
	w      = project_test_world();
	assets = project_test_assets();
	assert(assets);

	g_modelgen = project_host_eval(MODELGEN_SCM);
	assert(g_modelgen >= 0);
	assert(game_find("Modelgen") == g_modelgen);

	test_load_builds_the_stage();
	test_first_model_is_bound();
	test_every_model_binds_its_own_geometry();
	test_a_pick_off_the_end_clamps();
	test_the_turntable_turns();
	test_load_installs_the_panel();

	printf("modelgen_test: ok\n");
	return 0;
}
