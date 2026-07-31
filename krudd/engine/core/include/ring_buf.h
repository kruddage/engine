/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RING_BUF_H
#define RING_BUF_H

#include <stddef.h>

/*
 * Generic fixed-capacity ring buffer.  Caller provides the backing
 * storage — no heap allocation inside.  When full, ring_buf_push
 * overwrites the oldest element.
 */
struct ring_buf {
	void   *storage;
	size_t  elem_size;
	size_t  capacity;
	size_t  head;   /* index of next write position */
	size_t  count;  /* number of live elements      */
};

void   ring_buf_init(struct ring_buf *rb, void *storage,
		     size_t elem_size, size_t capacity);
void   ring_buf_push(struct ring_buf *rb, const void *elem);
int    ring_buf_peek(const struct ring_buf *rb, size_t index, void *out);
size_t ring_buf_len(const struct ring_buf *rb);

#endif /* RING_BUF_H */
