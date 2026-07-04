/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "primitives.h"
#include "mesh.h"

#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * Procedural geometry for the engine's built-in mesh primitives. Each
 * generator fills a mesh_blob in place (never a large stack array — the sphere
 * alone is ~24 KB) and hands back an owned buffer. Vertex/index counts mirror
 * the describe() metadata in asset_plugin.c so the two stay in agreement.
 */

#define SPHERE_RINGS   16
#define SPHERE_SECTORS 32

/* Writable views into a freshly allocated blob (the mesh.h accessors are const). */
static struct mesh_vertex *blob_verts(struct mesh_blob *b)
{
	return (struct mesh_vertex *)(b + 1);
}

static uint16_t *blob_indices(struct mesh_blob *b)
{
	return (uint16_t *)(blob_verts(b) + b->vertex_count);
}

static struct mesh_blob *blob_alloc(const struct memory_api *mem,
				    uint32_t vcount, uint32_t icount,
				    uint32_t *out_size)
{
	struct mesh_blob *b;
	uint32_t          size;

	size = mesh_blob_size(vcount, icount);
	b = mem->alloc(size);
	if (!b)
		return NULL;
	b->magic        = MESH_BLOB_MAGIC;
	b->vertex_count = vcount;
	b->index_count  = icount;
	b->index_format = 0; /* uint16 */
	if (out_size)
		*out_size = size;
	return b;
}

static void set_vertex(struct mesh_vertex *v, float px, float py, float pz,
		       float nx, float ny, float nz, float u, float w)
{
	v->position[0] = px;
	v->position[1] = py;
	v->position[2] = pz;
	v->normal[0]   = nx;
	v->normal[1]   = ny;
	v->normal[2]   = nz;
	v->uv0[0]      = u;
	v->uv0[1]      = w;
}

/* --- Cube: 6 quad faces, 24 verts / 36 indices --------------------------- */

struct quad_face {
	float n[3];  /* outward normal */
	float ax[3]; /* first in-plane axis (unit) */
	float ay[3]; /* second in-plane axis (unit) */
};

static const struct quad_face CUBE_FACES[6] = {
	{ {  1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
	{ { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
	{ {  0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
	{ {  0,-1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
	{ {  0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },
	{ {  0, 0,-1 }, { 1, 0, 0 }, { 0, 1, 0 } },
};

/* Corner sign pattern, CCW: (-,-), (+,-), (+,+), (-,+). */
static const float CORNER[4][2] = {
	{ -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 },
};

/*
 * Emit one axis-aligned quad face into verts[base..base+3] and its two
 * triangles into indices[ibase..]. center offsets the quad along its normal
 * (0.5 for a cube face, 0 for a flat plane).
 */
static void emit_quad(struct mesh_vertex *verts, uint16_t *indices,
		      uint16_t base, uint32_t ibase,
		      const struct quad_face *f, float center)
{
	int c;

	for (c = 0; c < 4; c++) {
		float sx = CORNER[c][0];
		float sy = CORNER[c][1];
		float px = center * f->n[0]
			 + 0.5f * sx * f->ax[0] + 0.5f * sy * f->ay[0];
		float py = center * f->n[1]
			 + 0.5f * sx * f->ax[1] + 0.5f * sy * f->ay[1];
		float pz = center * f->n[2]
			 + 0.5f * sx * f->ax[2] + 0.5f * sy * f->ay[2];

		set_vertex(&verts[base + c], px, py, pz,
			   f->n[0], f->n[1], f->n[2],
			   (sx + 1.0f) * 0.5f, (sy + 1.0f) * 0.5f);
	}

	indices[ibase + 0] = base + 0;
	indices[ibase + 1] = base + 1;
	indices[ibase + 2] = base + 2;
	indices[ibase + 3] = base + 0;
	indices[ibase + 4] = base + 2;
	indices[ibase + 5] = base + 3;
}

static struct mesh_blob *gen_cube(const struct memory_api *mem,
				  uint32_t *out_size)
{
	struct mesh_blob   *b;
	struct mesh_vertex *v;
	uint16_t           *idx;
	int                 f;

	b = blob_alloc(mem, 24, 36, out_size);
	if (!b)
		return NULL;
	v   = blob_verts(b);
	idx = blob_indices(b);
	for (f = 0; f < 6; f++)
		emit_quad(v, idx, (uint16_t)(f * 4), (uint32_t)(f * 6),
			  &CUBE_FACES[f], 0.5f);
	return b;
}

/* --- Plane: 1 quad on the XZ plane, 4 verts / 6 indices ------------------ */

static struct mesh_blob *gen_plane(const struct memory_api *mem,
				   uint32_t *out_size)
{
	static const struct quad_face top = {
		{ 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 1 },
	};
	struct mesh_blob *b;

	b = blob_alloc(mem, 4, 6, out_size);
	if (!b)
		return NULL;
	emit_quad(blob_verts(b), blob_indices(b), 0, 0, &top, 0.0f);
	return b;
}

/* --- Pyramid: square base + 4 sides, 16 verts / 18 indices --------------- */

static void normalize3(float v[3])
{
	float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (len > 1e-6f) {
		v[0] /= len;
		v[1] /= len;
		v[2] /= len;
	}
}

static struct mesh_blob *gen_pyramid(const struct memory_api *mem,
				     uint32_t *out_size)
{
	static const struct quad_face base = {
		{ 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 },
	};
	/* Base corners (y = -0.5), CCW seen from below; apex at the top. */
	static const float B[4][3] = {
		{ -0.5f, -0.5f, -0.5f }, {  0.5f, -0.5f, -0.5f },
		{  0.5f, -0.5f,  0.5f }, { -0.5f, -0.5f,  0.5f },
	};
	static const float apex[3] = { 0.0f, 0.5f, 0.0f };
	struct mesh_blob   *b;
	struct mesh_vertex *v;
	uint16_t           *idx;
	int                 s;

	b = blob_alloc(mem, 16, 18, out_size);
	if (!b)
		return NULL;
	v   = blob_verts(b);
	idx = blob_indices(b);

	/* Base quad occupies verts 0..3, indices 0..5. */
	emit_quad(v, idx, 0, 0, &base, -0.5f);

	/* Four triangular sides: verts 4.., indices 6.. */
	for (s = 0; s < 4; s++) {
		const float *p0 = B[s];
		const float *p1 = B[(s + 1) % 4];
		uint16_t     vb  = (uint16_t)(4 + s * 3);
		uint32_t     ib  = (uint32_t)(6 + s * 3);
		float        e0[3], e1[3], n[3];
		int          k;

		for (k = 0; k < 3; k++) {
			e0[k] = p1[k] - p0[k];
			e1[k] = apex[k] - p0[k];
		}
		n[0] = e0[1] * e1[2] - e0[2] * e1[1];
		n[1] = e0[2] * e1[0] - e0[0] * e1[2];
		n[2] = e0[0] * e1[1] - e0[1] * e1[0];
		normalize3(n);

		set_vertex(&v[vb + 0], p0[0], p0[1], p0[2],
			   n[0], n[1], n[2], 0.0f, 0.0f);
		set_vertex(&v[vb + 1], p1[0], p1[1], p1[2],
			   n[0], n[1], n[2], 1.0f, 0.0f);
		set_vertex(&v[vb + 2], apex[0], apex[1], apex[2],
			   n[0], n[1], n[2], 0.5f, 1.0f);

		idx[ib + 0] = vb + 0;
		idx[ib + 1] = vb + 1;
		idx[ib + 2] = vb + 2;
	}
	return b;
}

/* --- Sphere: UV sphere, 561 verts / 2880 indices ------------------------- */

static struct mesh_blob *gen_sphere(const struct memory_api *mem,
				    uint32_t *out_size)
{
	const uint32_t      rings   = SPHERE_RINGS;
	const uint32_t      sectors = SPHERE_SECTORS;
	const uint32_t      vcount  = (rings + 1) * (sectors + 1);
	struct mesh_blob   *b;
	struct mesh_vertex *v;
	uint16_t           *idx;
	uint32_t            r, s, w;

	/* Poles drop one triangle per sector: rings*sectors*6 - 2*sectors*3. */
	b = blob_alloc(mem, vcount, rings * sectors * 6 - sectors * 6,
		       out_size);
	if (!b)
		return NULL;
	v   = blob_verts(b);
	idx = blob_indices(b);

	for (r = 0; r <= rings; r++) {
		float theta = (float)M_PI * (float)r / (float)rings;
		float st    = sinf(theta);
		float ct    = cosf(theta);

		for (s = 0; s <= sectors; s++) {
			float phi = 2.0f * (float)M_PI
				  * (float)s / (float)sectors;
			float x = st * cosf(phi);
			float y = ct;
			float z = st * sinf(phi);

			set_vertex(&v[r * (sectors + 1) + s],
				   0.5f * x, 0.5f * y, 0.5f * z,
				   x, y, z,
				   (float)s / (float)sectors,
				   (float)r / (float)rings);
		}
	}

	w = 0;
	for (r = 0; r < rings; r++) {
		for (s = 0; s < sectors; s++) {
			uint16_t k1 = (uint16_t)(r * (sectors + 1) + s);
			uint16_t k2 = (uint16_t)(k1 + sectors + 1);

			if (r != 0) {
				idx[w++] = k1;
				idx[w++] = k2;
				idx[w++] = (uint16_t)(k1 + 1);
			}
			if (r != rings - 1) {
				idx[w++] = (uint16_t)(k1 + 1);
				idx[w++] = k2;
				idx[w++] = (uint16_t)(k2 + 1);
			}
		}
	}
	return b;
}

void *primitive_generate(enum primitive_kind kind,
			 const struct memory_api *mem, uint32_t *out_size)
{
	if (!mem)
		return NULL;

	switch (kind) {
	case PRIMITIVE_CUBE:    return gen_cube(mem, out_size);
	case PRIMITIVE_SPHERE:  return gen_sphere(mem, out_size);
	case PRIMITIVE_PLANE:   return gen_plane(mem, out_size);
	case PRIMITIVE_PYRAMID: return gen_pyramid(mem, out_size);
	}
	return NULL;
}
