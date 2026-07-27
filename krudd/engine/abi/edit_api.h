/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef EDIT_API_H
#define EDIT_API_H

#include <stdint.h>

/*
 * The "edit" subsystem — a single global undo/redo timeline shared by every
 * editor domain (scene, asset, ...). Domains never keep their own history;
 * they describe a reversible change as an edit_cmd and hand it to push().
 *
 * A change is a memento pair: apply() drives the state to its "after" value
 * (do / redo) and revert() drives it back to "before" (undo). Both read the
 * same opaque memento the domain allocates. The service owns that memento's
 * lifetime once pushed and releases it through the optional free hook when
 * the entry is dropped (redo cleared, ring overflow, clear, or shutdown).
 *
 * Gestures. A gizmo drag or a multi-field property edit is one user action,
 * so it should be one undo entry. Bracket the pushes in begin()/commit() and
 * they collapse into a single entry; abort() rolls the open gesture back.
 *
 * Coalescing. A continuous edit (dragging a slider) fires many pushes a frame
 * apart. Give them a shared non-zero coalesce_key and consecutive pushes fold
 * into the current entry instead of spamming the timeline — the entry keeps
 * the original "before" for undo and the newest "after" for redo.
 */

struct edit_cmd {
	/* Drive state to its post-edit value. Called by push() and redo(). */
	void		(*apply)(void *memento);
	/* Drive state back to its pre-edit value. Called by undo(). */
	void		(*revert)(void *memento);
	/*
	 * Release memento. Optional (NULL if the memento needs no freeing).
	 * The service calls it exactly once per allocation it drops.
	 */
	void		(*free)(void *memento);
	/* Opaque before/after state; the domain owns its shape. */
	void		*memento;
	/* Non-zero: fold into the previous same-key push. 0: never coalesce. */
	uint32_t	coalesce_key;
	/* Human-readable name for this change (used as the entry label). */
	const char	*label;
};

struct edit_api {
	/* Apply cmd now and record it as history (clears the redo stack). */
	void		(*push)(const struct edit_cmd *cmd);

	/* Bracket several pushes into ONE history entry. */
	void		(*begin)(const char *label);
	void		(*commit)(void);
	void		(*abort)(void);		/* roll back an open gesture */

	int32_t		(*undo)(void);		/* 0 if nothing to undo */
	int32_t		(*redo)(void);		/* 0 if nothing to redo */
	int32_t		(*can_undo)(void);
	int32_t		(*can_redo)(void);
	const char	*(*undo_label)(void);	/* next undo's label, or NULL */
	const char	*(*redo_label)(void);	/* next redo's label, or NULL */

	void		(*clear)(void);		/* drop all history */
	void		(*set_recording)(int32_t on);	/* the play-mode gate */
};

#endif /* EDIT_API_H */
