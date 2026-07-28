/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The boundary, driven end to end without a browser, a wasm module or a
 * subsystem manager: a hand-built world, two fake service vtables, a tape in,
 * a JSON document out.
 *
 * The assertions are strstr against the reply rather than a parsed tree,
 * because the reply's exact text *is* the contract — the editor's client
 * JSON.parses these bytes, and a test that reparses them with a different
 * reader would pass on documents no browser accepts. script_layout_json_test
 * asserts on its JSON the same way and for the same reason.
 */
#include "bridge.h"

#include "world.h"
#include "asset_api.h"
#include "camera_api.h"
#include "gizmo.h"
#include "viewport_api.h"
#include "script.h"
#include "log.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 * A fake scene service over a plain world
 * ------------------------------------------------------------------ */

static struct world g_world;
static int32_t      g_paused;

static const struct world *fake_get_world(void)
{
	return &g_world;
}

static int32_t fake_create(int32_t parent, const struct transform *local,
			   uint32_t mask, uint32_t render_ref)
{
	int32_t id;

	if (g_world.count >= WORLD_MAX_ENTITIES)
		return -1;
	if (parent >= 0 &&
	    ((uint32_t)parent >= g_world.count || !g_world.alive[parent]))
		return -1;
	id = (int32_t)g_world.count++;
	g_world.alive[id]        = 1;
	g_world.mask[id]         = mask;
	g_world.parent[id]       = parent;
	g_world.local[id]        = *local;
	g_world.world_xform[id]  = *local;
	g_world.name_off[id]     = WORLD_NO_NAME;
	g_world.render_ref[id]   = render_ref;
	g_world.material_ref[id] = 0;
	g_world.script_ref[id]   = 0;
	return id;
}

static int32_t live(int32_t id)
{
	return id >= 0 && (uint32_t)id < g_world.count && g_world.alive[id];
}

static void fake_destroy(int32_t id)
{
	uint32_t i;

	if (!live(id))
		return;
	g_world.alive[id] = 0;
	for (i = 0; i < g_world.count; i++)
		if (g_world.parent[i] == id)
			fake_destroy((int32_t)i);
	if (g_world.selected == id)
		g_world.selected = -1;
}

static void fake_set_transform(int32_t id, const struct transform *local)
{
	if (!live(id))
		return;
	g_world.local[id]       = *local;
	g_world.world_xform[id] = *local;
}

static void fake_set_name(int32_t id, const char *name)
{
	size_t n;

	if (!live(id))
		return;
	if (!name || !*name) {
		g_world.name_off[id] = WORLD_NO_NAME;
		return;
	}
	n = strlen(name) + 1;
	if (g_world.name_bytes + n >= WORLD_NAME_BYTES)
		return;
	g_world.name_off[id] = g_world.name_bytes;
	memcpy(g_world.names + g_world.name_bytes, name, n);
	g_world.name_bytes += (uint32_t)n;
}

static void fake_set_render_ref(int32_t id, uint32_t ref)
{
	if (live(id))
		g_world.render_ref[id] = ref;
}

static void fake_set_material_ref(int32_t id, uint32_t ref)
{
	if (live(id))
		g_world.material_ref[id] = ref;
}

static void fake_set_script_ref(int32_t id, uint32_t ref)
{
	if (live(id))
		g_world.script_ref[id] = ref;
}

static int32_t fake_get_selected(void)
{
	return g_world.selected;
}

static void fake_set_selected(int32_t id)
{
	if (id < 0)
		g_world.selected = -1;
	else if (live(id))
		g_world.selected = id;
}

static int32_t fake_get_paused(void)
{
	return g_paused;
}

static void fake_set_paused(int32_t p)
{
	g_paused = p != 0;
}

/* ------------------------------------------------------------------ *
 * A catalog with one real mesh script, for the inspector's derivation
 * ------------------------------------------------------------------ */

/*
 * A genuine (params ...) clause, parsed by the real s7 reader.
 *
 * Hand-written rather than borrowed from builtin_mesh_scripts.h so the three
 * shapes under test sit side by side: a range field with a default, a colour
 * field with none, and an int. What is being asserted is that the authored
 * (edit ...) hint reaches the editor verbatim — the moment this test fakes the
 * parse, it stops testing the thing #951 exists for.
 */
static const char MESH_SRC[] =
	"(mesh probe"
	"  (params (width float (edit range 0.1 3) (default 1.5))"
	"          (tint  vec3  (edit color))"
	"          (rings int   (default 4)))"
	"  (generate () '()))";

#define MESH_ASSET_ID 42u

static const void *fake_get_data(uint32_t id, uint32_t *out_size)
{
	if (id != MESH_ASSET_ID)
		return NULL;
	if (out_size)
		*out_size = (uint32_t)sizeof(MESH_SRC);	/* NUL included */
	return MESH_SRC;
}

static int32_t fake_asset_find(uint32_t id, struct asset_info *out)
{
	if (id != MESH_ASSET_ID || !out)
		return -1;
	memset(out, 0, sizeof(*out));
	out->id   = MESH_ASSET_ID;
	out->path = "builtin://mesh/probe";
	out->type = ASSET_TYPE_MESH;
	return 0;
}

static const struct asset_api g_asset = {
	.find     = fake_asset_find,
	.get_data = fake_get_data,
};

/* What the last param write handed the entity api. */
static uint8_t  g_params[WORLD_MESH_PARAM_CAP];
static uint32_t g_params_len;

static void fake_set_mesh_params(int32_t id, const uint8_t *bytes, uint32_t len)
{
	if (!live(id))
		return;
	g_params_len = len < sizeof(g_params) ? len : sizeof(g_params);
	if (bytes && g_params_len)
		memcpy(g_params, bytes, g_params_len);
	world_set_mesh_params(&g_world, id, bytes, len);
}

/* ------------------------------------------------------------------ *
 * The document path: what a load was handed, and what a save answers
 * ------------------------------------------------------------------ */

static int32_t g_cleared;
static char    g_built[256];
static int32_t g_build_result;

static void fake_clear_world(void)
{
	g_cleared++;
}

static int32_t fake_build_scene(const char *src)
{
	snprintf(g_built, sizeof(g_built), "%s", src ? src : "");
	return g_build_result;
}

/*
 * A fixed form rather than a real serialization: what the bridge owes the
 * client is that whatever the engine wrote reaches the reply intact and
 * escaped. Whether the writer writes the right thing is scene_save_test's
 * question, and answering it twice would couple this test to that format.
 *
 * The embedded quotes are the point — they are what proves the escaping.
 */
static int32_t fake_save_scene(char *out, int32_t cap)
{
	static const char form[] = "(scene fake (entity (name \"root\")))";

	if (cap < (int32_t)sizeof(form))
		return -1;
	memcpy(out, form, sizeof(form));
	return (int32_t)sizeof(form) - 1;
}

/*
 * Only the members the bridge touches are filled in. The rest stay NULL on
 * purpose: every call site in bridge.c guards, and a test that filled them
 * would stop proving that.
 */
static const struct entity_api g_entity = {
	.build_scene_scm  = fake_build_scene,
	.save_scene_scm   = fake_save_scene,
	.clear_world      = fake_clear_world,
	.get_world        = fake_get_world,
	.create_entity    = fake_create,
	.destroy_entity   = fake_destroy,
	.set_transform    = fake_set_transform,
	.set_name         = fake_set_name,
	.set_render_ref   = fake_set_render_ref,
	.set_material_ref = fake_set_material_ref,
	.set_script_ref   = fake_set_script_ref,
	.get_selected     = fake_get_selected,
	.set_selected     = fake_set_selected,
	.get_paused       = fake_get_paused,
	.set_paused       = fake_set_paused,
	.set_mesh_params  = fake_set_mesh_params,
};

/* ------------------------------------------------------------------ *
 * A fake history that records what it was asked to do
 * ------------------------------------------------------------------ */

static int32_t     g_begins, g_commits, g_aborts, g_undos, g_redos;
static int32_t     g_can_undo, g_can_redo;
static const char *g_last_label;
/* A copy, so the test can prove the label outlived the caller's buffer. */
static char        g_label_copy[64];

static void fake_begin(const char *label)
{
	g_begins++;
	g_last_label = label;
	snprintf(g_label_copy, sizeof(g_label_copy), "%s", label ? label : "");
}

static void fake_commit(void)     { g_commits++; }
static void fake_abort(void)      { g_aborts++; }
static int32_t fake_undo(void)    { g_undos++; return g_can_undo; }
static int32_t fake_redo(void)    { g_redos++; return g_can_redo; }
static int32_t fake_can_undo(void) { return g_can_undo; }
static int32_t fake_can_redo(void) { return g_can_redo; }

static const char *fake_undo_label(void)
{
	return g_can_undo ? "Move" : NULL;
}

static const char *fake_redo_label(void)
{
	return g_can_redo ? "Rename" : NULL;
}

static int32_t g_clears;

static void fake_clear_history(void)
{
	g_clears++;
}

static const struct edit_api g_edit = {
	.begin      = fake_begin,
	.commit     = fake_commit,
	.abort      = fake_abort,
	.undo       = fake_undo,
	.redo       = fake_redo,
	.can_undo   = fake_can_undo,
	.can_redo   = fake_can_redo,
	.undo_label = fake_undo_label,
	.redo_label = fake_redo_label,
	.clear      = fake_clear_history,
};

/* ------------------------------------------------------------------ *
 * A tape builder — the encoder the TypeScript client mirrors
 * ------------------------------------------------------------------ */

struct builder {
	uint8_t  bytes[BRIDGE_TAPE_BYTES];
	int32_t  len;
	uint32_t count;
	int32_t  record;	/* offset of the open record's length field */
};

static void b_reset(struct builder *b)
{
	uint32_t magic = BRIDGE_TAPE_MAGIC;
	uint32_t zero  = 0;

	b->len   = 0;
	b->count = 0;
	memcpy(b->bytes, &magic, 4);
	memcpy(b->bytes + 4, &zero, 4);
	b->len = 8;
}

static void b_raw(struct builder *b, const void *data, int32_t n)
{
	memcpy(b->bytes + b->len, data, (size_t)n);
	b->len += n;
}

static void b_open(struct builder *b, uint16_t op)
{
	uint16_t zero = 0;

	b_raw(b, &op, 2);
	b->record = b->len;
	b_raw(b, &zero, 2);
}

static void b_close(struct builder *b)
{
	uint16_t len = (uint16_t)(b->len - b->record - 2);

	memcpy(b->bytes + b->record, &len, 2);
	b->count++;
	memcpy(b->bytes + 4, &b->count, 4);
}

static void b_i32(struct builder *b, int32_t v) { b_raw(b, &v, 4); }
static void b_f32(struct builder *b, float v) { b_raw(b, &v, 4); }
static void b_u32(struct builder *b, uint32_t v) { b_raw(b, &v, 4); }

static void b_str(struct builder *b, const char *s)
{
	uint16_t n = (uint16_t)strlen(s);

	b_raw(b, &n, 2);
	b_raw(b, s, (int32_t)n);
}

static void b_transform(struct builder *b, float x, float y, float z)
{
	float xf[10] = { x, y, z, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f };

	b_raw(b, xf, BRIDGE_TAPE_TRANSFORM_BYTES);
}

/* One-op tapes are the common shape in these tests. */
static void b_simple(struct builder *b, uint16_t op)
{
	b_open(b, op);
	b_close(b);
}

/* ------------------------------------------------------------------ *
 * Fake camera, viewport and gizmo services (#949)
 *
 * Each records what it was asked to do rather than doing it: there is no
 * renderer to orbit, no canvas to pick against and nothing to draw. What the
 * boundary owes these vtables is that the right call arrives with the right
 * numbers, and that is exactly what a recorder can check.
 * ------------------------------------------------------------------ */

static float   g_orbit_yaw, g_orbit_pitch;
static float   g_pan_dx, g_pan_dy;
static float   g_dolly;
static int32_t g_resets;
static float   g_framed[3];
static float   g_framed_radius;
static int32_t g_frames;

/*
 * The identity, written out rather than through mat4_identity: this test links
 * no math library, on purpose — the boundary does no geometry, it hands
 * co-ordinates to services that do, and linking base/math here would let a
 * future arithmetic mistake in bridge.c compile.
 */
static void fake_get_view_proj(struct mat4 *out)
{
	int i;

	for (i = 0; i < 16; i++)
		out->m[i] = i % 5 == 0 ? 1.0f : 0.0f;
}
static void fake_get_eye(float out[3]) { out[0] = out[1] = 0.0f; out[2] = 5.0f; }
static void fake_set_viewport(float w, float h) { (void)w; (void)h; }
static void fake_orbit(float yaw, float pitch)
{
	g_orbit_yaw   = yaw;
	g_orbit_pitch = pitch;
}
static void fake_pan(float dx, float dy) { g_pan_dx = dx; g_pan_dy = dy; }
static void fake_dolly(float amount) { g_dolly = amount; }
static void fake_reset_view(void) { g_resets++; }
static void fake_frame(const float centre[3], float radius)
{
	g_framed[0]     = centre[0];
	g_framed[1]     = centre[1];
	g_framed[2]     = centre[2];
	g_framed_radius = radius;
	g_frames++;
}

static const struct camera_api g_camera = {
	fake_get_view_proj, fake_get_eye,  fake_set_viewport,
	fake_orbit,         fake_pan,      fake_dolly,
	fake_reset_view,    fake_frame,
};

static float   g_vp_w, g_vp_h;
/* What the next pick answers, and where it was asked. */
static int32_t g_pick_answer = -1;
static float   g_pick_x, g_pick_y;
/* Which entity has something to frame, and how big it is. */
static int32_t g_bounds_entity = -1;

static void fake_vp_set_size(float w, float h)
{
	if (w <= 0.0f || h <= 0.0f)
		return;
	g_vp_w = w;
	g_vp_h = h;
}
static void fake_vp_get_size(float *w, float *h)
{
	if (w)
		*w = g_vp_w;
	if (h)
		*h = g_vp_h;
}
static int32_t fake_vp_pick(float sx, float sy)
{
	g_pick_x = sx;
	g_pick_y = sy;
	return g_pick_answer;
}
static int32_t fake_vp_bounds(int32_t entity, float centre[3], float *radius)
{
	if (entity != g_bounds_entity)
		return 0;
	centre[0] = 1.0f;
	centre[1] = 2.0f;
	centre[2] = 3.0f;
	*radius   = 4.0f;
	return 1;
}

static const struct viewport_api g_viewport = {
	fake_vp_set_size, fake_vp_get_size, fake_vp_pick, fake_vp_bounds,
};

static int32_t           g_gizmo_mode = GIZMO_MODE_TRANSLATE;
static struct gizmo_snap g_gizmo_snap;
static int32_t           g_gizmo_axis = GIZMO_AXIS_NONE;
static int32_t           g_gizmo_begins, g_gizmo_ends;
static int32_t           g_grid_shown = 1;
static float             g_grid_spacing = 1.0f;
/*
 * What the next move() solves to, and whether it solves at all. A real gizmo
 * answers 0 when the pointer maps to nothing usable; the boundary has to leave
 * the entity alone in that case rather than writing a stale transform.
 */
static int32_t           g_gizmo_moves_to = 1;
static float             g_gizmo_move_x = 7.0f;

static void fake_gz_set_mode(int32_t mode) { g_gizmo_mode = mode; }
static int32_t fake_gz_get_mode(void) { return g_gizmo_mode; }
static void fake_gz_set_snap(float t, float r, float s)
{
	g_gizmo_snap.translate      = t;
	g_gizmo_snap.rotate_degrees = r;
	g_gizmo_snap.scale          = s;
}
static void fake_gz_get_snap(struct gizmo_snap *out) { *out = g_gizmo_snap; }
static int32_t fake_gz_begin(float sx, float sy)
{
	(void)sx;
	(void)sy;
	g_gizmo_begins++;
	return g_gizmo_axis;
}
static int32_t fake_gz_move(float sx, float sy, struct transform *out)
{
	(void)sx;
	(void)sy;
	if (!g_gizmo_moves_to)
		return 0;
	memset(out, 0, sizeof(*out));
	out->position[0] = g_gizmo_move_x;
	out->rotation[3] = 1.0f;
	out->scale[0] = out->scale[1] = out->scale[2] = 1.0f;
	return 1;
}
static void fake_gz_end(void)
{
	g_gizmo_ends++;
	g_gizmo_axis = GIZMO_AXIS_NONE;
}
static int32_t fake_gz_axis(void) { return g_gizmo_axis; }
static void fake_gz_set_grid(int32_t shown, float spacing)
{
	g_grid_shown = shown ? 1 : 0;
	if (spacing > 0.0f)
		g_grid_spacing = spacing;
}
static int32_t fake_gz_grid_shown(void) { return g_grid_shown; }
static float fake_gz_grid_spacing(void) { return g_grid_spacing; }

static const struct gizmo_api g_gizmo = {
	fake_gz_set_mode, fake_gz_get_mode,     fake_gz_set_snap,
	fake_gz_get_snap, fake_gz_begin,        fake_gz_move,
	fake_gz_end,      fake_gz_axis,         fake_gz_set_grid,
	fake_gz_grid_shown, fake_gz_grid_spacing,
};

static int32_t g_editor_mode;
static void fake_set_editor_mode(int32_t on) { g_editor_mode = on ? 1 : 0; }
static int32_t fake_get_editor_mode(void) { return g_editor_mode; }

/* ------------------------------------------------------------------ *
 * Fixtures
 * ------------------------------------------------------------------ */

static struct bridge g_bridge;

static void reset_all(void)
{
	static const struct bridge_host host = {
		.entity          = &g_entity,
		.edit            = &g_edit,
		.asset           = &g_asset,
		.camera          = &g_camera,
		.viewport        = &g_viewport,
		.gizmo           = &g_gizmo,
		.set_editor_mode = fake_set_editor_mode,
		.get_editor_mode = fake_get_editor_mode,
	};
	struct transform origin = {
		.position = { 0.0f, 0.0f, 0.0f },
		.rotation = { 0.0f, 0.0f, 0.0f, 1.0f },
		.scale    = { 1.0f, 1.0f, 1.0f },
	};

	memset(&g_world, 0, sizeof(g_world));
	g_world.selected = -1;
	g_world.outline  = -1;
	g_paused         = 0;
	g_begins = g_commits = g_aborts = g_undos = g_redos = 0;
	g_can_undo = g_can_redo = 0;
	g_last_label = NULL;
	g_cleared = g_clears = 0;
	g_built[0] = '\0';
	g_build_result = 0;

	g_orbit_yaw = g_orbit_pitch = g_pan_dx = g_pan_dy = g_dolly = 0.0f;
	g_resets = g_frames = 0;
	g_framed_radius = 0.0f;
	g_vp_w = g_vp_h = 0.0f;
	g_pick_answer = -1;
	g_pick_x = g_pick_y = 0.0f;
	g_bounds_entity = -1;
	g_gizmo_mode = GIZMO_MODE_TRANSLATE;
	memset(&g_gizmo_snap, 0, sizeof(g_gizmo_snap));
	g_gizmo_axis = GIZMO_AXIS_NONE;
	g_gizmo_begins = g_gizmo_ends = 0;
	g_gizmo_moves_to = 1;
	g_grid_shown = 1;
	g_grid_spacing = 1.0f;
	g_editor_mode = 0;

	bridge_init(&g_bridge, &host);

	/* Two entities: a root and a child, so parentage is observable. */
	assert(fake_create(-1, &origin, 0, 0) == 0);
	assert(fake_create(0, &origin, 0, 0) == 1);
	fake_set_name(0, "root");
	fake_set_name(1, "child");
	g_params_len = 0;
	memset(g_params, 0, sizeof(g_params));
}

static const char *exchange(struct builder *b)
{
	return bridge_exchange(&g_bridge, b->bytes, b->len);
}

static void has(const char *reply, const char *needle)
{
	if (!strstr(reply, needle)) {
		printf("FAIL: expected %s\n  in: %s\n", needle, reply);
		assert(0);
	}
}

static void lacks(const char *reply, const char *needle)
{
	if (strstr(reply, needle)) {
		printf("FAIL: unexpected %s\n  in: %s\n", needle, reply);
		assert(0);
	}
}

/* ------------------------------------------------------------------ *
 * Tests
 * ------------------------------------------------------------------ */

static void test_empty_tape(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	b_reset(&b);
	r = exchange(&b);

	{
		char want[32];

		/* Derived, not spelled: this assertion is that the reply
		 * carries the build's own wire version, not that the version
		 * is any particular number. */
		snprintf(want, sizeof(want), "\"protocol\":%d", BRIDGE_PROTOCOL);
		has(r, want);
	}
	has(r, "\"serial\":1");
	has(r, "\"applied\":0");
	has(r, "\"error\":null");
	has(r, "\"results\":[]");
	/* Seeded on the first refresh; 0 is reserved for "the client has none". */
	/* Every domain seeded to 1 on the first refresh, and the vector
	 * carries all of them — #949's viewport included. */
	has(r, "\"generations\":{\"scene\":1,\"selection\":1,\"history\":1,"
	       "\"viewport\":1}");
	printf("ok: empty tape\n");
}

static void test_command_then_query_sees_the_command(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_ENTITY_TRANSFORM);
	b_i32(&b, 1);
	b_transform(&b, 3.5f, 0.0f, 0.0f);
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_ENTITY);
	b_i32(&b, 1);
	b_u32(&b, 0);
	b_close(&b);
	r = exchange(&b);

	has(r, "\"applied\":1");
	/*
	 * The point of the two-pass walk: the query is answered against the
	 * world *after* the command in the same batch, not before it.
	 */
	has(r, "\"position\":[3.5,0,0]");
	has(r, "\"name\":\"child\"");
	has(r, "\"parent\":0");
	printf("ok: a query sees the commands batched ahead of it\n");
}

static void test_query_order_does_not_matter(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	/* Query written first, command second — same answer either way. */
	b_reset(&b);
	b_open(&b, BRIDGE_OP_QUERY_ENTITY);
	b_i32(&b, 1);
	b_u32(&b, 0);
	b_close(&b);
	b_open(&b, BRIDGE_OP_ENTITY_TRANSFORM);
	b_i32(&b, 1);
	b_transform(&b, 9.0f, 0.0f, 0.0f);
	b_close(&b);
	r = exchange(&b);

	has(r, "\"position\":[9,0,0]");
	printf("ok: a query's position in the tape does not change its answer\n");
}

static void test_generation_etag(void)
{
	struct builder b;
	const char    *r;
	uint32_t       gen;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_QUERY_TREE);
	b_u32(&b, 0);
	b_close(&b);
	r = exchange(&b);
	has(r, "\"fresh\":false");
	has(r, "\"entities\":[");
	gen = bridge_generation(&g_bridge, BRIDGE_SCENE);

	/* Ask again with the generation we now hold: a hit, and no payload. */
	b_reset(&b);
	b_open(&b, BRIDGE_OP_QUERY_TREE);
	b_u32(&b, gen);
	b_close(&b);
	r = exchange(&b);
	has(r, "\"fresh\":true");
	lacks(r, "\"entities\"");
	printf("ok: a matching generation answers fresh, with no value\n");
}

static void test_change_behind_the_bridges_back(void)
{
	struct builder b;
	const char    *r;
	uint32_t       before, after;

	reset_all();
	b_reset(&b);
	r = exchange(&b);
	before = bridge_generation(&g_bridge, BRIDGE_SCENE);

	/*
	 * Nobody told the bridge. This is the C-side gizmo drag, the game
	 * script, the scene load — #944 requires the editor to hear about all
	 * three, and the fingerprint is what delivers that.
	 */
	g_world.local[1].position[1] = 12.0f;

	b_reset(&b);
	b_open(&b, BRIDGE_OP_QUERY_TREE);
	b_u32(&b, before);
	b_close(&b);
	r = exchange(&b);
	after = bridge_generation(&g_bridge, BRIDGE_SCENE);

	assert(after == before + 1);
	has(r, "\"fresh\":false");
	printf("ok: a change the editor did not make still moves the generation\n");
}

static void test_idle_frames_do_not_move_generations(void)
{
	struct builder b;
	uint32_t       first, second;

	reset_all();
	b_reset(&b);
	(void)exchange(&b);
	first = bridge_generation(&g_bridge, BRIDGE_SCENE);
	(void)exchange(&b);
	(void)exchange(&b);
	second = bridge_generation(&g_bridge, BRIDGE_SCENE);

	/* Otherwise every cache in the editor misses on every frame. */
	assert(first == second);
	printf("ok: an unchanged world holds its generation\n");
}

static void test_domains_are_independent(void)
{
	struct builder b;
	uint32_t       scene_before, sel_before;

	reset_all();
	b_reset(&b);
	(void)exchange(&b);
	scene_before = bridge_generation(&g_bridge, BRIDGE_SCENE);
	sel_before   = bridge_generation(&g_bridge, BRIDGE_SELECTION);

	b_reset(&b);
	b_open(&b, BRIDGE_OP_SELECT);
	b_i32(&b, 1);
	b_close(&b);
	(void)exchange(&b);

	assert(bridge_generation(&g_bridge, BRIDGE_SELECTION) == sel_before + 1);
	/* A selection change must not invalidate a cached scene tree. */
	assert(bridge_generation(&g_bridge, BRIDGE_SCENE) == scene_before);
	printf("ok: selection and scene generations move independently\n");
}

static void test_selection_and_history_queries(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	g_can_undo = 1;

	b_reset(&b);
	b_open(&b, BRIDGE_OP_SELECT);
	b_i32(&b, 1);
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_SELECTION);
	b_u32(&b, 0);
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_HISTORY);
	b_u32(&b, 0);
	b_close(&b);
	r = exchange(&b);

	has(r, "\"kind\":\"selection\"");
	has(r, "\"value\":{\"id\":1}");
	has(r, "\"canUndo\":true");
	has(r, "\"canRedo\":false");
	has(r, "\"undoLabel\":\"Move\"");
	has(r, "\"redoLabel\":null");
	printf("ok: selection and history queries\n");
}

static void test_gesture_brackets(void)
{
	struct builder b;
	char           label[BRIDGE_LABEL_BYTES];

	reset_all();
	snprintf(label, sizeof(label), "Move entity");

	b_reset(&b);
	b_open(&b, BRIDGE_OP_GESTURE_BEGIN);
	b_str(&b, label);
	b_close(&b);
	b_open(&b, BRIDGE_OP_ENTITY_TRANSFORM);
	b_i32(&b, 1);
	b_transform(&b, 1.0f, 0.0f, 0.0f);
	b_close(&b);
	b_simple(&b, BRIDGE_OP_GESTURE_COMMIT);
	(void)exchange(&b);

	assert(g_begins == 1);
	assert(g_commits == 1);
	assert(g_aborts == 0);
	/*
	 * The label the history kept is not the caller's buffer. edit_api
	 * requires it to outlive the entry, and a tape-borne string does not —
	 * so scribbling over the source must not change what undo is called.
	 */
	memset(label, 'x', sizeof(label) - 1);
	label[sizeof(label) - 1] = '\0';
	assert(strcmp(g_last_label, "Move entity") == 0);
	assert(strcmp(g_label_copy, "Move entity") == 0);
	printf("ok: a gesture brackets its commands, and interns its label\n");
}

static void test_gesture_commit_without_begin_is_ignored(void)
{
	struct builder b;

	reset_all();
	b_reset(&b);
	b_simple(&b, BRIDGE_OP_GESTURE_COMMIT);
	b_simple(&b, BRIDGE_OP_GESTURE_ABORT);
	(void)exchange(&b);

	/*
	 * A stray commit would close a gesture the engine opened for its own
	 * gizmo drag, and the two undo entries would merge into one.
	 */
	assert(g_commits == 0);
	assert(g_aborts == 0);
	printf("ok: an unmatched commit or abort is dropped, not forwarded\n");
}

static void test_undo_is_an_ordinary_command(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	g_can_undo = 1;
	b_reset(&b);
	b_simple(&b, BRIDGE_OP_UNDO);
	r = exchange(&b);
	assert(g_undos == 1);
	has(r, "\"events\":[]");

	g_can_undo = 0;
	b_reset(&b);
	b_simple(&b, BRIDGE_OP_UNDO);
	r = exchange(&b);
	has(r, "\"type\":\"history.empty\"");
	has(r, "nothing to undo");
	printf("ok: undo dispatches to the C stack and reports an empty one\n");
}

static void test_events_are_per_exchange(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	bridge_emit(&g_bridge, "log", 3, "hello \"world\"\n");
	b_reset(&b);
	r = exchange(&b);
	/* The quotes and the newline survive as escapes, not as raw bytes. */
	has(r, "\"text\":\"hello \\\"world\\\"\\n\"");
	has(r, "\"eventsDropped\":0");

	r = exchange(&b);
	has(r, "\"events\":[]");
	printf("ok: events are delivered once, and escaped\n");
}

static void test_event_overflow_is_counted(void)
{
	struct builder b;
	const char    *r;
	int            i;

	reset_all();
	for (i = 0; i < BRIDGE_MAX_EVENTS + 5; i++)
		bridge_emit(&g_bridge, "log", 0, "spam");
	b_reset(&b);
	r = exchange(&b);
	has(r, "\"eventsDropped\":5");
	printf("ok: dropped events are reported rather than lost quietly\n");
}

static void test_create_and_destroy(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_ENTITY_CREATE);
	b_i32(&b, 0);
	b_transform(&b, 0.0f, 2.0f, 0.0f);
	b_u32(&b, 0);
	b_u32(&b, 7);
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_TREE);
	b_u32(&b, 0);
	b_close(&b);
	r = exchange(&b);
	has(r, "\"id\":2");
	has(r, "\"render\":7");

	b_reset(&b);
	b_open(&b, BRIDGE_OP_ENTITY_DESTROY);
	b_i32(&b, 1);
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_ENTITY);
	b_i32(&b, 1);
	b_u32(&b, 0);
	b_close(&b);
	r = exchange(&b);
	/* Asking about a destroyed entity is lag, not an error. */
	has(r, "\"error\":null");
	has(r, "\"value\":null");
	printf("ok: create and destroy, and a gone entity answers null\n");
}

static void test_rename_and_paused(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_ENTITY_NAME);
	b_i32(&b, 1);
	b_str(&b, "renamed");
	b_close(&b);
	b_open(&b, BRIDGE_OP_SET_PAUSED);
	b_i32(&b, 1);
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_TREE);
	b_u32(&b, 0);
	b_close(&b);
	r = exchange(&b);

	has(r, "\"name\":\"renamed\"");
	has(r, "\"paused\":true");
	printf("ok: rename and pause\n");
}

static void test_bad_magic(void)
{
	struct builder b;
	const char    *r;
	uint32_t       wrong = 0xdeadbeefu;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_SELECT);
	b_i32(&b, 1);
	b_close(&b);
	memcpy(b.bytes, &wrong, 4);
	r = exchange(&b);

	has(r, "\"error\":\"magic\"");
	has(r, "\"applied\":0");
	has(r, "\"results\":[]");
	/* Rejected whole: the select must not have landed. */
	assert(g_world.selected == -1);
	printf("ok: a tape with the wrong magic is rejected whole\n");
}

static void test_truncated_record(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_ENTITY_TRANSFORM);
	b_i32(&b, 1);
	b_transform(&b, 1.0f, 1.0f, 1.0f);
	b_close(&b);
	b.len -= 8;	/* the tape ends mid-payload */
	r = exchange(&b);

	has(r, "\"error\":\"truncated\"");
	assert(g_world.local[1].position[0] == 0.0f);
	printf("ok: a truncated record fails the batch\n");
}

static void test_unknown_opcode(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	b_reset(&b);
	b_simple(&b, 0x00ff);
	r = exchange(&b);
	has(r, "\"error\":\"opcode\"");

	/* And one above the query threshold, which the second pass rejects. */
	b_reset(&b);
	b_simple(&b, 0x0fff);
	r = exchange(&b);
	has(r, "\"error\":\"opcode\"");
	printf("ok: an opcode this build does not know fails the batch\n");
}

static void test_payload_length_mismatch(void)
{
	struct builder b;
	const char    *r;
	uint16_t       wrong = 8;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_SELECT);
	b_i32(&b, 1);
	b_i32(&b, 99);	/* four bytes the decoder will not read */
	memcpy(b.bytes + b.record, &wrong, 2);
	b.count++;
	memcpy(b.bytes + 4, &b.count, 4);
	r = exchange(&b);

	/*
	 * The declared length and what the decoder consumed disagree, so the
	 * two sides disagree about the format and everything after this record
	 * would be garbage.
	 */
	has(r, "\"error\":\"payload\"");
	assert(g_world.selected == -1);
	printf("ok: a record whose declared length is wrong fails the batch\n");
}

/*
 * Save is a query, and it is the one query the client is expected to ask once
 * rather than watch. The fake returns a fixed form, so what is under test here
 * is the plumbing — that the text reaches the reply, JSON-escaped — and not
 * scene_save's output, which scene_save_test owns.
 */
static void test_scene_text_query(void)
{
	struct builder b;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_QUERY_SCENE_TEXT);
	b_u32(&b, 0);
	b_close(&b);

	has(exchange(&b), "\"kind\":\"scene.text\"");
	has(g_bridge.reply, "\"fresh\":false");
	/* The quotes in the form's (name "root") clause survive as escapes. */
	has(g_bridge.reply, "(scene fake (entity (name \\\"root\\\")))");
	printf("ok: scene.text answers with the saved form, escaped\n");
}

/* It is a scene-domain query, so it is an ETag like every other read. */
static void test_scene_text_is_generation_cached(void)
{
	struct builder b;
	uint32_t       gen;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_QUERY_SCENE_TEXT);
	b_u32(&b, 0);
	b_close(&b);
	exchange(&b);
	gen = bridge_generation(&g_bridge, BRIDGE_SCENE);

	b_reset(&b);
	b_open(&b, BRIDGE_OP_QUERY_SCENE_TEXT);
	b_u32(&b, gen);
	b_close(&b);
	has(exchange(&b), "\"fresh\":true");
	lacks(g_bridge.reply, "(scene fake");
	printf("ok: an unchanged scene answers scene.text fresh, with no text\n");
}

/* A build that cannot write a scene says so rather than saving nothing. */
static void test_scene_text_without_a_writer(void)
{
	static const struct entity_api no_writer = {
		.get_world = fake_get_world,
	};
	static const struct bridge_host host = { .entity = &no_writer };
	struct builder b;

	bridge_init(&g_bridge, &host);
	b_reset(&b);
	b_open(&b, BRIDGE_OP_QUERY_SCENE_TEXT);
	b_u32(&b, 0);
	b_close(&b);

	has(exchange(&b), "\"value\":null");

	/*
	 * The reason is one exchange behind, and deliberately so: the envelope
	 * (events included) is written before the query pass runs, so a notice
	 * a query emits has already missed its own reply. That is why the
	 * failure is carried by `value` and only the explanation by the event —
	 * a client that waited for the event to know it failed would save an
	 * empty file this frame and hear why on the next one.
	 */
	lacks(g_bridge.reply, "query.unavailable");
	b_reset(&b);
	b_simple(&b, BRIDGE_OP_UNDO);
	has(exchange(&b), "query.unavailable");
	printf("ok: a build with no writer answers null, and says why next frame\n");
}

static void test_scene_load_replaces_the_world(void)
{
	struct builder b;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_SCENE_LOAD);
	b_str(&b, "(scene loaded (entity))");
	b_close(&b);
	exchange(&b);

	assert(g_cleared == 1);
	assert(strcmp(g_built, "(scene loaded (entity))") == 0);
	printf("ok: a load clears the world before building into it\n");
}

/*
 * The history goes with the document. A memento captured against the old world
 * refers to entity ids that now mean something else, so undoing across a load
 * would drive the new scene to a state the old one was in.
 */
static void test_scene_load_clears_the_history(void)
{
	struct builder b;

	reset_all();
	g_clears = 0;
	b_reset(&b);
	b_open(&b, BRIDGE_OP_SCENE_LOAD);
	b_str(&b, "(scene loaded)");
	b_close(&b);
	exchange(&b);

	assert(g_clears == 1);
	printf("ok: loading a scene clears the undo history\n");
}

/* A form the interpreter rejects is a notice, not a failed batch. */
static void test_scene_load_rejection_is_an_event(void)
{
	struct builder b;

	reset_all();
	g_build_result = -1;
	b_reset(&b);
	b_open(&b, BRIDGE_OP_SCENE_LOAD);
	b_str(&b, "not-a-scene");
	b_close(&b);

	has(exchange(&b), "command.rejected");
	has(g_bridge.reply, "\"error\":null");
	g_build_result = 0;
	printf("ok: a scene the engine cannot build is reported, not fatal\n");
}

/*
 * A load and a save in one batch: the load runs in the command pass and the
 * save in the query pass, so the text that comes back describes the world the
 * load produced. The two share one scratch buffer, and this is the test that
 * says that sharing is safe.
 */
static void test_load_then_save_in_one_batch(void)
{
	struct builder b;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_SCENE_LOAD);
	b_str(&b, "(scene incoming (entity))");
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_SCENE_TEXT);
	b_u32(&b, 0);
	b_close(&b);
	exchange(&b);

	assert(strcmp(g_built, "(scene incoming (entity))") == 0);
	has(g_bridge.reply, "(scene fake");
	printf("ok: a load and a save in one batch do not share a buffer\n");
}

/*
 * The derivation, end to end: an authored (edit ...) hint reaches the editor
 * exactly as written, beside the value the engine would actually use.
 *
 * This is #951's criterion in one assertion. Nothing in the reply is
 * hand-written per asset type — it is what script-params-form reported.
 */
static void test_entity_params_derive_from_the_asset(void)
{
	struct builder b;

	reset_all();
	fake_set_render_ref(0, MESH_ASSET_ID);

	b_reset(&b);
	b_open(&b, BRIDGE_OP_QUERY_ENTITY_PARAMS);
	b_i32(&b, 0);
	b_u32(&b, 0);
	b_close(&b);

	has(exchange(&b), "\"kind\":\"entity.params\"");
	has(g_bridge.reply, "\"slot\":\"mesh\"");
	has(g_bridge.reply, "\"path\":\"builtin://mesh/probe\"");

	/* The range field: hint, bounds and its authored default as the value. */
	has(g_bridge.reply, "\"name\":\"width\"");
	has(g_bridge.reply, "\"edit\":\"range\"");
	has(g_bridge.reply, "\"min\":0.100000001");
	has(g_bridge.reply, "\"max\":3");
	has(g_bridge.reply, "\"value\":[1.5]");

	/* The colour field: three components, no bounds worth reading. */
	has(g_bridge.reply, "\"name\":\"tint\"");
	has(g_bridge.reply, "\"edit\":\"color\"");
	has(g_bridge.reply, "\"components\":3");

	/* And a field with no (edit ...) at all still arrives, with its type. */
	has(g_bridge.reply, "\"name\":\"rings\"");
	has(g_bridge.reply, "\"type\":\"int\"");
	has(g_bridge.reply, "\"edit\":\"none\"");

	/* Nothing is overridden yet — every value above is a declared default. */
	has(g_bridge.reply, "\"overridden\":false");
	printf("ok: parameters derive from the asset, hints and all\n");
}

/* An entity with nothing parameterized bound is an empty list, not null. */
static void test_entity_params_without_assets(void)
{
	struct builder b;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_QUERY_ENTITY_PARAMS);
	b_i32(&b, 0);
	b_u32(&b, 0);
	b_close(&b);

	has(exchange(&b), "\"blocks\":[]");
	printf("ok: an entity with no parameterized asset has no blocks\n");
}

/* A gone entity answers null — the same shape the entity query uses. */
static void test_entity_params_for_a_dead_entity(void)
{
	struct builder b;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_QUERY_ENTITY_PARAMS);
	b_i32(&b, 99);
	b_u32(&b, 0);
	b_close(&b);

	has(exchange(&b), "\"value\":null");
	printf("ok: parameters for an entity that is not there answer null\n");
}

/*
 * A write packs at the declaration's offset, and leaves its neighbours at the
 * values they were already resolved to.
 *
 * The second half is the one that matters: rebuilding the block from resolved
 * values rather than from the entity's (absent) override is what stops editing
 * `width` from silently flattening `rings` to zero.
 */
static void test_entity_param_write(void)
{
	struct builder b;
	float          width = 0.0f;
	int32_t        rings = 0;

	reset_all();
	fake_set_render_ref(0, MESH_ASSET_ID);

	b_reset(&b);
	b_open(&b, BRIDGE_OP_ENTITY_PARAM);
	b_i32(&b, 0);	/* entity */
	b_i32(&b, 0);	/* slot: the mesh block */
	b_i32(&b, 0);	/* field: width */
	b_f32(&b, 2.25f);
	b_f32(&b, 0.0f);
	b_f32(&b, 0.0f);
	b_f32(&b, 0.0f);
	b_close(&b);
	exchange(&b);

	/* width is tight-packed first, rings third (float, vec3, int). */
	assert(g_params_len >= 20);
	memcpy(&width, g_params + 0, sizeof(width));
	memcpy(&rings, g_params + 16, sizeof(rings));
	assert(width == 2.25f);
	assert(rings == 4);	/* its declared default, not zero */
	printf("ok: a param write packs one field and preserves the rest\n");
}

/* The value comes back through the query, and the block reports as overridden. */
static void test_entity_param_write_is_visible(void)
{
	struct builder b;

	reset_all();
	fake_set_render_ref(0, MESH_ASSET_ID);

	b_reset(&b);
	b_open(&b, BRIDGE_OP_ENTITY_PARAM);
	b_i32(&b, 0);
	b_i32(&b, 0);
	b_i32(&b, 0);
	b_f32(&b, 2.25f);
	b_f32(&b, 0.0f);
	b_f32(&b, 0.0f);
	b_f32(&b, 0.0f);
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_ENTITY_PARAMS);
	b_i32(&b, 0);
	b_u32(&b, 0);
	b_close(&b);

	has(exchange(&b), "\"value\":[2.25]");
	has(g_bridge.reply, "\"overridden\":true");
	printf("ok: a written parameter reads back through the query\n");
}

/* A stale field index is a notice, not a failed batch. */
static void test_entity_param_write_out_of_range(void)
{
	struct builder b;

	reset_all();
	fake_set_render_ref(0, MESH_ASSET_ID);

	b_reset(&b);
	b_open(&b, BRIDGE_OP_ENTITY_PARAM);
	b_i32(&b, 0);
	b_i32(&b, 0);
	b_i32(&b, 99);	/* no such field */
	b_f32(&b, 1.0f);
	b_f32(&b, 0.0f);
	b_f32(&b, 0.0f);
	b_f32(&b, 0.0f);
	b_close(&b);

	has(exchange(&b), "command.rejected");
	has(g_bridge.reply, "\"error\":null");
	printf("ok: a stale parameter index is reported, not fatal\n");
}

static void test_no_services_is_survivable(void)
{
	struct bridge  bare;
	struct builder b;
	const char    *r;

	/* An engine built without the edit subsystem, or a boot mid-flight. */
	bridge_init(&bare, NULL);
	b_reset(&b);
	b_open(&b, BRIDGE_OP_SELECT);
	b_i32(&b, 1);
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_TREE);
	b_u32(&b, 0);
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_HISTORY);
	b_u32(&b, 0);
	b_close(&b);
	r = bridge_exchange(&bare, b.bytes, b.len);

	has(r, "\"error\":null");
	has(r, "\"value\":null");
	printf("ok: a bridge with no services answers rather than crashing\n");
}

/* ------------------------------------------------------------------ *
 * The viewport domain (#949)
 * ------------------------------------------------------------------ */

/* The camera gestures reach the camera with the numbers they were sent. */
static void test_camera_commands(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_CAMERA_ORBIT);
	b_f32(&b, 0.25f);
	b_f32(&b, -0.5f);
	b_close(&b);
	b_open(&b, BRIDGE_OP_CAMERA_PAN);
	b_f32(&b, 0.1f);
	b_f32(&b, 0.2f);
	b_close(&b);
	b_open(&b, BRIDGE_OP_CAMERA_DOLLY);
	b_f32(&b, -0.3f);
	b_close(&b);
	b_simple(&b, BRIDGE_OP_CAMERA_RESET);
	r = exchange(&b);

	has(r, "\"error\":null");
	has(r, "\"applied\":4");
	assert(g_orbit_yaw == 0.25f && g_orbit_pitch == -0.5f);
	assert(g_pan_dx == 0.1f && g_pan_dy == 0.2f);
	assert(g_dolly == -0.3f);
	assert(g_resets == 1);
	printf("ok: the camera gestures reach the camera unchanged\n");
}

/*
 * The size the editor reports is the size a pick is answered against, and the
 * viewport query reads it back — which is how the editor notices a dock that
 * collapsed under it.
 */
static void test_viewport_size_round_trips(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_VIEWPORT_SIZE);
	b_f32(&b, 1280.0f);
	b_f32(&b, 720.0f);
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_VIEWPORT);
	b_u32(&b, 0);
	b_close(&b);
	r = exchange(&b);

	has(r, "\"kind\":\"viewport\"");
	has(r, "\"width\":1280");
	has(r, "\"height\":720");
	printf("ok: the reported viewport size reads back\n");
}

/*
 * A pick is a select. The whole point of making it a command rather than a
 * query: the editor never holds an id the engine has not agreed to.
 */
static void test_pick_selects(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	g_pick_answer = 1;
	b_reset(&b);
	b_open(&b, BRIDGE_OP_PICK);
	b_f32(&b, 400.0f);
	b_f32(&b, 300.0f);
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_SELECTION);
	b_u32(&b, 0);
	b_close(&b);
	r = exchange(&b);

	assert(g_pick_x == 400.0f && g_pick_y == 300.0f);
	has(r, "\"kind\":\"selection\"");
	has(r, "\"id\":1");
	printf("ok: a pick selects what it hit\n");
}

/* A miss clears the selection, exactly as clicking empty space should. */
static void test_pick_miss_clears(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	g_pick_answer = 1;
	b_reset(&b);
	b_open(&b, BRIDGE_OP_PICK);
	b_f32(&b, 10.0f);
	b_f32(&b, 10.0f);
	b_close(&b);
	exchange(&b);
	assert(g_world.selected == 1);

	g_pick_answer = -1;
	b_reset(&b);
	b_open(&b, BRIDGE_OP_PICK);
	b_f32(&b, 10.0f);
	b_f32(&b, 10.0f);
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_SELECTION);
	b_u32(&b, 0);
	b_close(&b);
	r = exchange(&b);

	has(r, "\"id\":-1");
	printf("ok: a pick that misses clears the selection\n");
}

/* Frame with -1 means "the selection", so no caller has to know the id. */
static void test_frame_uses_the_selection(void)
{
	struct builder b;

	reset_all();
	g_bounds_entity = 1;
	b_reset(&b);
	b_open(&b, BRIDGE_OP_SELECT);
	b_i32(&b, 1);
	b_close(&b);
	b_open(&b, BRIDGE_OP_CAMERA_FRAME);
	b_i32(&b, -1);
	b_close(&b);
	exchange(&b);

	assert(g_frames == 1);
	assert(g_framed[0] == 1.0f && g_framed[1] == 2.0f &&
	       g_framed[2] == 3.0f);
	assert(g_framed_radius == 4.0f);
	printf("ok: frame-selection frames the selection's bounds\n");
}

/*
 * An entity with nothing drawn cannot be framed, and the reader who pressed the
 * key is told rather than left watching a still camera.
 */
static void test_frame_without_bounds_is_an_event(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	g_bounds_entity = -2;	/* nothing matches */
	b_reset(&b);
	b_open(&b, BRIDGE_OP_CAMERA_FRAME);
	b_i32(&b, 1);
	b_close(&b);
	r = exchange(&b);

	assert(g_frames == 0);
	has(r, "\"error\":null");
	has(r, "viewport.nothing");
	printf("ok: framing something undrawable says so\n");
}

/* Mode, snap and grid are engine state, and the query is how a panel reads it. */
static void test_gizmo_state_round_trips(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_GIZMO_MODE);
	b_i32(&b, GIZMO_MODE_ROTATE);
	b_close(&b);
	b_open(&b, BRIDGE_OP_GIZMO_SNAP);
	b_f32(&b, 0.5f);
	b_f32(&b, 15.0f);
	b_f32(&b, 0.25f);
	b_close(&b);
	b_open(&b, BRIDGE_OP_GRID);
	b_i32(&b, 0);
	b_f32(&b, 2.0f);
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_VIEWPORT);
	b_u32(&b, 0);
	b_close(&b);
	r = exchange(&b);

	has(r, "\"mode\":2");
	has(r, "\"translate\":0.5");
	has(r, "\"rotate\":15");
	has(r, "\"scale\":0.25");
	has(r, "\"shown\":false");
	has(r, "\"spacing\":2");
	printf("ok: the gizmo's mode, snap and grid read back\n");
}

/*
 * The drag serial. A press that grabs nothing changes nothing else, so without
 * it the editor could not tell "no answer yet" from "answered: nothing" — and
 * it would either orbit on a grabbed handle or refuse to orbit at all.
 */
static void test_drag_serial_advances_on_every_begin(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	g_gizmo_axis = GIZMO_AXIS_NONE;	/* the press will miss */
	b_reset(&b);
	b_open(&b, BRIDGE_OP_GIZMO_DRAG);
	b_i32(&b, BRIDGE_DRAG_BEGIN);
	b_f32(&b, 10.0f);
	b_f32(&b, 10.0f);
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_VIEWPORT);
	b_u32(&b, 0);
	b_close(&b);
	r = exchange(&b);

	assert(g_gizmo_begins == 1);
	has(r, "\"dragSerial\":1");
	has(r, "\"axis\":-1");
	printf("ok: a press that grabs nothing still reports an answer\n");
}

/*
 * A drag's moves land on set_transform — the same call an inspector edit makes.
 * That is the one line that makes a gizmo drag an ordinary undoable edit
 * (#944, Q2), so it is the one worth pinning.
 */
static void test_drag_moves_the_selection(void)
{
	struct builder b;

	reset_all();
	g_gizmo_axis = GIZMO_AXIS_X;
	b_reset(&b);
	b_open(&b, BRIDGE_OP_SELECT);
	b_i32(&b, 1);
	b_close(&b);
	b_open(&b, BRIDGE_OP_GIZMO_DRAG);
	b_i32(&b, BRIDGE_DRAG_BEGIN);
	b_f32(&b, 10.0f);
	b_f32(&b, 10.0f);
	b_close(&b);
	b_open(&b, BRIDGE_OP_GIZMO_DRAG);
	b_i32(&b, BRIDGE_DRAG_MOVE);
	b_f32(&b, 40.0f);
	b_f32(&b, 10.0f);
	b_close(&b);
	b_open(&b, BRIDGE_OP_GIZMO_DRAG);
	b_i32(&b, BRIDGE_DRAG_END);
	b_f32(&b, 40.0f);
	b_f32(&b, 10.0f);
	b_close(&b);
	exchange(&b);

	assert(g_world.local[1].position[0] == 7.0f);
	assert(g_gizmo_ends == 1);
	printf("ok: a gizmo drag moves the selection through set_transform\n");
}

/* A move that solves to nothing leaves the entity where it was. */
static void test_drag_that_solves_to_nothing_writes_nothing(void)
{
	struct builder b;

	reset_all();
	g_gizmo_axis     = GIZMO_AXIS_X;
	g_gizmo_moves_to = 0;
	b_reset(&b);
	b_open(&b, BRIDGE_OP_SELECT);
	b_i32(&b, 1);
	b_close(&b);
	b_open(&b, BRIDGE_OP_GIZMO_DRAG);
	b_i32(&b, BRIDGE_DRAG_MOVE);
	b_f32(&b, 40.0f);
	b_f32(&b, 10.0f);
	b_close(&b);
	exchange(&b);

	assert(g_world.local[1].position[0] == 0.0f);
	printf("ok: an unsolvable drag leaves the entity alone\n");
}

/* An unknown phase is a notice, not a rejected batch. */
static void test_unknown_drag_phase_is_an_event(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_GIZMO_DRAG);
	b_i32(&b, 99);
	b_f32(&b, 0.0f);
	b_f32(&b, 0.0f);
	b_close(&b);
	r = exchange(&b);

	has(r, "\"error\":null");
	has(r, "command.rejected");
	printf("ok: a drag phase this build does not know is a notice\n");
}

/* Editor mode is a switch, and leaving it abandons any drag in progress. */
static void test_editor_mode_switch(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_EDITOR_MODE);
	b_i32(&b, 1);
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_VIEWPORT);
	b_u32(&b, 0);
	b_close(&b);
	r = exchange(&b);
	assert(g_editor_mode == 1);
	has(r, "\"editorMode\":true");

	g_gizmo_axis = GIZMO_AXIS_Y;
	b_reset(&b);
	b_open(&b, BRIDGE_OP_EDITOR_MODE);
	b_i32(&b, 0);
	b_close(&b);
	exchange(&b);
	assert(g_editor_mode == 0);
	assert(g_gizmo_ends == 1);
	printf("ok: leaving editor mode abandons a drag in progress\n");
}

/*
 * The viewport is its own domain. A scene edit must not invalidate the mode
 * buttons, and pressing a mode button must not re-send the scene tree.
 */
static void test_viewport_is_its_own_domain(void)
{
	struct builder b;
	const char    *r;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_QUERY_VIEWPORT);
	b_u32(&b, 0);
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_TREE);
	b_u32(&b, 0);
	b_close(&b);
	r = exchange(&b);
	has(r, "\"fresh\":false");

	/* Move the scene only. The viewport's stamp must hold. */
	b_reset(&b);
	b_open(&b, BRIDGE_OP_ENTITY_NAME);
	b_i32(&b, 0);
	b_str(&b, "renamed");
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_VIEWPORT);
	b_u32(&b, bridge_generation(&g_bridge, BRIDGE_VIEWPORT));
	b_close(&b);
	r = exchange(&b);
	has(r, "\"kind\":\"viewport\"");
	has(r, "\"fresh\":true");

	/* Move the viewport only. The scene's stamp must hold. */
	b_reset(&b);
	b_open(&b, BRIDGE_OP_GIZMO_MODE);
	b_i32(&b, GIZMO_MODE_SCALE);
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_TREE);
	b_u32(&b, bridge_generation(&g_bridge, BRIDGE_SCENE));
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_VIEWPORT);
	b_u32(&b, bridge_generation(&g_bridge, BRIDGE_VIEWPORT));
	b_close(&b);
	r = exchange(&b);
	has(r, "\"kind\":\"scene.tree\",\"generation\":");
	has(r, "\"mode\":3");
	printf("ok: the viewport is cached apart from the scene\n");
}

/*
 * The camera does *not* move the viewport generation. An orbit runs every frame
 * of a drag, and a domain that moved with it would re-send the mode buttons
 * sixty times a second to say the same thing.
 */
static void test_camera_does_not_move_the_generation(void)
{
	struct builder b;
	uint32_t       before;

	reset_all();
	b_reset(&b);
	b_open(&b, BRIDGE_OP_QUERY_VIEWPORT);
	b_u32(&b, 0);
	b_close(&b);
	exchange(&b);
	before = bridge_generation(&g_bridge, BRIDGE_VIEWPORT);

	b_reset(&b);
	b_open(&b, BRIDGE_OP_CAMERA_ORBIT);
	b_f32(&b, 0.4f);
	b_f32(&b, 0.1f);
	b_close(&b);
	exchange(&b);

	assert(bridge_generation(&g_bridge, BRIDGE_VIEWPORT) == before);
	printf("ok: orbiting does not invalidate the viewport's cache\n");
}

/*
 * A build with no camera, no canvas and no gizmo still answers. The editor's
 * viewport panel has to render something on a page whose engine came up half
 * way, and "null" is not something a row of mode buttons can render.
 */
static void test_viewport_without_services(void)
{
	struct bridge  bare;
	struct builder b;
	const char    *r;

	bridge_init(&bare, NULL);
	b_reset(&b);
	b_open(&b, BRIDGE_OP_CAMERA_ORBIT);
	b_f32(&b, 1.0f);
	b_f32(&b, 1.0f);
	b_close(&b);
	b_open(&b, BRIDGE_OP_PICK);
	b_f32(&b, 1.0f);
	b_f32(&b, 1.0f);
	b_close(&b);
	b_open(&b, BRIDGE_OP_GIZMO_DRAG);
	b_i32(&b, BRIDGE_DRAG_BEGIN);
	b_f32(&b, 1.0f);
	b_f32(&b, 1.0f);
	b_close(&b);
	b_open(&b, BRIDGE_OP_QUERY_VIEWPORT);
	b_u32(&b, 0);
	b_close(&b);
	r = bridge_exchange(&bare, b.bytes, b.len);

	has(r, "\"error\":null");
	has(r, "\"kind\":\"viewport\"");
	has(r, "\"axis\":-1");
	has(r, "\"editorMode\":false");
	printf("ok: a viewport with no services answers rather than crashing\n");
}

int main(void)
{
	/*
	 * The real s7 image: the (params ...) clause under test is parsed by
	 * script-params-form, which is the engine's own reader. See build.scm.
	 */
	log_init();
	script_init();

	test_empty_tape();
	test_command_then_query_sees_the_command();
	test_query_order_does_not_matter();
	test_generation_etag();
	test_change_behind_the_bridges_back();
	test_idle_frames_do_not_move_generations();
	test_domains_are_independent();
	test_selection_and_history_queries();
	test_gesture_brackets();
	test_gesture_commit_without_begin_is_ignored();
	test_undo_is_an_ordinary_command();
	test_events_are_per_exchange();
	test_event_overflow_is_counted();
	test_create_and_destroy();
	test_rename_and_paused();
	test_bad_magic();
	test_truncated_record();
	test_unknown_opcode();
	test_payload_length_mismatch();
	test_entity_params_derive_from_the_asset();
	test_entity_params_without_assets();
	test_entity_params_for_a_dead_entity();
	test_entity_param_write();
	test_entity_param_write_is_visible();
	test_entity_param_write_out_of_range();
	test_scene_text_query();
	test_scene_text_is_generation_cached();
	test_scene_text_without_a_writer();
	test_scene_load_replaces_the_world();
	test_scene_load_clears_the_history();
	test_scene_load_rejection_is_an_event();
	test_load_then_save_in_one_batch();
	test_no_services_is_survivable();

	/* The viewport domain (#949). */
	test_camera_commands();
	test_viewport_size_round_trips();
	test_pick_selects();
	test_pick_miss_clears();
	test_frame_uses_the_selection();
	test_frame_without_bounds_is_an_event();
	test_gizmo_state_round_trips();
	test_drag_serial_advances_on_every_begin();
	test_drag_moves_the_selection();
	test_drag_that_solves_to_nothing_writes_nothing();
	test_unknown_drag_phase_is_an_event();
	test_editor_mode_switch();
	test_viewport_is_its_own_domain();
	test_camera_does_not_move_the_generation();
	test_viewport_without_services();

	printf("all bridge tests passed\n");
	return 0;
}
