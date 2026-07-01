/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef EDIT_H
#define EDIT_H

#include "edit_api.h"

#include <stdint.h>

/*
 * Pure history data structure behind the "edit" subsystem. The plugin owns one
 * static instance and wraps these ops in the edit_api vtable; native tests link
 * this file directly and drive a caller-owned instance with fake commands.
 *
 * The store is a fixed ring of entries so history is depth-bounded — pushing
 * past the cap drops the oldest entry and frees its mementos. An entry holds an
 * ordered list of steps; a plain push makes a one-step entry, a begin/commit
 * gesture makes a multi-step one. Undo reverts an entry's steps back-to-front;
 * redo applies them front-to-back.
 *
 * Coalescing keeps a continuous edit to one step: a step carries a separate
 * undo side (the original "before") and redo side (the latest "after"), so
 * folding a newer push in just swaps the redo side and frees the superseded
 * intermediate memento.
 */

#define EDIT_MAX_ENTRIES	128
#define EDIT_MAX_STEPS		32

struct edit_step {
	void		(*redo)(void *);	/* apply side (newest) */
	void		(*undo)(void *);	/* revert side (original) */
	void		(*redo_free)(void *);
	void		(*undo_free)(void *);
	void		*redo_memento;
	void		*undo_memento;
	uint32_t	coalesce_key;
};

struct edit_entry {
	struct edit_step	steps[EDIT_MAX_STEPS];
	int32_t			step_count;
	const char		*label;
};

struct edit_history {
	struct edit_entry	entries[EDIT_MAX_ENTRIES];
	int32_t			head;		/* ring index of logical entry 0 */
	int32_t			size;		/* live entries (undo + redo) */
	int32_t			pos;		/* undoable entries, 0..size */
	int32_t			txn_depth;	/* open begin() nesting */
	int32_t			txn_open;	/* a gesture entry is materialised */
	int32_t			recording;	/* the play-mode gate */
	int32_t			anchor;		/* logical entry open to coalesce */
	const char		*txn_label;
};

void edit_history_reset(struct edit_history *h);
void edit_history_push(struct edit_history *h, const struct edit_cmd *cmd);
void edit_history_begin(struct edit_history *h, const char *label);
void edit_history_commit(struct edit_history *h);
void edit_history_abort(struct edit_history *h);
int32_t edit_history_undo(struct edit_history *h);
int32_t edit_history_redo(struct edit_history *h);
int32_t edit_history_can_undo(const struct edit_history *h);
int32_t edit_history_can_redo(const struct edit_history *h);
const char *edit_history_undo_label(const struct edit_history *h);
const char *edit_history_redo_label(const struct edit_history *h);
void edit_history_clear(struct edit_history *h);
void edit_history_set_recording(struct edit_history *h, int32_t on);

#endif /* EDIT_H */
