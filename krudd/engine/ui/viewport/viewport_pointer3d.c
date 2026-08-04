/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * pointer3d — the world-space pointer's bridge to the scene (#996).
 *
 * The twin of viewport.c's click-to-pick, for a pointer that is already a ray.
 * viewport.c has a pixel and must unproject it through the camera to reach the
 * scene; a pointer held in the world arrives as an origin and a direction and
 * goes straight into the shared raycast (viewport_pick_ray). Everything past
 * that point is identical, deliberately: the same ray_tri_intersect over the
 * same generated meshes, and the same hand-off to the scene's selection, which
 * is what makes a controller press reach a loaded game's rules through the door
 * a mouse click already used. Chess's select-then-move flow is not rebound;
 * only where the ray starts changed.
 *
 * WHO PUSHES, AND WHY IT IS THAT WAY ROUND. Nothing here knows what a headset
 * is. ui/ sits BELOW xr/ in the module order (kruddmake/manifest.scm), so this
 * module may not reach up to ask "is a session running, and where is the
 * controller" — and it does not need to: the host that has pointers calls
 * DOWN into viewport_pointer3d_publish() once per frame, the same inversion
 * #994 used to keep the scene renderer from ever naming XR. Grep this
 * directory for "xr" and there is nothing to find, which is the acceptance
 * test the initiative states for itself (#987).
 *
 * wasm-only, like the rest of viewport.c: a native build hosts no games and no
 * scene to point at.
 */
#include "viewport_pointer3d.h"

#include <viewport/pointer3d.h>
#include <viewport/viewport_pick.h>

#include <core/subsystem.h>
#include <core/subsystem_manager.h>
#include <abi/asset_api.h>
#include <abi/edit_api.h>
#include <abi/entity_api.h>
#include <abi/memory_api.h>
#include <abi/log_api.h>
#include <abi/log_level.h>
#include <entity/world.h>
#include <math/math_types.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const struct log_api    *g_log;
static const struct entity_api *g_scene;
static const struct asset_api  *g_asset;
static const struct memory_api *g_mem;
static const struct edit_api   *g_edit;   /* NULL = no undo history to gate */

/* This frame's pointers, exactly as the last publish left them. */
static struct pointer3d_ray g_rays[POINTER3D_MAX];
static int32_t              g_count;

/*
 * The rod drawn along each pointer, as an entity id (-1 = none yet).
 *
 * A pick with no visible ray is unusable in a headset — you cannot aim at what
 * you cannot see — so each live pointer gets a thin box stretched along it.
 * The engine has no immediate-mode world-space line to draw instead: everything
 * the scene renderer draws is an entity, so the debug ray is an entity too.
 *
 * These are why viewport_pick_ray takes an ignore list. The rod lies ALONG the
 * ray that cast it, starting at its origin, so without one every pick would
 * strike the pointer's own body at t ~ 0 and nothing else would ever be
 * selected.
 *
 * It trails the hand by one frame, because the scene subsystem propagates local
 * transforms to world ones in ITS tick, and that tick has already run by the
 * time this one sets them. A frame of lag on a debug affordance is worth
 * strictly less than reordering the engine's subsystems to remove it; the PICK
 * is not lagged, because it reads the published ray and not the rod.
 */
static int32_t  g_rod[POINTER3D_MAX] = { -1, -1 };
static uint32_t g_rod_mesh;      /* builtin://mesh/box */
static uint32_t g_rod_material;  /* builtin://material/pbr-metal */
static int32_t  g_assets_looked_up;

/*
 * How long the rod is drawn, and how thick, in metres (one world unit is one
 * metre — math/camera.h). Long enough to reach past a table-sized scene and
 * read as a direction rather than a stub, thin enough not to hide what it is
 * pointing at. It does not stop at the hit: the pick runs on the press, not
 * every frame (see below), so there is no per-frame hit to stop at.
 */
#define ROD_LENGTH    3.0f
#define ROD_THICKNESS 0.006f

/*
 * The rod's mesh and material, resolved once out of the asset catalog by path.
 * Built-ins the asset plugin seeds at startup, so they are always there in a
 * booted page — but looked up rather than assumed, because a catalog id is not
 * a constant and this module has no business inventing one.
 *
 * Nothing is drawn until both resolve. That is the honest failure: a debug ray
 * that cannot be built should be absent, not a mesh-less entity in the world.
 */
static void look_up_rod_assets(void)
{
	uint32_t n, i;

	if (g_assets_looked_up || !g_asset)
		return;
	g_assets_looked_up = 1;

	n = g_asset->count();
	for (i = 0; i < n; i++) {
		struct asset_info info;

		if (g_asset->info(i, &info) != 0 || !info.path)
			continue;
		if (strcmp(info.path, "builtin://mesh/box") == 0)
			g_rod_mesh = info.id;
		else if (strcmp(info.path, "builtin://material/pbr-metal") == 0)
			g_rod_material = info.id;
	}
	if ((!g_rod_mesh || !g_rod_material) && g_log)
		g_log->write(LOG_LEVEL_WARN,
			     "pointer3d: no built-in rod mesh/material; the "
			     "pointer will pick but will not be visible");
}

static int32_t entity_is_live(const struct world *w, int32_t id)
{
	return w && id >= 0 && (uint32_t)id < w->count && w->alive[id];
}

/*
 * The undo timeline is not where a cursor belongs.
 *
 * Every entity_api mutation records a reversible edit, and recording one costs
 * two whole-world snapshots. A rod that follows a hand is a mutation on every
 * single frame, so left recording it would bury a user's actual edits under a
 * headset's refresh rate and churn the history ring doing it. set_recording is
 * the engine's existing gate for exactly this ("the play-mode gate",
 * abi/edit_api.h): the change still applies, it is simply not history.
 *
 * The snapshots are still taken and thrown away, which is the part this cannot
 * fix from here — entity_api has no unrecorded setter, and adding one is a
 * change to a vtable this module does not own.
 */
static void edit_recording(int32_t on)
{
	if (g_edit && g_edit->set_recording)
		g_edit->set_recording(on);
}

/*
 * The shortest-arc rotation taking the box's +Z axis onto dir, as a quaternion
 * in the xyzw order struct transform stores. dir is unit length (the api says
 * so), so the usual (u x v, 1 + u.v) form needs no length correction before it
 * is normalised.
 *
 * The antiparallel case has no shortest arc — every half-turn about an axis in
 * the XY plane is as good — so it is answered with the half-turn about X
 * rather than left to normalise a zero quaternion.
 */
static void rotation_to(float out[4], const float dir[3])
{
	float len;

	if (dir[2] < -0.999999f) {
		out[0] = 1.0f;
		out[1] = 0.0f;
		out[2] = 0.0f;
		out[3] = 0.0f;
		return;
	}

	/* cross((0, 0, 1), dir) */
	out[0] = -dir[1];
	out[1] = dir[0];
	out[2] = 0.0f;
	out[3] = 1.0f + dir[2];

	len = sqrtf(out[0] * out[0] + out[1] * out[1] +
		    out[2] * out[2] + out[3] * out[3]);
	if (len <= 0.0f) {
		out[0] = 0.0f;
		out[1] = 0.0f;
		out[2] = 0.0f;
		out[3] = 1.0f;
		return;
	}
	out[0] /= len;
	out[1] /= len;
	out[2] /= len;
	out[3] /= len;
}

/* The rod's transform for a ray: a unit box stretched along it from origin. */
static void rod_transform(struct transform *out,
			  const struct pointer3d_ray *ray)
{
	int32_t i;

	for (i = 0; i < 3; i++)
		out->position[i] = ray->origin[i] +
				   ray->dir[i] * (ROD_LENGTH * 0.5f);
	rotation_to(out->rotation, ray->dir);
	out->scale[0] = ROD_THICKNESS;
	out->scale[1] = ROD_THICKNESS;
	out->scale[2] = ROD_LENGTH;
}

/*
 * Show slot i's rod along its ray, creating it if this is the first frame it
 * has been needed or if a scene change tombstoned the last one (clear_world
 * takes the whole world down, and these entities go with it).
 */
static void rod_show(const struct world *w, int32_t i)
{
	struct transform t;

	if (!g_rod_mesh || !g_rod_material)
		return;

	rod_transform(&t, &g_rays[i]);
	if (!entity_is_live(w, g_rod[i])) {
		g_rod[i] = g_scene->create_entity(-1, &t, 0, g_rod_mesh);
		if (g_rod[i] < 0)
			return;
		g_scene->set_material_ref(g_rod[i], g_rod_material);
		g_scene->set_name(g_rod[i], i == 0 ? "Pointer Ray 1"
						   : "Pointer Ray 2");
		return;
	}
	/*
	 * Re-bind the mesh as well as moving it: a slot that was hidden by
	 * rod_hide() still exists, and unbinding the mesh is how it was hidden.
	 */
	if (!(w->mask[g_rod[i]] & COMPONENT_RENDER))
		g_scene->set_render_ref(g_rod[i], g_rod_mesh);
	g_scene->set_transform(g_rod[i], &t);
}

/*
 * Take slot i's rod out of the frame. Unbound rather than destroyed: destroying
 * clears a selection it tombstones, and a controller going quiet for a frame
 * must not silently drop the piece a player is holding. An entity with no
 * COMPONENT_RENDER is neither drawn nor a pick candidate, which is the whole of
 * what "hidden" has to mean here.
 */
static void rod_hide(const struct world *w, int32_t i)
{
	if (!entity_is_live(w, g_rod[i]))
		return;
	if (w->mask[g_rod[i]] & COMPONENT_RENDER)
		g_scene->set_render_ref(g_rod[i], 0);
}

/*
 * The press, resolved. Raycast every clicking pointer and hand the hit to the
 * scene's shared selection — including -1 on a miss, so a game's edge detection
 * resets exactly as it does when a canvas click lands on nothing.
 *
 * On the CLICK EDGE ONLY, and not every frame. The raycast lowers every render
 * entity's mesh through the mesh script interpreter each time it runs; that is
 * affordable once per press and is not affordable at ninety frames a second,
 * so this module reports what a click hit and does not hover-test. The api says
 * so where `hit` is declared.
 */
static void resolve_clicks(const struct world *w)
{
	int32_t i;

	for (i = 0; i < g_count; i++) {
		g_rays[i].hit = -1;
		if (!g_rays[i].click)
			continue;
		g_rays[i].hit = viewport_pick_ray(w, g_rays[i].origin,
						  g_rays[i].dir, g_rod,
						  POINTER3D_MAX, g_asset,
						  g_mem);
		g_scene->set_selected(g_rays[i].hit);
	}
}

/*
 * Whether the rods have anything to do this frame: a pointer to follow, or one
 * still drawn that no longer has one. A flat page with no spatial pointer has
 * neither, and this is what keeps it from paying for a feature it is not using
 * — including the catalog scan, which then never happens at all.
 */
static int32_t rods_need_work(const struct world *w)
{
	int32_t i;

	if (g_count > 0)
		return 1;
	for (i = 0; i < POINTER3D_MAX; i++) {
		if (entity_is_live(w, g_rod[i]) &&
		    (w->mask[g_rod[i]] & COMPONENT_RENDER))
			return 1;
	}
	return 0;
}

static void pointer3d_tick(void)
{
	const struct world *w;
	int32_t             i;

	if (!g_scene || !g_asset || !g_mem)
		return;
	w = g_scene->get_world();
	if (!w)
		return;

	resolve_clicks(w);
	if (!rods_need_work(w))
		return;

	look_up_rod_assets();
	edit_recording(0);
	for (i = 0; i < POINTER3D_MAX; i++) {
		if (i < g_count)
			rod_show(w, i);
		else
			rod_hide(w, i);
	}
	edit_recording(1);
}

void viewport_pointer3d_publish(const struct pointer3d_ray *rays, int32_t count)
{
	int32_t i;

	if (!rays || count < 0)
		count = 0;
	if (count > POINTER3D_MAX)
		count = POINTER3D_MAX;

	for (i = 0; i < count; i++) {
		g_rays[i]     = rays[i];
		g_rays[i].hit = -1;
	}
	g_count = count;
}

void viewport_pointer3d_clear(void)
{
	viewport_pointer3d_publish(NULL, 0);
}

static int32_t pointer3d_count(void)
{
	return g_count;
}

static int32_t pointer3d_get(int32_t index, struct pointer3d_ray *out)
{
	if (!out || index < 0 || index >= g_count)
		return -1;
	*out = g_rays[index];
	return 0;
}

static const struct pointer3d_api api = {
	.count = pointer3d_count,
	.get   = pointer3d_get,
};

static void pointer3d_init(void)
{
	if (g_log)
		g_log->write(LOG_LEVEL_INFO, "pointer3d: init");
}

static const struct subsystem desc = {
	.name = "pointer3d",
	.api  = &api,
	.init = pointer3d_init,
	.tick = pointer3d_tick,
};

void viewport_pointer3d_register(struct subsystem_manager *mgr)
{
	g_log   = subsystem_manager_get_api(mgr, "log");
	g_scene = subsystem_manager_get_api(mgr, "scene");
	g_asset = subsystem_manager_get_api(mgr, "asset");
	g_mem   = subsystem_manager_get_api(mgr, "memory");
	g_edit  = subsystem_manager_get_api(mgr, "edit");

	/*
	 * Registered whether or not anything will ever publish a pointer. A
	 * consumer that has to guard a NULL api before asking a question every
	 * frame will forget to, and "there are no pointers" is a number this
	 * can answer perfectly well from a desktop page with no headset in the
	 * building.
	 */
	subsystem_manager_register(mgr, &desc);
}
