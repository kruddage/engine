/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "edit.h"

#include <stddef.h>
#include <string.h>

/*
 * Decisions (see edit_api.h for the contract):
 *   - Memento ownership: per-cmd free() hook. The domain chooses its memento
 *     shape and how to release it; the service just calls the hook once per
 *     allocation it drops.
 *   - Nested transactions: flattened. Inner begin/commit adjust the depth but
 *     only the outermost commit closes the entry; every push in between lands
 *     in the one gesture entry.
 *   - Coalescing window: by key AND gesture boundary. A begin/commit or an
 *     undo/redo clears the coalesce anchor, so folding never bridges two
 *     gestures even when they reuse a key.
 */

static struct edit_entry *entry_at(struct edit_history *h, int32_t logical)
{
	return &h->entries[(h->head + logical) % EDIT_MAX_ENTRIES];
}

/* Release both memento sides of a step, freeing a shared allocation once. */
static void step_free(struct edit_step *s)
{
	if (s->redo_free && s->redo_memento)
		s->redo_free(s->redo_memento);
	if (s->undo_memento != s->redo_memento &&
	    s->undo_free && s->undo_memento)
		s->undo_free(s->undo_memento);
}

static void entry_free(struct edit_entry *e)
{
	int32_t i;

	for (i = 0; i < e->step_count; i++)
		step_free(&e->steps[i]);
	e->step_count = 0;
	e->label = NULL;
}

/* A fresh step records the same memento for both undo and redo. */
static void step_init(struct edit_step *s, const struct edit_cmd *cmd)
{
	s->redo         = cmd->apply;
	s->undo         = cmd->revert;
	s->redo_free    = cmd->free;
	s->undo_free    = cmd->free;
	s->redo_memento = cmd->memento;
	s->undo_memento = cmd->memento;
	s->coalesce_key = cmd->coalesce_key;
}

/*
 * Fold a newer cmd into an existing step: keep the original undo side, adopt
 * the newer redo side, and free the intermediate redo memento it replaces
 * (unless it is still shared with the undo side, i.e. the first fold).
 */
static void step_coalesce(struct edit_step *s, const struct edit_cmd *cmd)
{
	if (s->redo_memento != s->undo_memento &&
	    s->redo_free && s->redo_memento)
		s->redo_free(s->redo_memento);
	s->redo         = cmd->apply;
	s->redo_free    = cmd->free;
	s->redo_memento = cmd->memento;
}

/* Append cmd to entry e as a new step, or coalesce into the last one. */
static void entry_add_step(struct edit_entry *e, const struct edit_cmd *cmd)
{
	struct edit_step *last;

	if (e->step_count > 0) {
		last = &e->steps[e->step_count - 1];
		if (cmd->coalesce_key != 0 &&
		    last->coalesce_key == cmd->coalesce_key) {
			step_coalesce(last, cmd);
			return;
		}
		/* Out of step slots: fold rather than drop the edit. */
		if (e->step_count >= EDIT_MAX_STEPS) {
			step_coalesce(last, cmd);
			return;
		}
	}
	step_init(&e->steps[e->step_count], cmd);
	e->step_count++;
}

/* Drop every redoable entry above pos so a fresh push can take their place. */
static void clear_redo(struct edit_history *h)
{
	int32_t i;

	for (i = h->pos; i < h->size; i++)
		entry_free(entry_at(h, i));
	h->size = h->pos;
}

static void drop_oldest(struct edit_history *h)
{
	entry_free(entry_at(h, 0));
	h->head = (h->head + 1) % EDIT_MAX_ENTRIES;
	h->size--;
	if (h->pos > 0)
		h->pos--;
	h->anchor = (h->anchor > 0) ? h->anchor - 1 : -1;
}

/*
 * Append a new empty entry at the top of the undo stack and return it. Assumes
 * redo has already been cleared so size == pos on entry.
 */
static struct edit_entry *new_entry(struct edit_history *h)
{
	struct edit_entry *e;

	if (h->size >= EDIT_MAX_ENTRIES)
		drop_oldest(h);
	e = entry_at(h, h->size);
	e->step_count = 0;
	e->label = NULL;
	h->size++;
	h->pos = h->size;
	return e;
}

void edit_history_reset(struct edit_history *h)
{
	memset(h, 0, sizeof(*h));
	h->recording = 1;
	h->anchor = -1;
}

void edit_history_set_recording(struct edit_history *h, int32_t on)
{
	h->recording = on ? 1 : 0;
}

void edit_history_push(struct edit_history *h, const struct edit_cmd *cmd)
{
	struct edit_entry *e;
	struct edit_step  *last;

	/* A push always takes effect immediately — even when not recording. */
	if (cmd->apply)
		cmd->apply(cmd->memento);

	if (!h->recording) {
		if (cmd->free && cmd->memento)
			cmd->free(cmd->memento);
		return;
	}

	if (h->txn_depth > 0) {
		if (!h->txn_open) {
			clear_redo(h);
			e = new_entry(h);
			e->label = h->txn_label;
			h->txn_open = 1;
		}
		entry_add_step(entry_at(h, h->pos - 1), cmd);
		h->anchor = -1;	/* a gesture never coalesces across entries */
		return;
	}

	/* Non-gesture push: fold into the previous same-key entry if open. */
	if (cmd->coalesce_key != 0 && h->anchor >= 0 &&
	    h->anchor == h->pos - 1) {
		e = entry_at(h, h->anchor);
		if (e->step_count > 0) {
			last = &e->steps[e->step_count - 1];
			if (last->coalesce_key == cmd->coalesce_key) {
				step_coalesce(last, cmd);
				return;
			}
		}
	}

	clear_redo(h);
	e = new_entry(h);
	e->label = cmd->label;
	entry_add_step(e, cmd);
	h->anchor = (cmd->coalesce_key != 0) ? h->pos - 1 : -1;
}

void edit_history_begin(struct edit_history *h, const char *label)
{
	h->txn_depth++;
	if (h->txn_depth == 1) {
		h->txn_open = 0;
		h->txn_label = label;
	}
	h->anchor = -1;
}

void edit_history_commit(struct edit_history *h)
{
	if (h->txn_depth == 0)
		return;
	h->txn_depth--;
	if (h->txn_depth == 0) {
		h->txn_open = 0;
		h->txn_label = NULL;
		h->anchor = -1;
	}
}

void edit_history_abort(struct edit_history *h)
{
	struct edit_entry *e;
	int32_t            i;

	if (h->txn_depth == 0)
		return;

	/* Roll the open gesture back and discard its entry, if any push landed. */
	if (h->txn_open) {
		e = entry_at(h, h->pos - 1);
		for (i = e->step_count - 1; i >= 0; i--)
			e->steps[i].undo(e->steps[i].undo_memento);
		entry_free(e);
		h->pos--;
		h->size--;
	}
	h->txn_depth = 0;
	h->txn_open = 0;
	h->txn_label = NULL;
	h->anchor = -1;
}

int32_t edit_history_undo(struct edit_history *h)
{
	struct edit_entry *e;
	int32_t            i;

	h->anchor = -1;
	if (h->pos == 0)
		return 0;
	h->pos--;
	e = entry_at(h, h->pos);
	for (i = e->step_count - 1; i >= 0; i--)
		e->steps[i].undo(e->steps[i].undo_memento);
	return 1;
}

int32_t edit_history_redo(struct edit_history *h)
{
	struct edit_entry *e;
	int32_t            i;

	h->anchor = -1;
	if (h->pos == h->size)
		return 0;
	e = entry_at(h, h->pos);
	for (i = 0; i < e->step_count; i++)
		e->steps[i].redo(e->steps[i].redo_memento);
	h->pos++;
	return 1;
}

int32_t edit_history_can_undo(const struct edit_history *h)
{
	return h->pos > 0;
}

int32_t edit_history_can_redo(const struct edit_history *h)
{
	return h->pos < h->size;
}

const char *edit_history_undo_label(const struct edit_history *h)
{
	if (h->pos == 0)
		return NULL;
	return h->entries[(h->head + h->pos - 1) % EDIT_MAX_ENTRIES].label;
}

const char *edit_history_redo_label(const struct edit_history *h)
{
	if (h->pos == h->size)
		return NULL;
	return h->entries[(h->head + h->pos) % EDIT_MAX_ENTRIES].label;
}

void edit_history_clear(struct edit_history *h)
{
	int32_t i;

	for (i = 0; i < h->size; i++)
		entry_free(entry_at(h, i));
	h->head = 0;
	h->size = 0;
	h->pos = 0;
	h->txn_depth = 0;
	h->txn_open = 0;
	h->txn_label = NULL;
	h->anchor = -1;
}
