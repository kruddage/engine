/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MESH_H
#define MESH_H

#include <stddef.h>
#include <stdint.h>

/*
 * Canonical vertex for built-in mesh geometry: interleaved position, normal,
 * and a single UV channel, 32-byte stride. Matches the "position, normal, uv0"
 * attribute set the primitive assets advertise via describe(), and the vertex
 * layout the scene renderer's pipeline declares.
 */
struct mesh_vertex {
	float position[3];
	float normal[3];
	float uv0[2];
};

/*
 * On-the-wire mesh blob. A mesh is two arrays (vertices + indices), but the
 * asset catalog delivers a single opaque byte buffer through get_data(), so we
 * pack both behind this header: the header, then vertex_count mesh_vertex
 * records, then index_count uint16_t indices. Native and WASM share endianness
 * (little), so the bytes travel verbatim.
 */
#define MESH_BLOB_MAGIC 0x4853454du /* sentinel, not a version */

struct mesh_blob {
	uint32_t magic;        /* MESH_BLOB_MAGIC */
	uint32_t vertex_count;
	uint32_t index_count;
	uint32_t index_format; /* gpu_index_format; 0 = uint16 (the only kind) */
};

/* Borrow the vertex array packed immediately after the header. */
static inline const struct mesh_vertex *
mesh_blob_vertices(const struct mesh_blob *b)
{
	return (const struct mesh_vertex *)(b + 1);
}

/* Borrow the index array packed immediately after the vertices. */
static inline const uint16_t *mesh_blob_indices(const struct mesh_blob *b)
{
	return (const uint16_t *)(mesh_blob_vertices(b) + b->vertex_count);
}

/* Total byte size of a blob holding the given vertex and index counts. */
static inline uint32_t mesh_blob_size(uint32_t vertex_count,
				      uint32_t index_count)
{
	return (uint32_t)(sizeof(struct mesh_blob)
			  + (size_t)vertex_count * sizeof(struct mesh_vertex)
			  + (size_t)index_count * sizeof(uint16_t));
}

#endif /* MESH_H */
