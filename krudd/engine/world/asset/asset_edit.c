/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "asset_edit.h"

#include <stddef.h>
#include <string.h>

/*
 * A captured asset state. `present == 0` means the id held nothing (the state
 * before a create or after a destroy); otherwise bytes/path/type describe the
 * authored asset. `bytes` is a private heap copy (NULL when size == 0).
 */
struct asset_snapshot {
	int32_t   present;
	int32_t   type;			/* ASSET_TYPE_* when present */
	uint32_t  size;
	uint8_t  *bytes;		/* owned; NULL when size == 0 */
	char      path[ASSET_PATH_MAX];
};

static struct asset_snapshot *snap_alloc(const struct memory_api *mem)
{
	struct asset_snapshot *s;

	if (!mem)
		return NULL;
	s = mem->alloc(sizeof(*s));
	if (!s)
		return NULL;
	s->present = 0;
	s->type    = ASSET_TYPE_UNKNOWN;
	s->size    = 0;
	s->bytes   = NULL;
	s->path[0] = '\0';
	return s;
}

struct asset_snapshot *asset_snapshot_absent(const struct memory_api *mem)
{
	return snap_alloc(mem);
}

struct asset_snapshot *asset_snapshot_capture(uint32_t id,
					      const struct memory_api *mem)
{
	struct asset_snapshot *s = snap_alloc(mem);
	struct asset_info      info;
	const void            *bytes;
	uint32_t               size = 0;

	if (!s)
		return NULL;
	if (asset_catalog_find(id, &info) != 0)
		return s;		/* absent — nothing under this id */

	strncpy(s->path, info.path, ASSET_PATH_MAX - 1);
	s->path[ASSET_PATH_MAX - 1] = '\0';
	s->type = info.type;

	bytes = asset_catalog_get_data(id, &size);
	if (size > 0 && bytes) {
		s->bytes = mem->alloc((size_t)size);
		if (!s->bytes) {
			mem->free(s);
			return NULL;
		}
		memcpy(s->bytes, bytes, (size_t)size);
	}
	s->size    = size;
	s->present = 1;
	return s;
}

void asset_snapshot_free(struct asset_snapshot *s, const struct memory_api *mem)
{
	if (!s || !mem)
		return;
	if (s->bytes)
		mem->free(s->bytes);
	mem->free(s);
}

uint32_t asset_edit_key(uint32_t id)
{
	if (id == 0)
		return 0;
	return 0x80000000u | (id & 0x7fffffffu);
}

/*
 * Drive asset `id` to state `s`. Reversing an edit is just re-establishing the
 * target state regardless of where we are now: absent means destroy if it's
 * there; present means overwrite the bytes if it exists, or inject it back under
 * its original id if it was gone. inject() preserves identity so a recreated
 * asset keeps the same stable id it had before.
 */
static void drive_to(uint32_t id, const struct asset_snapshot *s)
{
	struct asset_info info;
	int32_t           exists = (asset_catalog_find(id, &info) == 0);

	if (!s->present) {
		if (exists)
			asset_mut_destroy(id);
		return;
	}
	if (exists)
		asset_mut_set_data(id, s->bytes, s->size);
	else
		asset_mut_inject(id, s->path, s->type, s->bytes, s->size);
}

/*
 * Command state: the allocator that owns the snapshots plus the id and the two
 * states bracketing the edit. apply/redo drives to `after`, revert/undo drives
 * to `before`. The edit service owns this memento and calls free when it drops
 * the entry.
 */
struct asset_edit_memento {
	const struct memory_api *mem;
	uint32_t                 id;
	struct asset_snapshot   *before;
	struct asset_snapshot   *after;
};

static void asset_edit_apply(void *memento)
{
	struct asset_edit_memento *m = memento;

	drive_to(m->id, m->after);
}

static void asset_edit_revert(void *memento)
{
	struct asset_edit_memento *m = memento;

	drive_to(m->id, m->before);
}

static void asset_edit_free(void *memento)
{
	struct asset_edit_memento *m = memento;

	if (!m)
		return;
	asset_snapshot_free(m->before, m->mem);
	asset_snapshot_free(m->after, m->mem);
	m->mem->free(m);
}

void asset_edit_record(const struct edit_api *edit, const struct memory_api *mem,
		       uint32_t id, struct asset_snapshot *before,
		       const char *label, uint32_t coalesce_key)
{
	struct asset_edit_memento *m;
	struct asset_snapshot     *after;
	struct edit_cmd            cmd;

	/* No history/allocator (or nothing captured): drop before and go. */
	if (!edit || !mem || !before) {
		asset_snapshot_free(before, mem);
		return;
	}

	after = asset_snapshot_capture(id, mem);
	m     = mem->alloc(sizeof(*m));
	if (!after || !m) {
		/* The mutation already happened; we just can't record it. */
		asset_snapshot_free(before, mem);
		asset_snapshot_free(after, mem);
		mem->free(m);
		return;
	}

	m->mem    = mem;
	m->id     = id;
	m->before = before;
	m->after  = after;

	cmd.apply        = asset_edit_apply;
	cmd.revert       = asset_edit_revert;
	cmd.free         = asset_edit_free;
	cmd.memento      = m;
	cmd.coalesce_key = coalesce_key;
	cmd.label        = label;

	/* push() re-applies `after` (already the live state) — a no-op — then
	 * records the entry. */
	edit->push(&cmd);
}
