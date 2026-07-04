/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "shader_graph.h"
#include "vscript_api.h"
#include "asset_api.h"
#include "memory_api.h"

#include <string.h>
#include <stddef.h>
#include <stdint.h>

/*
 * GLSL ES 3.00 codegen backend (sub-issue 3).  Walks a shader-target graph in
 * topological order and emits a fragment shader the WebGL renderer compiles
 * unchanged: fixed header + varying, one `uniform` per parameter node, one SSA
 * temp per interior node, and the surface Output assigned to frag_color.
 * sampler2D values are never bound to a local (illegal in GLSL) — their uniform
 * name is inlined at the use site instead.
 */

#ifdef __EMSCRIPTEN__
static const struct memory_api *g_mem;
#else
#include "memory.h"
static const struct memory_api native_mem = {
	mem_alloc, mem_alloc_zero, mem_free,
	mem_pool_create, mem_pool_alloc, mem_pool_free, mem_pool_destroy,
};
static const struct memory_api *g_mem = &native_mem;
#endif

void sg_set_mem(const struct memory_api *mem)
{
	if (mem)
		g_mem = mem;
}

#define SG_SRC_MAX  8192
#define SG_MAX_NODE 128
#define SG_EXPR_MAX 64

static const char *gltype(uint32_t type)
{
	switch (type) {
	case SG_TYPE_FLOAT:     return "float";
	case SG_TYPE_VEC2:      return "vec2";
	case SG_TYPE_VEC3:      return "vec3";
	case SG_TYPE_VEC4:      return "vec4";
	case SG_TYPE_SAMPLER2D: return "sampler2D";
	default:                return NULL;
	}
}

/* Bounded string builder over a caller-provided buffer. */
struct sb {
	char    *buf;
	uint32_t cap;
	uint32_t len;
	int32_t  err;
};

static void sb_puts(struct sb *s, const char *str)
{
	size_t n = strlen(str);

	if (s->err || s->len + n + 1 > s->cap) {
		s->err = 1;
		return;
	}
	memcpy(s->buf + s->len, str, n);
	s->len += (uint32_t)n;
	s->buf[s->len] = '\0';
}

/* Append a small non-negative integer in decimal. */
static void sb_putu(struct sb *s, int32_t v)
{
	char tmp[12];
	int  i = 0;

	if (v == 0) {
		sb_puts(s, "0");
		return;
	}
	while (v > 0 && i < (int)sizeof(tmp)) {
		tmp[i++] = (char)('0' + v % 10);
		v /= 10;
	}
	while (i-- > 0) {
		char c[2] = { tmp[i], '\0' };

		sb_puts(s, c);
	}
}

/* Per-node output expression table (SSA temp name or inlined sampler name). */
struct expr_tab {
	int32_t id[SG_MAX_NODE];
	char    str[SG_MAX_NODE][SG_EXPR_MAX];
	uint32_t n;
};

static const char *expr_of(const struct expr_tab *t, int32_t id)
{
	uint32_t i;

	for (i = 0; i < t->n; i++) {
		if (t->id[i] == id)
			return t->str[i];
	}
	return NULL;
}

/* Expand a node's GLSL template into sb: $0..$9 = inputs, $p = param. */
static void expand(const struct vscript_api *vs, vscript_graph_t g,
		   int32_t node, const struct expr_tab *tab,
		   const char *tmpl, struct sb *sb)
{
	const char *p;

	for (p = tmpl; *p; p++) {
		if (*p != '$') {
			char c[2] = { *p, '\0' };

			sb_puts(sb, c);
			continue;
		}
		p++;
		if (*p >= '0' && *p <= '9') {
			int32_t     src = 0;
			const char *e;

			if (vs->input_source(g, node, (uint32_t)(*p - '0'),
					     &src, NULL) != 0) {
				sb->err = 1;
				return;
			}
			e = expr_of(tab, src);
			if (!e) {
				sb->err = 1;
				return;
			}
			sb_puts(sb, e);
		} else if (*p == 'p') {
			const char *param = vs->node_param(g, node);

			if (!param) {
				sb->err = 1;
				return;
			}
			sb_puts(sb, param);
		} else if (*p == '\0') {
			break;
		} else {
			char c[3] = { '$', *p, '\0' };

			sb_puts(sb, c);
		}
	}
}

/* Emit one `uniform` per distinct parameter node, in topo order. */
static void emit_uniforms(const struct vscript_api *vs, vscript_graph_t g,
			  const int32_t *order, int32_t n, struct sb *sb)
{
	const char *seen[SG_MAX_NODE];
	uint32_t    seen_n = 0;
	int32_t     i;
	uint32_t    j;

	for (i = 0; i < n; i++) {
		const struct sg_node_meta *m = vs->node_user(g, order[i]);
		const char                *name;
		const char                *ty;
		int                        dup = 0;

		if (!m || !m->is_uniform)
			continue;
		name = vs->node_param(g, order[i]);
		ty   = gltype(vs->port_type(g, order[i], 1, 0));
		if (!name || !ty) {
			sb->err = 1;
			return;
		}
		for (j = 0; j < seen_n; j++) {
			if (strcmp(seen[j], name) == 0) {
				dup = 1;
				break;
			}
		}
		if (dup)
			continue;
		seen[seen_n++] = name;

		sb_puts(sb, "uniform ");
		sb_puts(sb, ty);
		sb_puts(sb, " ");
		sb_puts(sb, name);
		sb_puts(sb, ";\n");
	}
}

char *sg_emit_fragment(const struct vscript_api *vs, vscript_graph_t g,
		       uint32_t *out_size)
{
	int32_t         order[SG_MAX_NODE];
	struct expr_tab tab;
	struct sb       sb;
	int32_t         n, i, out_node = -1, out_count = 0;

	if (!vs || !g)
		return NULL;
	if (vs->require_target(g, VSCRIPT_TARGET_SHADER) != 0)
		return NULL;

	n = vs->topo_order(g, order, SG_MAX_NODE);
	if (n < 0)
		return NULL;

	for (i = 0; i < n; i++) {
		const struct sg_node_meta *m = vs->node_user(g, order[i]);

		if (m && m->is_output) {
			out_node = order[i];
			out_count++;
		}
	}
	if (out_count != 1)
		return NULL; /* v1 needs exactly one surface Output */

	sb.buf = g_mem->alloc(SG_SRC_MAX);
	if (!sb.buf)
		return NULL;
	sb.cap = SG_SRC_MAX;
	sb.len = 0;
	sb.err = 0;
	sb.buf[0] = '\0';
	tab.n = 0;

	sb_puts(&sb, "#version 300 es\n");
	sb_puts(&sb, "precision highp float;\n\n");
	sb_puts(&sb, "in vec2 v_uv;\n");
	emit_uniforms(vs, g, order, n, &sb);
	sb_puts(&sb, "\nout vec4 frag_color;\n\nvoid main()\n{\n");

	for (i = 0; i < n; i++) {
		int32_t                    id = order[i];
		const struct sg_node_meta *m  = vs->node_user(g, id);
		uint32_t                   ot;

		if (!m) {
			sb.err = 1;
			break;
		}
		if (m->is_output)
			continue;

		ot = vs->port_type(g, id, 1, 0);
		if (tab.n >= SG_MAX_NODE) {
			sb.err = 1;
			break;
		}

		if (ot == SG_TYPE_SAMPLER2D) {
			/* Inline the sampler uniform; no local binding. */
			struct sb es;

			es.buf = tab.str[tab.n];
			es.cap = SG_EXPR_MAX;
			es.len = 0;
			es.err = 0;
			es.buf[0] = '\0';
			expand(vs, g, id, &tab, m->tmpl, &es);
			if (es.err) {
				sb.err = 1;
				break;
			}
			tab.id[tab.n] = id;
			tab.n++;
			continue;
		}

		/* SSA temp: `<type> v<id> = <expr>;` */
		sb_puts(&sb, "\t");
		sb_puts(&sb, gltype(ot));
		sb_puts(&sb, " v");
		sb_putu(&sb, id);
		sb_puts(&sb, " = ");
		expand(vs, g, id, &tab, m->tmpl, &sb);
		sb_puts(&sb, ";\n");

		tab.id[tab.n] = id;
		{
			struct sb name = { tab.str[tab.n], SG_EXPR_MAX, 0, 0 };

			name.buf[0] = '\0';
			sb_puts(&name, "v");
			sb_putu(&name, id);
			if (name.err)
				sb.err = 1;
		}
		tab.n++;
	}

	if (!sb.err) {
		const struct sg_node_meta *m = vs->node_user(g, out_node);

		sb_puts(&sb, "\tfrag_color = ");
		expand(vs, g, out_node, &tab, m->tmpl, &sb);
		sb_puts(&sb, ";\n}\n");
	}

	if (sb.err) {
		g_mem->free(sb.buf);
		return NULL;
	}
	if (out_size)
		*out_size = sb.len + 1; /* include the NUL, like other shaders */
	return sb.buf;
}

uint32_t sg_compile_to_shader(const struct vscript_api *vs,
			      const struct asset_mut_api *mut,
			      vscript_graph_t g, const char *out_path)
{
	struct asset_decl_field decl[2];
	char                   *src;
	uint32_t                size = 0;
	uint32_t                id;

	if (!mut)
		return 0;
	src = sg_emit_fragment(vs, g, &size);
	if (!src)
		return 0;

	id = mut->create(out_path, ASSET_TYPE_SHADER, src, size);
	if (id) {
		decl[0].key = "dialect"; decl[0].value = "glsl_es_300";
		decl[1].key = "stage";   decl[1].value = "fragment";
		mut->set_decl(id, decl, 2);
	}

	g_mem->free(src);
	return id;
}

const char *sg_passthrough_vertex(void)
{
	return
		"#version 300 es\n"
		"layout(location = 0) in vec3 a_pos;\n"
		"layout(location = 1) in vec2 a_uv;\n"
		"out vec2 v_uv;\n"
		"void main()\n"
		"{\n"
		"\tv_uv = a_uv;\n"
		"\tgl_Position = vec4(a_pos, 1.0);\n"
		"}\n";
}
