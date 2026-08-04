/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PROJECT_TEST_H
#define PROJECT_TEST_H

#include <stdint.h>

struct asset_api;
struct world;

/*
 * project_test — the harness a project is driven through under test: the real
 * s7 image, the real launcher registry and a real world behind a stand-in
 * "scene" api, brought up by one call.
 *
 * A project is a single .scm the engine loads at runtime (#976), so every
 * project's test is the same program: bring the script half up, put a world
 * behind the three entity_api entries the project host reaches the world
 * through, hand the source to project_host_eval, then read the world back.
 * That program was written out by hand in every directory that had a project
 * in it — the same forty lines of vtable and counter-poll in the C, the same
 * nine include roots and seven link names in the build.scm — and a harness
 * copied five times is five places for it to drift. This is that program once,
 * as a library (#1035).
 *
 * A consumer declares (link "krudd_project_test") and includes this header.
 * The include roots a project test writes against — <entity/world.h>,
 * <host/game.h>, <project/project_host.h> and the rest — come with the link,
 * so the harness is one name in a build.scm rather than a list to keep in step
 * with this one.
 *
 * Native only, and not by accident: this is a test harness, so it must not
 * enter the WASM image. See the module's build.scm.
 *
 *
 * THE CATALOG IS OPTIONAL AT THE CALL SITE, NOT AT THE LINK SITE
 *
 * Projects divide in two. Some name assets — meshes, materials, entity scripts
 * — and register their own into the catalog as they are evaluated, so whether
 * those paths still bind is the thing worth testing and only the real catalog
 * can answer it. Others name none: an unbound mesh path still spawns every
 * entity a scene declares, so the geometry is fully readable with no catalog
 * behind it and a catalog would be weight for nothing.
 *
 * Both link this one archive. The difference is which init they call:
 * project_test_init brings up the lean half, project_test_init_with_catalog
 * brings up that plus the real asset catalog, the bindings a project's
 * *-define! calls register into it, and the entity-script driver.
 *
 * That is a linking property and not a convention anyone has to honour. The
 * catalog half is a translation unit of its own inside the archive, so a test
 * that never names project_test_init_with_catalog never pulls that member, and
 * the catalog, its seeded built-ins and the script driver are as absent from
 * its binary as if it had linked a leaner library. One archive, two costs —
 * which is why there is no second archive.
 */

/*
 * Bring the harness up: the allocator, the log, the s7 image and its scene-*
 * primitives, an empty world, and the project host over a subsystem manager
 * carrying the stand-in "scene" api. Call it once, before evaluating any
 * project source. Nothing is registered on the launcher by this — a project
 * gets there by project_host_eval, exactly as it does in the engine.
 */
void project_test_init(void);

/*
 * The same, plus the real asset catalog: it is seeded with the built-ins,
 * bound to the *-define! authoring trio so a project's own meshes, materials
 * and scripts land somewhere real, and handed to every build and dispatch the
 * harness makes. The entity-* primitives a (script ...) clause calls are
 * registered too, so a bound behaviour script can run.
 *
 * Use this when the project under test names or defines assets; use
 * project_test_init when it does not. See the note above for why the choice
 * costs a link nothing.
 */
void project_test_init_with_catalog(void);

/*
 * The world every check reads: entity count, liveness, names, transforms, and
 * the selection a click stands in for. Borrowed, never owned — it lives for
 * the process, because it is far too big for a stack frame.
 */
struct world *project_test_world(void);

/*
 * The catalog the harness passes on every build and dispatch, or NULL when it
 * came up without one. A test that wants to look an asset up — did the mesh
 * this project defined land, and with what bytes — goes through this vtable
 * rather than reaching for the catalog module's own header, so what a project
 * test may ask of the catalog is the abi's published surface and nothing wider.
 */
const struct asset_api *project_test_assets(void);

/*
 * One frame: ticks the subsystem manager, which is what runs the project
 * host's own per-frame half — the active-project gate, the (on-tick ...) hook
 * and the selection edge that fires (on-selected ...).
 */
void project_test_tick(void);

/*
 * Call image procedure FN (an integer -> integer) with ARG, the harness's world
 * and catalog bound for the call. This is how a C test reads image state back:
 * a project's rules keep their counters in the image and expose each through a
 * one-argument poll procedure. Returns FN's result, or -1 if FN is undefined.
 */
int32_t project_test_call(const char *fn, int32_t arg);

/*
 * Evaluate a bare (scene ...) form into the harness's world, outside any
 * project — the hand-built board a rules check wants when the project's own
 * scene is bigger than the thing under test. Returns the entity count, or -1.
 */
int32_t project_test_build(const char *src);

/*
 * The id of the live entity named NAME, or -1. The "what was clicked" a real
 * picker would hand the engine, and the one lookup every project test wrote
 * for itself.
 */
int32_t project_test_entity(const char *name);

/*
 * Drive one frame of entity scripts at clock T (seconds) — the driver that
 * ticks a bound behaviour script, which the engine runs from the entity plugin
 * and which a test must step by hand because it owns its own clock. Part of
 * the catalog half: a script only runs if the catalog it was defined into is
 * really there, so this is meaningful only after
 * project_test_init_with_catalog.
 */
void project_test_script_tick(float t);

#endif /* PROJECT_TEST_H */
