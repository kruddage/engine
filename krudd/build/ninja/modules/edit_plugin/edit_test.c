/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "edit.h"
#include "memory.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Fake commands drive a single shared integer so undo/redo correctness is
 * observable, and every memento is heap-allocated with a live counter so leaks
 * (or double frees) surface here and under ASan.
 */
static int32_t g_value;
static int32_t g_live_mementos;

struct set_memento {
	int32_t *target;
	int32_t  before;
	int32_t  after;
};

static void set_apply(void *m)
{
	struct set_memento *sm = m;

	*sm->target = sm->after;
}

static void set_revert(void *m)
{
	struct set_memento *sm = m;

	*sm->target = sm->before;
}

static void set_free(void *m)
{
	mem_free(m);
	g_live_mementos--;
}

/* Build a "set g_value to after" command; before is the current value. */
static struct edit_cmd set_cmd(int32_t after, uint32_t key, const char *label)
{
	struct set_memento *sm = mem_alloc(sizeof(*sm));
	struct edit_cmd     cmd;

	sm->target = &g_value;
	sm->before = g_value;
	sm->after  = after;
	g_live_mementos++;

	cmd.apply        = set_apply;
	cmd.revert       = set_revert;
	cmd.free         = set_free;
	cmd.memento      = sm;
	cmd.coalesce_key = key;
	cmd.label        = label;
	return cmd;
}

static void reset_world(struct edit_history *h)
{
	g_value = 0;
	edit_history_reset(h);
}

/* AC: LIFO undo/redo; a fresh push after undo clears the redo stack. */
static void test_lifo_and_redo_clear(void)
{
	struct edit_history h;
	struct edit_cmd     c;

	reset_world(&h);

	c = set_cmd(10, 0, "a"); edit_history_push(&h, &c);
	c = set_cmd(20, 0, "b"); edit_history_push(&h, &c);
	c = set_cmd(30, 0, "c"); edit_history_push(&h, &c);
	assert(g_value == 30);

	assert(edit_history_undo(&h) == 1);
	assert(g_value == 20);
	assert(edit_history_undo(&h) == 1);
	assert(g_value == 10);
	assert(edit_history_redo(&h) == 1);
	assert(g_value == 20);

	/* Redo stack still has "c"; a fresh push must discard it. */
	assert(edit_history_can_redo(&h));
	c = set_cmd(99, 0, "d"); edit_history_push(&h, &c);
	assert(g_value == 99);
	assert(!edit_history_can_redo(&h));
	assert(edit_history_redo(&h) == 0);

	edit_history_clear(&h);
	assert(g_live_mementos == 0);
	printf("ok: lifo + redo-clear\n");
}

/* AC: begin..commit undoes/redoes as one entry; abort reverts, leaves history. */
static void test_transaction_and_abort(void)
{
	struct edit_history h;
	struct edit_cmd     c;

	reset_world(&h);

	edit_history_begin(&h, "move");
	c = set_cmd(5, 0, "x"); edit_history_push(&h, &c);
	c = set_cmd(7, 0, "y"); edit_history_push(&h, &c);
	edit_history_commit(&h);
	assert(g_value == 7);

	/* One entry: a single undo reverts BOTH pushes. */
	assert(edit_history_undo(&h) == 1);
	assert(g_value == 0);
	assert(!edit_history_can_undo(&h));
	assert(edit_history_redo(&h) == 1);
	assert(g_value == 7);

	/* Abort rolls the open gesture back and leaves history untouched. */
	edit_history_begin(&h, "scratch");
	c = set_cmd(11, 0, "p"); edit_history_push(&h, &c);
	c = set_cmd(13, 0, "q"); edit_history_push(&h, &c);
	edit_history_abort(&h);
	assert(g_value == 7);			/* rolled back to pre-gesture */
	assert(edit_history_undo_label(&h));	/* still the "move" entry */
	assert(edit_history_undo(&h) == 1);
	assert(g_value == 0);

	edit_history_clear(&h);
	assert(g_live_mementos == 0);
	printf("ok: transaction + abort\n");
}

/* AC: nested begin flattens — inner begin/commit fold into the outer entry. */
static void test_nested_transaction(void)
{
	struct edit_history h;
	struct edit_cmd     c;

	reset_world(&h);

	edit_history_begin(&h, "outer");
	c = set_cmd(1, 0, "a"); edit_history_push(&h, &c);
	edit_history_begin(&h, "inner");
	c = set_cmd(2, 0, "b"); edit_history_push(&h, &c);
	edit_history_commit(&h);		/* inner: does not close */
	c = set_cmd(3, 0, "c"); edit_history_push(&h, &c);
	edit_history_commit(&h);		/* outer: closes the entry */
	assert(g_value == 3);

	assert(edit_history_undo(&h) == 1);
	assert(g_value == 0);			/* all three revert together */
	assert(!edit_history_can_undo(&h));

	edit_history_clear(&h);
	assert(g_live_mementos == 0);
	printf("ok: nested transaction flattens\n");
}

/* AC: same-key consecutive pushes coalesce; different keys do not. */
static void test_coalescing(void)
{
	struct edit_history h;
	struct edit_cmd     c;

	reset_world(&h);

	/* A slider drag: many same-key pushes fold into one entry. */
	c = set_cmd(10, 7, "drag"); edit_history_push(&h, &c);
	c = set_cmd(20, 7, "drag"); edit_history_push(&h, &c);
	c = set_cmd(30, 7, "drag"); edit_history_push(&h, &c);
	assert(g_value == 30);

	/* One undo returns to the pre-drag value, not an intermediate one. */
	assert(edit_history_undo(&h) == 1);
	assert(g_value == 0);
	assert(!edit_history_can_undo(&h));
	assert(edit_history_redo(&h) == 1);
	assert(g_value == 30);			/* redo lands on the newest */
	edit_history_clear(&h);

	/* Different keys stay distinct entries. */
	reset_world(&h);
	c = set_cmd(1, 1, "f1"); edit_history_push(&h, &c);
	c = set_cmd(2, 2, "f2"); edit_history_push(&h, &c);
	assert(edit_history_undo(&h) == 1);
	assert(g_value == 1);
	assert(edit_history_undo(&h) == 1);
	assert(g_value == 0);
	edit_history_clear(&h);

	/* An undo between same-key pushes breaks the coalesce chain. */
	reset_world(&h);
	c = set_cmd(4, 5, "g"); edit_history_push(&h, &c);
	edit_history_undo(&h);
	edit_history_redo(&h);
	c = set_cmd(8, 5, "g"); edit_history_push(&h, &c);
	assert(edit_history_can_undo(&h));
	assert(edit_history_undo(&h) == 1);
	assert(g_value == 4);			/* second push is its own entry */

	edit_history_clear(&h);
	assert(g_live_mementos == 0);
	printf("ok: coalescing\n");
}

/* AC: history is depth-bounded; overflow frees the dropped entry's memento. */
static void test_depth_bound(void)
{
	struct edit_history h;
	struct edit_cmd     c;
	int32_t             i;
	int32_t             undos;

	reset_world(&h);

	/* Push more than the cap of distinct entries. */
	for (i = 0; i < EDIT_MAX_ENTRIES + 5; i++) {
		c = set_cmd(i + 1, 0, "n");
		edit_history_push(&h, &c);
	}
	/* Only the cap's worth survive; the 5 oldest were freed, not leaked. */
	assert(g_live_mementos == EDIT_MAX_ENTRIES);

	undos = 0;
	while (edit_history_undo(&h))
		undos++;
	assert(undos == EDIT_MAX_ENTRIES);

	edit_history_clear(&h);
	assert(g_live_mementos == 0);
	printf("ok: depth-bounded (%d entries)\n", EDIT_MAX_ENTRIES);
}

/* AC: set_recording(0) applies commands without recording them. */
static void test_recording_gate(void)
{
	struct edit_history h;
	struct edit_cmd     c;

	reset_world(&h);

	edit_history_set_recording(&h, 0);
	c = set_cmd(42, 0, "play"); edit_history_push(&h, &c);
	assert(g_value == 42);			/* applied... */
	assert(!edit_history_can_undo(&h));	/* ...but not recorded */
	assert(g_live_mementos == 0);		/* memento freed immediately */

	edit_history_set_recording(&h, 1);
	c = set_cmd(43, 0, "edit"); edit_history_push(&h, &c);
	assert(edit_history_can_undo(&h));

	edit_history_clear(&h);
	assert(g_live_mementos == 0);
	printf("ok: recording gate\n");
}

/* AC: undo/redo labels report the next entry's name. */
static void test_labels(void)
{
	struct edit_history h;
	struct edit_cmd     c;

	reset_world(&h);
	assert(edit_history_undo_label(&h) == NULL);
	assert(edit_history_redo_label(&h) == NULL);

	c = set_cmd(1, 0, "first"); edit_history_push(&h, &c);
	assert(edit_history_undo_label(&h) != NULL);
	assert(edit_history_redo_label(&h) == NULL);

	edit_history_undo(&h);
	assert(edit_history_undo_label(&h) == NULL);
	assert(edit_history_redo_label(&h) != NULL);

	edit_history_clear(&h);
	assert(g_live_mementos == 0);
	printf("ok: labels\n");
}

int main(void)
{
	test_lifo_and_redo_clear();
	test_transaction_and_abort();
	test_nested_transaction();
	test_coalescing();
	test_depth_bound();
	test_recording_gate();
	test_labels();
	printf("all edit tests passed\n");
	return 0;
}
