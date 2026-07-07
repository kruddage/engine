/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "branch.h"

#include <string.h>

static const char MAIN_NAME[] = "main";   /* reserved bootstrap branch */

void branches_init(struct branches *b, struct cas *store)
{
	b->store  = store;
	b->count  = 0;
	b->active = BRANCH_NONE;
}

static int name_ok(const char *name)
{
	size_t len;

	if (!name)
		return 0;
	len = strlen(name);
	return len > 0 && len < BRANCH_NAME_MAX;
}

int32_t branches_find(const struct branches *b, const char *name)
{
	uint32_t i;

	if (!name)
		return BRANCH_NONE;
	for (i = 0; i < b->count; i++) {
		if (strcmp(b->list[i].name, name) == 0)
			return (int32_t)i;
	}
	return BRANCH_NONE;
}

static int32_t add_branch(struct branches *b, const char *name,
			  cas_hash_t manifest, cas_hash_t base)
{
	struct branch_info *bi;

	if (b->count >= BRANCH_MAX)
		return BRANCH_NONE;
	bi = &b->list[b->count];
	memcpy(bi->name, name, strlen(name) + 1);   /* length checked by caller */
	bi->manifest = manifest;
	bi->base     = base;
	return (int32_t)b->count++;
}

int32_t branches_commit(struct branches *b, cas_hash_t manifest)
{
	if (b->count == 0) {
		int32_t idx = add_branch(b, MAIN_NAME, manifest, CAS_HASH_NONE);

		if (idx == BRANCH_NONE)
			return BRANCH_NONE;
		b->active = idx;   /* bootstrap: main is born active */
		return idx;
	}

	if (b->active < 0 || (uint32_t)b->active >= b->count)
		return BRANCH_NONE;
	b->list[b->active].manifest = manifest;
	return b->active;
}

int32_t branches_create(struct branches *b, const char *name,
			cas_hash_t manifest, cas_hash_t base)
{
	if (!name_ok(name))
		return BRANCH_NONE;
	if (strcmp(name, MAIN_NAME) == 0)              /* reserved for bootstrap */
		return BRANCH_NONE;
	if (branches_find(b, name) != BRANCH_NONE)     /* names are unique */
		return BRANCH_NONE;
	return add_branch(b, name, manifest, base);
}

int32_t branches_set_active(struct branches *b, int32_t index)
{
	if (index < 0 || (uint32_t)index >= b->count)
		return -1;
	b->active = index;
	return 0;
}

int32_t branches_set_manifest(struct branches *b, int32_t index,
			      cas_hash_t manifest)
{
	if (index < 0 || (uint32_t)index >= b->count)
		return -1;
	b->list[index].manifest = manifest;
	return 0;
}

int32_t branches_active(const struct branches *b)
{
	return b->active;
}

uint32_t branches_count(const struct branches *b)
{
	return b->count;
}

const struct branch_info *branches_get(const struct branches *b, int32_t index)
{
	if (index < 0 || (uint32_t)index >= b->count)
		return NULL;
	return &b->list[index];
}

int32_t branches_working_set(struct branches *b, int32_t index,
			     struct cas_entry *out, uint32_t max)
{
	const struct branch_info *bi = branches_get(b, index);

	if (!bi)
		return -1;
	return cas_get_manifest(b->store, bi->manifest, out, max);
}
