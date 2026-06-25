/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ring_buf.h"

#include <string.h>

void ring_buf_init(struct ring_buf *rb, void *storage,
		   size_t elem_size, size_t capacity)
{
	rb->storage   = storage;
	rb->elem_size = elem_size;
	rb->capacity  = capacity;
	rb->head      = 0;
	rb->count     = 0;
}

void ring_buf_push(struct ring_buf *rb, const void *elem)
{
	char *dst;

	dst = (char *)rb->storage + rb->head * rb->elem_size;
	memcpy(dst, elem, rb->elem_size);
	rb->head = (rb->head + 1) % rb->capacity;
	if (rb->count < rb->capacity)
		rb->count++;
}

int ring_buf_peek(const struct ring_buf *rb, size_t index, void *out)
{
	size_t real_idx;

	if (index >= rb->count)
		return -1;

	if (rb->count < rb->capacity)
		real_idx = index;
	else
		real_idx = (rb->head + index) % rb->capacity;

	memcpy(out, (const char *)rb->storage + real_idx * rb->elem_size,
	       rb->elem_size);
	return 0;
}

size_t ring_buf_len(const struct ring_buf *rb)
{
	return rb->count;
}
