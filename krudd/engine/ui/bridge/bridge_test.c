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

/*
 * Only the members the bridge touches are filled in. The rest stay NULL on
 * purpose: every call site in bridge.c guards, and a test that filled them
 * would stop proving that.
 */
static const struct entity_api g_entity = {
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
 * Fixtures
 * ------------------------------------------------------------------ */

static struct bridge g_bridge;

static void reset_all(void)
{
	static const struct bridge_host host = {
		.entity = &g_entity,
		.edit   = &g_edit,
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

	bridge_init(&g_bridge, &host);

	/* Two entities: a root and a child, so parentage is observable. */
	assert(fake_create(-1, &origin, 0, 0) == 0);
	assert(fake_create(0, &origin, 0, 0) == 1);
	fake_set_name(0, "root");
	fake_set_name(1, "child");
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

	has(r, "\"protocol\":1");
	has(r, "\"serial\":1");
	has(r, "\"applied\":0");
	has(r, "\"error\":null");
	has(r, "\"results\":[]");
	/* Seeded on the first refresh; 0 is reserved for "the client has none". */
	has(r, "\"generations\":{\"scene\":1,\"selection\":1,\"history\":1}");
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

int main(void)
{
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
	test_no_services_is_survivable();
	printf("all bridge tests passed\n");
	return 0;
}
