/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CAS_MEM_H
#define CAS_MEM_H

#include "cas.h"

struct memory_api;

/*
 * In-memory content backing for struct cas — an open-addressing hash table of
 * refcounted blobs, allocated through the injected memory_api.  This is the
 * native / unit-test implementation of struct cas_backing; the browser uses an
 * IndexedDB backing exposing the same interface.
 *
 * cas_mem_init wires s->backing (and s->mem) to a freshly allocated table.
 * Returns 0 on success, -1 on bad args or OOM.  cas_mem_shutdown frees every
 * stored blob and the table.
 */
int32_t cas_mem_init(struct cas *s, const struct memory_api *mem);
void    cas_mem_shutdown(struct cas *s);

#endif /* CAS_MEM_H */
