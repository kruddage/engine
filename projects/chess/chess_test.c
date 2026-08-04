/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * chess — the game end to end against the real image, with no C plugin behind
 * it. chess.scm is one (project ...) form, so this test drives it the way the
 * engine does: project_host_eval registers the form on the real launcher
 * registry, and game_load runs the host's own load path — clear the world,
 * build the (scene ...) clause, run the (on-load ...) hook — over a stand-in
 * "scene" api bound to the world below. That is the whole of what chess.c used
 * to be, so pointing the harness at the project source rather than at two
 * embedded fragments is the only shape change here; every check it was already
 * making it still makes.
 *
 * What it checks: the scene builds to the standard opening position (the right
 * entity count, and the key pieces on the right squares by name); the rules run
 * the two-click select→move flow over a small hand-built board — a slide, a
 * capture, re-picking, a deselect, turn alternation and the selection outline;
 * the camera script the game registers binds and eases; and the six piece
 * meshes and two army materials it registers land in the catalog with the
 * geometry and the bytes they had when the engine seeded them. No GPU and no
 * browser is needed — a mesh/material path that resolves to "unbound" still
 * spawns every named entity, so the layout is fully checkable headless.
 */
#include <entity/world.h>
#include <entity/scene_script.h>
#include "entity_script.h"

#include "asset.h"
#include <abi/asset_api.h>
#include <abi/entity_api.h>
#include <asset/mesh_script.h>

#include <core/script.h>
#include <core/subsystem_manager.h>
#include <host/game.h>
#include <log/log.h>
#include <memory/memory.h>
#include <project/project_host.h>

#include "chess_scm.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* One world instance reused across the checks; too big for the stack. */
static struct world w;

static struct subsystem_manager mgr;

/* The launcher slot chess.scm took, from project_host_eval in main. */
static int32_t g_chess;

/*
 * The real asset catalog, assembled from asset.h the way the asset plugin
 * assembles it for the subsystem manager. The rules checks need no catalog and
 * drive their hand-built board without one, but the camera and the piece meshes
 * do: those are assets this game registers into the catalog itself, and binding
 * them is the thing worth testing. So are the two army materials, which need
 * the catalog for a second reason: material-define! packs them against the
 * Material block of the seeded pbr shader it finds there.
 */
static const struct asset_api catalog_api = {
	.count    = asset_catalog_count,
	.info     = asset_catalog_info,
	.describe = asset_catalog_describe,
	.find     = asset_catalog_find,
	.get_data = asset_catalog_get_data,
};

static const struct asset_mut_api mut_api = {
	.create   = asset_mut_create,
	.set_data = asset_mut_set_data,
	.destroy  = asset_mut_destroy,
	.set_decl = asset_mut_set_decl,
	.inject   = asset_mut_inject,
};

/*
 * The stand-in "scene" subsystem: the three entity_api entries a project's host
 * uses, each binding this test's world exactly as entity_plugin.c binds the
 * live one. The real asset catalog is passed on every call, because that is
 * what the checks below are about — the meshes, materials and script chess.scm
 * registers only bind if the build and the dispatch can see the catalog.
 */
static int32_t t_build_scene_scm(const char *src)
{
	return scene_script_build(&w, &catalog_api, src);
}

static int32_t t_dispatch_scm(const char *fn, int32_t arg)
{
	return scene_script_call(&w, &catalog_api, fn, arg);
}

static void t_clear_world(void)
{
	world_reset(&w);
}

static const struct entity_api t_scene_api = {
	.build_scene_scm = t_build_scene_scm,
	.dispatch_scm    = t_dispatch_scm,
	.clear_world     = t_clear_world,
};

static const struct subsystem t_subsystems[] = {
	{ .name = "scene", .api = &t_scene_api },
	{ NULL }
};

/*
 * Total entities the scene spawns: a camera, a ground plane, the board slab, 64
 * square tiles, 32 pieces, and the two kings' crosses (two boxes each). Its
 * fingerprint — it moves only when the (scene ...) clause of chess.scm gains or
 * loses an entity.
 */
#define CHESS_ENTITY_COUNT 103

/*
 * Open the game the way the launcher does, and answer how many entities stand
 * afterwards. game_load runs the project host's own load callback, which clears
 * the world, builds chess.scm's (scene ...) clause into it and then runs the
 * (on-load ...) hook — the three steps chess_load used to do in C. Every check
 * that used to call scene_script_build on the embedded scene source calls this
 * instead, so what is tested is the path the engine actually takes.
 */
static uint32_t open_chess(void)
{
	game_load(g_chess);
	return w.count;
}

/* #t when a live entity named NAME exists — the piece-on-square check. */
static int has_named(const char *name)
{
	uint32_t e;

	for (e = 0; e < w.count; e++) {
		const char *n = world_entity_name(&w, e);

		if (n && strcmp(n, name) == 0)
			return 1;
	}
	return 0;
}

/* The scene builds to the full opening set, every entity accounted for. */
static void test_scene_builds(void)
{
	assert(open_chess() == CHESS_ENTITY_COUNT);
}

/*
 * The pieces stand in the standard opening position: kings on e, the white
 * queen on d1 (her own colour), rooks in the corners, and a full pawn rank in
 * front of each army. A representative sample pins the layout without listing
 * all 32.
 */
static void test_starting_position(void)
{
	assert(open_chess() == CHESS_ENTITY_COUNT);

	/* White back rank: R N B Q K B N R on rank 1. */
	assert(has_named("wR-a1") && has_named("wN-b1") && has_named("wB-c1"));
	assert(has_named("wQ-d1") && has_named("wK-e1"));
	assert(has_named("wB-f1") && has_named("wN-g1") && has_named("wR-h1"));

	/* Black mirrors it on rank 8, king still on the e file. */
	assert(has_named("bR-a8") && has_named("bQ-d8") && has_named("bK-e8"));
	assert(has_named("bR-h8"));

	/* Full pawn ranks: rank 2 white, rank 7 black. */
	assert(has_named("wP-a2") && has_named("wP-h2"));
	assert(has_named("bP-a7") && has_named("bP-h7"));

	/* The board itself and the camera are present. */
	assert(has_named("Camera") && has_named("board-base"));
	assert(has_named("sq-a1") && has_named("sq-h8"));

	/* No stray piece on an empty middle rank. */
	assert(!has_named("wP-a4"));
}

/*
 * Opening the game twice yields the same count — the build is deterministic,
 * and the host's load clears the world before it builds rather than stacking a
 * second set on top of the first.
 */
static void test_rebuild_is_stable(void)
{
	uint32_t a, b;

	a = open_chess();
	b = open_chess();
	assert(a == b && a == CHESS_ENTITY_COUNT);
}

/*
 * A tiny hand-built board for driving the rules, entity ids 0..3 in this order:
 *   0  wP-e2   a white pawn on e2   (world 0.5, 2.5)
 *   1  sq-e4   an empty target tile (world 0.5, 0.5)
 *   2  bP-e7   a black pawn on e7   (world 0.5, -2.5)   — a capture target
 *   3  wN-b1   a white knight on b1 (world -2.5, 3.5)   — a second white piece
 * The rules read names and positions and move by id, so no meshes or catalog are
 * needed; the y a piece lands at is the authored 0.03.
 */
static const char *RULES_BOARD =
	"(scene c"
	"  (entity (name \"wP-e2\") (at 0.5 0.03 2.5))"
	"  (entity (name \"sq-e4\") (at 0.5 0.02 0.5))"
	"  (entity (name \"bP-e7\") (at 0.5 0.03 -2.5))"
	"  (entity (name \"wN-b1\") (at -2.5 0.03 3.5))"
	"  (entity (name \"board-base\") (at 0.0 -0.16 0.0)))";

/* Click the entity whose id is `id`, returning chess-on-selected's code. */
static int32_t click(int32_t id)
{
	return scene_script_call(&w, NULL, "chess-on-selected", id);
}

/* Poll *chess-turn*: 1 white to move, 2 black. */
static int32_t turn(void)
{
	return scene_script_call(&w, NULL, "chess-turn", 0);
}

/* Poll chess-cam-holding?: 1 while the post-move camera hold is armed. */
static int32_t cam_holding(void)
{
	return scene_script_call(&w, NULL, "chess-cam-holding?", 0);
}

/*
 * The world's game-outline target — what the renderer's outline pass highlights
 * in-game. The rules set it through scene-outline! (world_set_outline) as the
 * player picks a piece up, so a headless test can watch the pick/drop directly
 * even though the ring itself needs a GPU to draw.
 */
static int32_t outline(void)
{
	return world_get_outline(&w);
}

/* #t when entity `id`'s local position is (x, *, z) within a hair. */
static int at_xz(int32_t id, float x, float z)
{
	const float *p = w.local[id].position;

	return fabsf(p[0] - x) < 1e-4f && fabsf(p[2] - z) < 1e-4f;
}

static void reset_rules_board(void)
{
	world_reset(&w);
	assert(scene_script_build(&w, NULL, RULES_BOARD) == 5);
	scene_script_call(&w, NULL, "chess-reset", 0);
}

/* Two clicks — pick a pawn, then an empty square — slide it and pass the turn. */
static void test_move_and_turn(void)
{
	reset_rules_board();

	assert(turn() == 1);             /* white to move */
	assert(click(0) == 0);           /* pick up wP-e2 (a selection, no move) */
	assert(at_xz(0, 0.5f, 2.5f));    /* still home until the second click */
	assert(turn() == 1);             /* selecting does not pass the turn */
	assert(click(1) == 1);           /* click sq-e4 -> slide there */
	assert(at_xz(0, 0.5f, 0.5f));    /* the pawn is on e4 now */
	assert(turn() == 2);             /* and it is black's move */
}

/* Picking an enemy piece as the destination removes it and takes its square. */
static void test_capture(void)
{
	reset_rules_board();

	assert(click(0) == 0);           /* pick up wP-e2 */
	assert(click(2) == 2);           /* click bP-e7 -> capture (code 2) */
	assert(!w.alive[2]);             /* the black pawn is gone */
	assert(at_xz(0, 0.5f, -2.5f));   /* the white pawn stands on e7 */
	assert(turn() == 2);             /* turn passed */
}

/* Clicking your own piece while one is picked re-picks it rather than moving. */
static void test_repick(void)
{
	reset_rules_board();

	assert(click(0) == 0);           /* pick up the pawn */
	assert(click(3) == 0);           /* click the knight (also white) -> re-pick */
	assert(at_xz(0, 0.5f, 2.5f));    /* the pawn did not move */
	assert(turn() == 1);             /* no move, still white */
	assert(click(1) == 1);           /* now sq-e4 moves the KNIGHT, not the pawn */
	assert(at_xz(3, 0.5f, 0.5f));    /* knight on e4 */
	assert(at_xz(0, 0.5f, 2.5f));    /* pawn untouched */
	assert(turn() == 2);
}

/* A first click on the wrong side's piece (or empty space) picks up nothing. */
static void test_wrong_side_ignored(void)
{
	reset_rules_board();

	assert(click(2) == 0);           /* black pawn, white to move -> ignored */
	assert(turn() == 1);
	assert(click(1) == 0);           /* an empty square with nothing picked -> no-op */
	assert(turn() == 1);
	assert(w.alive[2]);              /* nothing was moved or captured */
}

/*
 * The outline target tracks the picked piece: set on a pick, moved on a re-pick,
 * cleared on a move, a capture, or a deselect — and never set by picking the
 * wrong side. This is the state the renderer's in-game outline pass reads.
 */
static void test_outline_tracks_pick(void)
{
	reset_rules_board();

	assert(outline() == -1);         /* nothing picked at the start */
	assert(click(0) == 0);           /* pick up wP-e2 */
	assert(outline() == 0);          /* the pawn is outlined */
	assert(click(3) == 0);           /* re-pick the knight */
	assert(outline() == 3);          /* outline follows to the knight */
	assert(click(1) == 1);           /* move it to e4 */
	assert(outline() == -1);         /* a move clears the outline */

	reset_rules_board();
	assert(click(0) == 0);           /* pick the pawn */
	assert(click(2) == 2);           /* capture the black pawn */
	assert(outline() == -1);         /* a capture clears it too */

	reset_rules_board();
	assert(click(0) == 0);           /* pick the pawn */
	assert(outline() == 0);
	assert(click(4) == 0);           /* click the board slab -> deselect */
	assert(outline() == -1);         /* deselect clears the outline */
	assert(turn() == 1);             /* and did not pass the turn */

	reset_rules_board();
	assert(click(2) == 0);           /* black piece, white to move -> ignored */
	assert(outline() == -1);         /* nothing picked, nothing outlined */
}

/*
 * A move never arms the camera's post-move hold in this harness: chess-cam-
 * mark-move! only starts the hold once the render clock has ticked at least
 * once (chess-camera-tick!, which this test never drives — no camera script
 * runs headless), so a move here must leave chess-cam-holding? at 0 rather
 * than arming a timer nothing will ever advance past.
 */
static void test_camera_hold_guarded_headless(void)
{
	reset_rules_board();

	assert(cam_holding() == 0);      /* nothing armed at the start */
	assert(click(0) == 0);           /* pick up wP-e2 */
	assert(click(1) == 1);           /* slide it to e4 */
	assert(cam_holding() == 0);      /* still not armed: no clock ticked */
}

/* What the stand-in recorder below last heard: 0 nothing, 1 blip, 2 beep. */
static int32_t sound(void)
{
	return scene_script_call(&w, NULL, "chess-test-sound", 0);
}

static void sound_clear(void)
{
	scene_script_call(&w, NULL, "chess-test-sound-clear", 0);
}

/*
 * The move cue. chess.c used to read chess-on-selected's return code in C and
 * pick a sound from it; the rules now do that themselves through audio-play!
 * (#979), which is the same call the C made through the audio api, one layer
 * closer to the thing that knows what happened.
 *
 * No audio subsystem is up here, so the primitive is genuinely absent and the
 * board plays silently — the guard that makes that a no-op rather than a fault
 * is checked by every other rules test in this file passing. Standing in for it
 * with a recorder of the same name is what proves the MAPPING survived the
 * move: a pick is silent, a slide taps (blip at 0.6), a capture is the sharper
 * cue (beep at 0.7). Defined here rather than in chess.scm because a game must
 * not carry a stub for a primitive the engine owns.
 */
static void test_sound_cues(void)
{
	assert(script_eval(
		"(define *chess-test-sound* \"\")"
		"(define (audio-play! path . rest)"
		"  (set! *chess-test-sound* path))"
		"(define (chess-test-sound ignored)"
		"  (cond ((string=? *chess-test-sound* \"builtin://sound/blip\") 1)"
		"        ((string=? *chess-test-sound* \"builtin://sound/beep\") 2)"
		"        (else 0)))"
		"(define (chess-test-sound-clear ignored)"
		"  (set! *chess-test-sound* \"\") 0)") == 0);

	reset_rules_board();
	sound_clear();
	assert(click(0) == 0);           /* picking a piece up moves nothing */
	assert(sound() == 0);            /* so it is silent */
	assert(click(1) == 1);           /* slide it to e4 */
	assert(sound() == 1);            /* a soft wood tap */

	reset_rules_board();
	sound_clear();
	assert(click(0) == 0);
	assert(click(2) == 2);           /* take the black pawn */
	assert(sound() == 2);            /* the sharper cue */

	/* A click that resolves to nothing stays silent. */
	reset_rules_board();
	sound_clear();
	assert(click(2) == 0);           /* black piece, white to move */
	assert(sound() == 0);
}

/* The entity id carrying NAME, asserted to exist. */
static uint32_t id_named(const char *name)
{
	uint32_t e;

	for (e = 0; e < w.count; e++) {
		const char *n = world_entity_name(&w, e);

		if (n && strcmp(n, name) == 0)
			return e;
	}
	assert(0 && "no such entity");
	return 0;
}

/*
 * The camera script's exact text. It used to be seeded by the engine as
 * builtin://script/chess-camera (world/asset/include/asset/builtin_scripts.h);
 * chess.scm now registers this same text itself through script-define!. Pinned
 * here so the move cannot quietly become a rewrite — a different script is a
 * different camera.
 */
static const char *CHESS_CAMERA_SRC =
	"(script chess-camera\n"
	"  (on-tick (self t)\n"
	"    (chess-camera-tick! self t)))\n";

#define CHESS_CAMERA_PATH "project://script/chess-camera"

/*
 * The camera end to end, over the real catalog: chess.scm registered its script
 * when it was evaluated, the scene clause's (script ...) resolves that path, and
 * the entity-script driver ticks it. The engine seeded nothing for this — the
 * whole chain exists because the game brought it.
 *
 * The behaviour asserted is what is on screen: the eye rests at white's
 * authored 3/4 view while white is to move, and eases across to black's
 * mirrored view once black is. The authored transform is never touched.
 */
static void test_camera_defined_binds_and_eases(void)
{
	struct asset_info info;
	uint32_t          cam;
	float             t;
	int               i;

	/* The script is in the catalog because the game's source put it there. */
	assert(open_chess() == CHESS_ENTITY_COUNT);
	cam = id_named("Camera");
	assert(w.mask[cam] & COMPONENT_SCRIPT);
	assert(w.script_ref[cam] != 0);
	assert(asset_catalog_find(w.script_ref[cam], &info) == 0);
	assert(strcmp(info.path, CHESS_CAMERA_PATH) == 0);
	assert(strcmp((const char *)asset_catalog_get_data(info.id, NULL),
		      CHESS_CAMERA_SRC) == 0);

	/* No chess-reset call here: the load already ran it, since that is the
	 * project's (on-load ...) hook. */

	/* White to move: the eye holds the authored 3/4 view it starts at. */
	for (i = 0, t = 0.0f; i < 30; i++) {
		t += 0.016f;
		world_tick(&w, 16.0f);
		entity_script_tick(&w, &catalog_api, t);
	}
	assert(fabsf(w.world_xform[cam].position[0] - 5.5f) < 1e-4f);
	assert(fabsf(w.world_xform[cam].position[1] - 8.5f) < 1e-4f);
	assert(fabsf(w.world_xform[cam].position[2] - 10.5f) < 1e-4f);

	/* A white pawn move hands the turn to black. */
	assert(scene_script_call(&w, &catalog_api, "chess-on-selected",
				 (int32_t)id_named("wP-e2")) == 0);
	assert(scene_script_call(&w, &catalog_api, "chess-on-selected",
				 (int32_t)id_named("sq-e4")) == 1);
	assert(scene_script_call(&w, &catalog_api, "chess-turn", 0) == 2);

	/* Given time to ease — past the post-move hold — it lands on black's
	 * eye, the mirror of white's across the board's centre. */
	for (i = 0; i < 600; i++) {
		t += 0.016f;
		world_tick(&w, 16.0f);
		entity_script_tick(&w, &catalog_api, t);
	}
	assert(fabsf(w.world_xform[cam].position[0] + 5.5f) < 0.05f);
	assert(fabsf(w.world_xform[cam].position[1] - 8.5f) < 0.05f);
	assert(fabsf(w.world_xform[cam].position[2] + 10.5f) < 0.05f);

	/* The authored rest pose never moved: only the render pose animates. */
	assert(fabsf(w.local[cam].position[0] - 5.5f) < 1e-4f);
	assert(fabsf(w.local[cam].position[2] - 10.5f) < 1e-4f);
}

/* ------------------------------------------------------------------ */
/* The piece meshes this game brings with it                           */
/* ------------------------------------------------------------------ */

#define EPS 1.0e-5f

static const struct memory_api test_mem_impl = {
	mem_alloc, mem_alloc_zero, mem_free,
	mem_pool_create, mem_pool_alloc, mem_pool_free, mem_pool_destroy,
};
static const struct memory_api *g_mem = &test_mem_impl;

/* The stable catalog id at PATH, or 0 when nothing is there. */
static uint32_t asset_id_at(const char *path)
{
	struct asset_info info;
	uint32_t          i, n = asset_catalog_count();

	for (i = 0; i < n; i++) {
		if (asset_catalog_info(i, &info) == 0 && info.path
		    && strcmp(info.path, path) == 0)
			return info.id;
	}
	return 0;
}

static float vlen(const float *v)
{
	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

/* Radius of a vertex from the Y axis — a turned piece's natural extent. */
static float radius_xz(const struct mesh_vertex *v)
{
	return sqrtf(v->position[0] * v->position[0]
		     + v->position[2] * v->position[2]);
}

/*
 * Resolve the mesh registered at PATH to a real mesh_blob and check the shared
 * contract: the expected vertex/index counts, a whole number of triangles,
 * every index in range, every normal unit length. The counts are the geometry's
 * fingerprint — they move only when a silhouette in rules.scm does.
 *
 * These checks used to live in world/asset/mesh_script_test.c against the
 * CHESS_*_MESH_SCRIPT_SRC macros the engine seeded. The sources are this game's
 * own now, so the coverage is too — and it runs over the catalog bytes
 * mesh-define! actually stored, which is a strictly stronger claim than
 * checking a macro.
 */
static struct mesh_blob *piece_blob(const char *path, uint32_t exp_v,
				    uint32_t exp_i)
{
	uint32_t                  id = asset_id_at(path);
	const char               *src;
	struct mesh_blob         *b;
	const struct mesh_vertex *v;
	const uint16_t           *idx;
	uint32_t                  size = 0, i;

	assert(id != 0);
	src = (const char *)asset_catalog_get_data(id, NULL);
	assert(src);
	b = mesh_script_generate(src, NULL, 0, g_mem, &size);
	assert(b != NULL);
	assert(size == mesh_blob_size(exp_v, exp_i));
	assert(b->magic        == MESH_BLOB_MAGIC);
	assert(b->vertex_count == exp_v);
	assert(b->index_count  == exp_i);
	assert(b->index_count % 3 == 0);

	v   = mesh_blob_vertices(b);
	idx = mesh_blob_indices(b);
	for (i = 0; i < exp_i; i++)
		assert(idx[i] < exp_v);
	for (i = 0; i < exp_v; i++)
		assert(fabsf(vlen(v[i].normal) - 1.0f) <= EPS);
	return b;
}

/* A lathed piece's own envelope: a foot on y = 0, within MAXR and MAXY. */
static void check_piece(const char *path, uint32_t nv, uint32_t ni,
			float maxr, float maxy)
{
	struct mesh_blob         *b = piece_blob(path, nv, ni);
	const struct mesh_vertex *v = mesh_blob_vertices(b);
	uint32_t                  i;

	for (i = 0; i < nv; i++) {
		assert(radius_xz(&v[i]) <= maxr + EPS);
		assert(v[i].position[1] >= -EPS);       /* foot sits on y = 0 */
		assert(v[i].position[1] <= maxy + EPS);
	}
	g_mem->free(b);
}

/*
 * The five turned pieces — pawn, rook, bishop, queen, king — each a single
 * silhouette swept by mesh-lathe, plus the blocky knight. The engine seeds none
 * of them: they are in the catalog because chess.scm's mesh-define! calls put
 * them there when the project source was evaluated, as authored ASSET_TYPE_MESH
 * entries.
 *
 * The fingerprints are the same numbers the engine's own test asserted while
 * the sources were seeded, so "unchanged on screen" is checked, not assumed —
 * a silhouette that drifted in either move would move a count.
 */
static void test_piece_meshes_defined(void)
{
	struct asset_info info;
	struct mesh_blob *b;
	const struct mesh_vertex *v;
	uint32_t          i;
	float             maxz = -1.0e9f;

	/* Authored, not seeded: the game put these in the catalog itself. */
	assert(asset_catalog_find(asset_id_at("project://mesh/chess-pawn"),
				  &info) == 0);
	assert(info.type      == ASSET_TYPE_MESH);
	assert(info.origin    == ASSET_ORIGIN_AUTHORED);
	assert(info.read_only == 0);

	check_piece("project://mesh/chess-pawn",   375, 2016, 0.27f, 0.80f);
	check_piece("project://mesh/chess-rook",   375, 2016, 0.30f, 0.82f);
	check_piece("project://mesh/chess-bishop", 425, 2304, 0.26f, 1.17f);
	check_piece("project://mesh/chess-queen",  475, 2592, 0.30f, 1.34f);
	check_piece("project://mesh/chess-king",   475, 2592, 0.31f, 1.44f);

	/*
	 * The knight is the one non-revolved piece — a lathed pedestal fused
	 * with two tilted boxes — so it owes an envelope of its own: a foot on
	 * y = 0 and a muzzle that really does lean forward into +Z.
	 */
	b = piece_blob("project://mesh/chess-knight", 273, 1224);
	v = mesh_blob_vertices(b);
	for (i = 0; i < b->vertex_count; i++) {
		assert(radius_xz(&v[i]) <= 0.45f + EPS);
		assert(v[i].position[1] >= -EPS);
		assert(v[i].position[1] <= 0.93f + EPS);
		if (v[i].position[2] > maxz)
			maxz = v[i].position[2];
	}
	assert(maxz > 0.3f);
	g_mem->free(b);
}

/*
 * The board on screen: every piece entity in the built scene resolves its
 * (mesh ...) clause to the game's own catalog entry, and the squares still
 * resolve to the engine's plane. Binding is what the seeds used to buy; this is
 * the check that mesh-define! bought it back.
 */
static void test_scene_binds_piece_meshes(void)
{
	static const struct {
		const char *entity;
		const char *path;
	} PIECES[] = {
		{ "wP-e2", "project://mesh/chess-pawn"   },
		{ "wR-a1", "project://mesh/chess-rook"   },
		{ "wN-b1", "project://mesh/chess-knight" },
		{ "wB-c1", "project://mesh/chess-bishop" },
		{ "wQ-d1", "project://mesh/chess-queen"  },
		{ "bK-e8", "project://mesh/chess-king"   },
	};
	uint32_t i, e;

	assert(open_chess() == CHESS_ENTITY_COUNT);

	for (i = 0; i < sizeof(PIECES) / sizeof(PIECES[0]); i++) {
		e = id_named(PIECES[i].entity);
		assert(w.mask[e] & COMPONENT_RENDER);
		assert(w.render_ref[e] != 0);
		assert(w.render_ref[e] == asset_id_at(PIECES[i].path));
	}

	/* The squares are still the engine's plane — only the pieces moved. */
	e = id_named("sq-a1");
	assert(w.render_ref[e] == asset_id_at("builtin://mesh/plane"));
}

/*
 * Reloading the game re-evaluates its source, which re-runs every mesh-define!.
 * That must replace in place: the ids the live scene is bound to have to
 * survive, or a reload would leave every piece pointing at a dead asset. This
 * is the redefinition contract, checked where it actually matters.
 */
static void test_reload_keeps_piece_bindings(void)
{
	uint32_t before[6], i, e;
	static const char *PATHS[] = {
		"project://mesh/chess-pawn",   "project://mesh/chess-rook",
		"project://mesh/chess-knight", "project://mesh/chess-bishop",
		"project://mesh/chess-queen",  "project://mesh/chess-king",
	};

	assert(open_chess() == CHESS_ENTITY_COUNT);
	for (i = 0; i < 6; i++)
		before[i] = asset_id_at(PATHS[i]);
	e = id_named("wQ-d1");
	assert(w.render_ref[e] == before[4]);

	/* A reload: the whole project source evaluated again, which is how a
	 * project reloads. It keeps its launcher slot rather than stacking a
	 * second "Chess" beside the first. */
	assert(project_host_eval(CHESS_SCM) == g_chess);

	for (i = 0; i < 6; i++) {
		assert(asset_id_at(PATHS[i]) == before[i]);
		assert(before[i] != 0);
	}
	/* The already-bound queen still points at live geometry. */
	assert(w.render_ref[e] == asset_id_at("project://mesh/chess-queen"));
	check_piece("project://mesh/chess-queen", 475, 2592, 0.30f, 1.34f);
}

/* ------------------------------------------------------------------ */
/* The army materials this game brings with it                         */
/* ------------------------------------------------------------------ */

/* The float stored at byte OFF of an asset's bytes. */
static float material_float(uint32_t id, uint32_t off)
{
	const uint8_t *b = (const uint8_t *)asset_catalog_get_data(id, NULL);
	float          v;

	assert(b);
	memcpy(&v, b + off, sizeof(v));
	return v;
}

/*
 * The two piece colours, packed by material-define! when the project source was
 * evaluated. The engine seeds nothing named chess, so these are in the catalog
 * only because chess.scm put them there — authored ASSET_TYPE_MATERIAL entries
 * holding the
 * same wire form the C seeder used to write: [shader-ref u32] then the pbr
 * shader's 48-byte std140 Material block.
 *
 * The values asserted here are the ones builtin://material/chess-ivory and
 * -ebony carried while the engine seeded them, at the offsets the pbr block
 * declares (base_color @0, metallic @16, roughness @20, emissive @32,
 * subsurface @44, all plus the 4-byte header). That is what "the pieces are
 * unchanged on screen" means for a material: identical bytes reach the Material
 * UBO, so identical pixels come out.
 */
static void test_army_materials_defined(void)
{
	static const struct {
		const char *path;
		float       rgba[4];
		float       roughness;
		float       subsurface;
	} ARMIES[] = {
		{ "project://material/chess-ivory",
		  { 0.90f, 0.85f, 0.74f, 1.0f }, 0.32f, 0.6f },
		{ "project://material/chess-ebony",
		  { 0.06f, 0.055f, 0.05f, 1.0f }, 0.28f, 0.0f },
	};
	uint32_t          pbr = asset_id_at("builtin://shader/pbr");
	struct asset_info info;
	uint32_t          i, c, size;

	assert(pbr != 0);
	for (i = 0; i < sizeof(ARMIES) / sizeof(ARMIES[0]); i++) {
		uint32_t id = asset_id_at(ARMIES[i].path);

		assert(id != 0);
		assert(asset_catalog_find(id, &info) == 0);
		assert(info.type      == ASSET_TYPE_MATERIAL);
		assert(info.origin    == ASSET_ORIGIN_AUTHORED);
		assert(info.read_only == 0);

		size = 0;
		assert(asset_catalog_get_data(id, &size) != NULL);
		assert(size == 4u + 48u);
		{
			uint32_t ref;

			memcpy(&ref, asset_catalog_get_data(id, NULL),
			       sizeof(ref));
			assert(ref == pbr);
		}
		for (c = 0; c < 4; c++)
			assert(material_float(id, 4u + c * 4u)
			       == ARMIES[i].rgba[c]);
		assert(material_float(id, 20) == 0.0f);   /* matte dielectric */
		assert(material_float(id, 24) == ARMIES[i].roughness);
		assert(material_float(id, 36) == 0.0f);   /* emissive stays 0 */
		assert(material_float(id, 40) == 0.0f);
		assert(material_float(id, 44) == 0.0f);
		assert(material_float(id, 48) == ARMIES[i].subsurface);
	}
}

/*
 * The board on screen: every white piece binds the game's own ivory and every
 * black one its ebony, while the squares still bind the engine's generic
 * board-light / board-dark. Binding is what the seeds used to buy; this is the
 * check that material-define! bought it back — and that unseeding the two piece
 * materials did not take the board's with it.
 */
static void test_scene_binds_army_materials(void)
{
	static const struct {
		const char *entity;
		const char *path;
	} BOUND[] = {
		{ "wP-e2",  "project://material/chess-ivory" },
		{ "wK-e1",  "project://material/chess-ivory" },
		{ "bQ-d8",  "project://material/chess-ebony" },
		{ "bN-g8",  "project://material/chess-ebony" },
		{ "sq-a1",  "builtin://material/board-dark"  },
		{ "sq-b1",  "builtin://material/board-light" },
		{ "ground", "builtin://material/pbr-stone"   },
	};
	uint32_t i, e;

	assert(open_chess() == CHESS_ENTITY_COUNT);

	for (i = 0; i < sizeof(BOUND) / sizeof(BOUND[0]); i++) {
		e = id_named(BOUND[i].entity);
		assert(w.material_ref[e] != 0);
		assert(w.material_ref[e] == asset_id_at(BOUND[i].path));
	}

	/* Nothing named chess is seeded on chess's behalf any more. */
	assert(asset_id_at("builtin://material/chess-ivory") == 0);
	assert(asset_id_at("builtin://material/chess-ebony") == 0);
}

/*
 * Reloading the game re-evaluates its source, which re-runs both
 * material-define! calls. That must repack in place: the ids the live scene is
 * bound to have to survive, or a reload would leave every piece pointing at a
 * dead material and drawing with the default pipeline.
 */
static void test_reload_keeps_army_bindings(void)
{
	uint32_t ivory, ebony, e;

	assert(open_chess() == CHESS_ENTITY_COUNT);
	ivory = asset_id_at("project://material/chess-ivory");
	ebony = asset_id_at("project://material/chess-ebony");
	assert(ivory != 0 && ebony != 0);
	e = id_named("wQ-d1");
	assert(w.material_ref[e] == ivory);

	/* A reload: the whole project source evaluated again, which is how a
	 * project reloads. */
	assert(project_host_eval(CHESS_SCM) == g_chess);

	assert(asset_id_at("project://material/chess-ivory") == ivory);
	assert(asset_id_at("project://material/chess-ebony") == ebony);
	assert(w.material_ref[e] == ivory);
	test_army_materials_defined();
}

int main(void)
{
	mem_init();
	log_init();
	asset_init();         /* the real catalog the camera script lands in */
	script_init();       /* loads scene_script.scm (scene-build) */
	entity_script_init(); /* the entity-* primitives a script clause calls */
	/* Bind the catalog before the project source is evaluated: its
	 * top-level script-define! is what puts the camera script in it. */
	scene_script_bind_catalog(&catalog_api, &mut_api);
	scene_script_init(); /* registers the scene-* host primitives */
	world_reset(&w);

	/*
	 * The project host, brought up over a manager holding the stand-in
	 * "scene" api — the same two steps engine.c takes, minus the rest of
	 * the engine. Evaluating chess.scm then does everything chess.c's
	 * register-time half did: loads the rules into the shared image,
	 * registers the game's assets, and puts "Chess" on the launcher.
	 */
	subsystem_manager_init(&mgr, t_subsystems);
	project_host_plugin_entry(&mgr);
	g_chess = project_host_eval(CHESS_SCM);
	assert(g_chess >= 0);
	assert(game_count() == 1);
	assert(game_find("Chess") == g_chess);

	test_scene_builds();
	test_starting_position();
	test_rebuild_is_stable();
	test_move_and_turn();
	test_capture();
	test_repick();
	test_wrong_side_ignored();
	test_outline_tracks_pick();
	test_camera_hold_guarded_headless();
	test_sound_cues();
	test_camera_defined_binds_and_eases();
	test_piece_meshes_defined();
	test_scene_binds_piece_meshes();
	test_reload_keeps_piece_bindings();
	test_army_materials_defined();
	test_scene_binds_army_materials();
	test_reload_keeps_army_bindings();

	printf("chess_test: ok\n");
	return 0;
}
