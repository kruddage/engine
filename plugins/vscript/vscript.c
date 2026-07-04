/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "vscript.h"
#include "asset_codec_api.h"
#include "subsystem_manager.h"
#include "memory_api.h"

#include <string.h>
#include <stddef.h>
#include <stdint.h>

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

/*
 * Fixed capacities.  A visual-scripting graph is an authoring artifact, not a
 * runtime hot structure; these ceilings keep the graph a single allocation and
 * every lookup a bounded scan.
 */
#define VSCRIPT_MAX_NODES    128
#define VSCRIPT_MAX_CONNS    256
#define VSCRIPT_MAX_TYPES     64
#define VSCRIPT_MAX_PORTS      8
#define VSCRIPT_PARAM_MAX     64

/* Process-wide node-type registry entry (deep-copies only scalars/ports). */
struct type_entry {
	const char          *name;
	const void          *user;
	struct vscript_port  inputs[VSCRIPT_MAX_PORTS];
	struct vscript_port  outputs[VSCRIPT_MAX_PORTS];
	uint32_t             input_count;
	uint32_t             output_count;
};

static struct type_entry g_types[VSCRIPT_MAX_TYPES];
static uint32_t          g_type_count;

struct node {
	int32_t  id;
	int32_t  type;                 /* index into g_types */
	char     param[VSCRIPT_PARAM_MAX];
	uint8_t  has_param;
};

struct conn {
	int32_t  from_node;
	uint32_t from_port;
	int32_t  to_node;
	uint32_t to_port;
};

struct vscript_graph {
	uint32_t     target;
	uint32_t     node_count;
	uint32_t     conn_count;
	int32_t      next_id;
	struct node  nodes[VSCRIPT_MAX_NODES];
	struct conn  conns[VSCRIPT_MAX_CONNS];
};

/* --- Registry ----------------------------------------------------------- */

static int32_t find_type(const char *name)
{
	uint32_t i;

	if (!name)
		return -1;
	for (i = 0; i < g_type_count; i++) {
		if (strcmp(g_types[i].name, name) == 0)
			return (int32_t)i;
	}
	return -1;
}

static int32_t register_node_type(const struct vscript_node_type *desc)
{
	struct type_entry *e;
	uint32_t           i;

	if (!desc || !desc->name)
		return -1;
	if (desc->input_count > VSCRIPT_MAX_PORTS
			|| desc->output_count > VSCRIPT_MAX_PORTS)
		return -1;
	if (g_type_count >= VSCRIPT_MAX_TYPES)
		return -1;
	if (find_type(desc->name) >= 0)
		return -1;

	/* Every declared port must carry a valid (non-zero) type tag. */
	for (i = 0; i < desc->input_count; i++) {
		if (desc->inputs[i].type == 0)
			return -1;
	}
	for (i = 0; i < desc->output_count; i++) {
		if (desc->outputs[i].type == 0)
			return -1;
	}

	e = &g_types[g_type_count];
	e->name         = desc->name;
	e->input_count  = desc->input_count;
	e->output_count = desc->output_count;
	e->user         = desc->user;
	for (i = 0; i < desc->input_count; i++)
		e->inputs[i] = desc->inputs[i];
	for (i = 0; i < desc->output_count; i++)
		e->outputs[i] = desc->outputs[i];

	g_type_count++;
	return 0;
}

/* --- Graph lifetime + node lookup --------------------------------------- */

static vscript_graph_t graph_create(uint32_t target)
{
	struct vscript_graph *g;

	g = g_mem->alloc_zero(sizeof(*g));
	if (!g)
		return NULL;
	g->target  = target;
	g->next_id = 1;
	return g;
}

static void graph_destroy(vscript_graph_t g)
{
	if (g)
		g_mem->free(g);
}

static struct node *node_by_id(vscript_graph_t g, int32_t id)
{
	uint32_t i;

	if (!g || id <= 0)
		return NULL;
	for (i = 0; i < g->node_count; i++) {
		if (g->nodes[i].id == id)
			return &g->nodes[i];
	}
	return NULL;
}

/* Insert a node with an explicit id; keeps next_id ahead of every id. */
static int32_t add_node_raw(vscript_graph_t g, int32_t type, int32_t id,
			    const char *param)
{
	struct node *n;

	if (g->node_count >= VSCRIPT_MAX_NODES)
		return -1;

	n = &g->nodes[g->node_count];
	n->id        = id;
	n->type      = type;
	n->has_param = 0;
	n->param[0]  = '\0';
	if (param) {
		size_t len = strlen(param);

		if (len >= VSCRIPT_PARAM_MAX)
			return -1;
		memcpy(n->param, param, len + 1);
		n->has_param = 1;
	}

	g->node_count++;
	if (id >= g->next_id)
		g->next_id = id + 1;
	return id;
}

static int32_t add_node(vscript_graph_t g, const char *type_name,
			const char *param)
{
	int32_t type;

	if (!g)
		return -1;
	type = find_type(type_name);
	if (type < 0)
		return -1;
	return add_node_raw(g, type, g->next_id, param);
}

/* --- Connections + validation ------------------------------------------- */

static int32_t connect(vscript_graph_t g, int32_t from_node,
		       uint32_t from_port, int32_t to_node, uint32_t to_port)
{
	struct node *from, *to;
	uint32_t     i;

	if (!g || g->conn_count >= VSCRIPT_MAX_CONNS)
		return -1;

	from = node_by_id(g, from_node);
	to   = node_by_id(g, to_node);
	if (!from || !to)
		return -1;
	if (from_port >= g_types[from->type].output_count)
		return -1;
	if (to_port >= g_types[to->type].input_count)
		return -1;

	/* Type-checked: driver output tag must equal driven input tag. */
	if (g_types[from->type].outputs[from_port].type
			!= g_types[to->type].inputs[to_port].type)
		return -1;

	/* Single driver per input. */
	for (i = 0; i < g->conn_count; i++) {
		if (g->conns[i].to_node == to_node
				&& g->conns[i].to_port == to_port)
			return -1;
	}

	g->conns[g->conn_count].from_node = from_node;
	g->conns[g->conn_count].from_port = from_port;
	g->conns[g->conn_count].to_node   = to_node;
	g->conns[g->conn_count].to_port   = to_port;
	g->conn_count++;
	return 0;
}

/*
 * Kahn's algorithm over node-level edges (from_node drives to_node).  Writes
 * a topological order into out and returns the node count; returns -1 on a
 * cycle or if out cannot hold every node.
 */
static int32_t topo_order(vscript_graph_t g, int32_t *out, uint32_t max)
{
	uint32_t indeg[VSCRIPT_MAX_NODES];
	int32_t  queue[VSCRIPT_MAX_NODES];
	uint32_t head = 0, tail = 0, done = 0;
	uint32_t i, c;

	if (!g || !out || max < g->node_count)
		return -1;

	for (i = 0; i < g->node_count; i++)
		indeg[i] = 0;
	for (c = 0; c < g->conn_count; c++) {
		for (i = 0; i < g->node_count; i++) {
			if (g->nodes[i].id == g->conns[c].to_node) {
				indeg[i]++;
				break;
			}
		}
	}

	for (i = 0; i < g->node_count; i++) {
		if (indeg[i] == 0)
			queue[tail++] = (int32_t)i;
	}

	while (head < tail) {
		int32_t slot = queue[head++];
		int32_t id   = g->nodes[slot].id;

		out[done++] = id;
		for (c = 0; c < g->conn_count; c++) {
			if (g->conns[c].from_node != id)
				continue;
			for (i = 0; i < g->node_count; i++) {
				if (g->nodes[i].id != g->conns[c].to_node)
					continue;
				if (--indeg[i] == 0)
					queue[tail++] = (int32_t)i;
				break;
			}
		}
	}

	if (done != g->node_count)
		return -1; /* a cycle left nodes unresolved */
	return (int32_t)g->node_count;
}

static int32_t validate(vscript_graph_t g)
{
	int32_t scratch[VSCRIPT_MAX_NODES];

	return topo_order(g, scratch, VSCRIPT_MAX_NODES) < 0 ? -1 : 0;
}

/* --- Target ------------------------------------------------------------- */

static uint32_t graph_target(vscript_graph_t g)
{
	return g ? g->target : VSCRIPT_TARGET_INVALID;
}

static int32_t require_target(vscript_graph_t g, uint32_t want)
{
	return (g && g->target == want) ? 0 : -1;
}

const char *vscript_target_decl_value(uint32_t target)
{
	switch (target) {
	case VSCRIPT_TARGET_SHADER:
		return "shader";
	case VSCRIPT_TARGET_GAMESCRIPT:
		return "gamescript";
	default:
		return NULL;
	}
}

/* --- Introspection ------------------------------------------------------ */

static const char *node_type_name(vscript_graph_t g, int32_t id)
{
	struct node *n = node_by_id(g, id);

	return n ? g_types[n->type].name : NULL;
}

static const char *node_param(vscript_graph_t g, int32_t id)
{
	struct node *n = node_by_id(g, id);

	return (n && n->has_param) ? n->param : NULL;
}

static const void *node_user(vscript_graph_t g, int32_t id)
{
	struct node *n = node_by_id(g, id);

	return n ? g_types[n->type].user : NULL;
}

static uint32_t node_input_count(vscript_graph_t g, int32_t id)
{
	struct node *n = node_by_id(g, id);

	return n ? g_types[n->type].input_count : 0;
}

static uint32_t node_output_count(vscript_graph_t g, int32_t id)
{
	struct node *n = node_by_id(g, id);

	return n ? g_types[n->type].output_count : 0;
}

static uint32_t port_type(vscript_graph_t g, int32_t id, int32_t is_output,
			  uint32_t port)
{
	struct node       *n = node_by_id(g, id);
	struct type_entry *t;

	if (!n)
		return 0;
	t = &g_types[n->type];
	if (is_output)
		return port < t->output_count ? t->outputs[port].type : 0;
	return port < t->input_count ? t->inputs[port].type : 0;
}

static int32_t input_source(vscript_graph_t g, int32_t id, uint32_t port,
			    int32_t *out_node, uint32_t *out_port)
{
	uint32_t i;

	if (!g)
		return -1;
	for (i = 0; i < g->conn_count; i++) {
		if (g->conns[i].to_node != id || g->conns[i].to_port != port)
			continue;
		if (out_node)
			*out_node = g->conns[i].from_node;
		if (out_port)
			*out_port = g->conns[i].from_port;
		return 0;
	}
	return -1;
}

static uint32_t node_count(vscript_graph_t g)
{
	return g ? g->node_count : 0;
}

static int32_t node_id_at(vscript_graph_t g, uint32_t index)
{
	if (!g || index >= g->node_count)
		return -1;
	return g->nodes[index].id;
}

/* Drop connection slot i by compacting the tail down over it. */
static void conn_erase(vscript_graph_t g, uint32_t i)
{
	uint32_t k;

	for (k = i + 1; k < g->conn_count; k++)
		g->conns[k - 1] = g->conns[k];
	g->conn_count--;
}

static int32_t remove_node(vscript_graph_t g, int32_t id)
{
	uint32_t i;

	if (!node_by_id(g, id))
		return -1;

	/* Drop every connection touching the node (iterate in place). */
	i = 0;
	while (i < g->conn_count) {
		if (g->conns[i].from_node == id
				|| g->conns[i].to_node == id)
			conn_erase(g, i);
		else
			i++;
	}

	for (i = 0; i < g->node_count; i++) {
		if (g->nodes[i].id != id)
			continue;
		for (; i + 1 < g->node_count; i++)
			g->nodes[i] = g->nodes[i + 1];
		g->node_count--;
		return 0;
	}
	return -1;
}

static int32_t set_param(vscript_graph_t g, int32_t id, const char *param)
{
	struct node *n = node_by_id(g, id);
	size_t       len;

	if (!n)
		return -1;
	n->has_param = 0;
	n->param[0]  = '\0';
	if (param) {
		len = strlen(param);
		if (len >= VSCRIPT_PARAM_MAX)
			return -1;
		memcpy(n->param, param, len + 1);
		n->has_param = 1;
	}
	return 0;
}

static int32_t disconnect(vscript_graph_t g, int32_t to_node, uint32_t to_port)
{
	uint32_t i;

	if (!g)
		return -1;
	for (i = 0; i < g->conn_count; i++) {
		if (g->conns[i].to_node == to_node
				&& g->conns[i].to_port == to_port) {
			conn_erase(g, i);
			return 0;
		}
	}
	return -1;
}

static uint32_t type_count(void)
{
	return g_type_count;
}

static const char *type_name_at(uint32_t index)
{
	return index < g_type_count ? g_types[index].name : NULL;
}

/* --- Binary .vscript codec ---------------------------------------------- */

struct vscript_header {
	uint8_t  magic[4];     /* "KVSG" */
	uint32_t version;      /* 1 */
	uint32_t target;
	uint32_t node_count;
	uint32_t conn_count;
	uint32_t string_bytes;
};

struct node_record {
	uint32_t id;
	uint32_t type_off;     /* offset into string blob */
	uint32_t param_off;    /* offset into blob, or 0xFFFFFFFF = none */
};

struct conn_record {
	uint32_t from_node;
	uint32_t from_port;
	uint32_t to_node;
	uint32_t to_port;
};

#define VSCRIPT_MAGIC   "KVSG"
#define VSCRIPT_VERSION 1u
#define VSCRIPT_NO_PARAM 0xFFFFFFFFu

/* Append a NUL-terminated string to blob, returning its start offset. */
static uint32_t blob_put(char *blob, uint32_t *used, const char *s)
{
	uint32_t off = *used;
	size_t   len = strlen(s) + 1;

	memcpy(blob + off, s, len);
	*used += (uint32_t)len;
	return off;
}

void *vscript_encode(const void *typed, uint32_t *out_size)
{
	const struct vscript_graph *g = typed;
	struct vscript_header      *hdr;
	struct node_record         *nrec;
	struct conn_record         *crec;
	char                       *blob;
	uint8_t                    *buf;
	uint32_t                    string_bytes = 0;
	uint32_t                    used = 0;
	size_t                      nb, cb, total;
	uint32_t                    i;

	if (!g)
		return NULL;

	/* Blob holds each node's type name and optional param, packed. */
	for (i = 0; i < g->node_count; i++) {
		string_bytes +=
			(uint32_t)strlen(g_types[g->nodes[i].type].name) + 1;
		if (g->nodes[i].has_param)
			string_bytes += (uint32_t)strlen(g->nodes[i].param) + 1;
	}

	nb    = (size_t)g->node_count * sizeof(struct node_record);
	cb    = (size_t)g->conn_count * sizeof(struct conn_record);
	total = sizeof(*hdr) + nb + cb + string_bytes;

	buf = g_mem->alloc(total);
	if (!buf)
		return NULL;

	hdr  = (struct vscript_header *)buf;
	nrec = (struct node_record *)(buf + sizeof(*hdr));
	crec = (struct conn_record *)((uint8_t *)nrec + nb);
	blob = (char *)((uint8_t *)crec + cb);

	memcpy(hdr->magic, VSCRIPT_MAGIC, 4);
	hdr->version      = VSCRIPT_VERSION;
	hdr->target       = g->target;
	hdr->node_count   = g->node_count;
	hdr->conn_count   = g->conn_count;
	hdr->string_bytes = string_bytes;

	for (i = 0; i < g->node_count; i++) {
		nrec[i].id = (uint32_t)g->nodes[i].id;
		nrec[i].type_off =
			blob_put(blob, &used, g_types[g->nodes[i].type].name);
		nrec[i].param_off = g->nodes[i].has_param
			? blob_put(blob, &used, g->nodes[i].param)
			: VSCRIPT_NO_PARAM;
	}
	for (i = 0; i < g->conn_count; i++) {
		crec[i].from_node = (uint32_t)g->conns[i].from_node;
		crec[i].from_port = g->conns[i].from_port;
		crec[i].to_node   = (uint32_t)g->conns[i].to_node;
		crec[i].to_port   = g->conns[i].to_port;
	}

	if (out_size)
		*out_size = (uint32_t)total;
	return buf;
}

/* A blob offset must land inside the blob and its string be NUL-terminated. */
static int32_t blob_str_ok(const char *blob, uint32_t bytes, uint32_t off)
{
	if (off >= bytes)
		return -1;
	if (blob[bytes - 1] != '\0')
		return -1;
	return 0;
}

void *vscript_decode(const void *bytes, uint32_t size)
{
	const struct vscript_header *hdr;
	const struct node_record    *nrec;
	const struct conn_record    *crec;
	const char                  *blob;
	struct vscript_graph        *g;
	size_t                       nb, cb;
	uint32_t                     i;

	if (size < (uint32_t)sizeof(*hdr))
		return NULL;

	hdr = bytes;
	if (memcmp(hdr->magic, VSCRIPT_MAGIC, 4) != 0)
		return NULL;
	if (hdr->version != VSCRIPT_VERSION)
		return NULL;
	if (hdr->node_count > VSCRIPT_MAX_NODES
			|| hdr->conn_count > VSCRIPT_MAX_CONNS)
		return NULL;

	nb = (size_t)hdr->node_count * sizeof(struct node_record);
	cb = (size_t)hdr->conn_count * sizeof(struct conn_record);
	if ((size_t)size < sizeof(*hdr) + nb + cb + (size_t)hdr->string_bytes)
		return NULL;

	nrec = (const struct node_record *)((const uint8_t *)bytes
					    + sizeof(*hdr));
	crec = (const struct conn_record *)((const uint8_t *)nrec + nb);
	blob = (const char *)((const uint8_t *)crec + cb);

	g = graph_create(hdr->target);
	if (!g)
		return NULL;

	for (i = 0; i < hdr->node_count; i++) {
		const char *type_name;
		const char *param = NULL;
		int32_t     type;

		if (blob_str_ok(blob, hdr->string_bytes, nrec[i].type_off) != 0)
			goto fail;
		type_name = blob + nrec[i].type_off;
		type = find_type(type_name);
		if (type < 0)
			goto fail; /* unregistered node type */

		if (nrec[i].param_off != VSCRIPT_NO_PARAM) {
			if (blob_str_ok(blob, hdr->string_bytes,
					nrec[i].param_off) != 0)
				goto fail;
			param = blob + nrec[i].param_off;
		}
		if (add_node_raw(g, type, (int32_t)nrec[i].id, param) < 0)
			goto fail;
	}

	for (i = 0; i < hdr->conn_count; i++) {
		if (connect(g, (int32_t)crec[i].from_node, crec[i].from_port,
			    (int32_t)crec[i].to_node, crec[i].to_port) != 0)
			goto fail;
	}

	return g;
fail:
	graph_destroy(g);
	return NULL;
}

/* --- Service vtable + entry --------------------------------------------- */

static const struct vscript_api g_vscript_api = {
	.create             = graph_create,
	.destroy            = graph_destroy,
	.register_node_type = register_node_type,
	.add_node           = add_node,
	.connect            = connect,
	.validate           = validate,
	.topo_order         = topo_order,
	.target             = graph_target,
	.require_target     = require_target,
	.node_type_name     = node_type_name,
	.node_param         = node_param,
	.node_user          = node_user,
	.node_input_count   = node_input_count,
	.node_output_count  = node_output_count,
	.port_type          = port_type,
	.input_source       = input_source,
	.node_count         = node_count,
	.node_id_at         = node_id_at,
	.remove_node        = remove_node,
	.disconnect         = disconnect,
	.set_param          = set_param,
	.type_count         = type_count,
	.type_name_at       = type_name_at,
};

const struct vscript_api *vscript_native_api(void)
{
	return &g_vscript_api;
}

static const struct subsystem vscript_desc = {
	.name = "vscript",
	.api  = &g_vscript_api,
};

#ifdef __EMSCRIPTEN__
void plugin_entry(struct subsystem_manager *mgr)
#else
void vscript_plugin_entry(struct subsystem_manager *mgr)
#endif
{
	const struct asset_codec_api *codec;

#ifdef __EMSCRIPTEN__
	g_mem = subsystem_manager_get_api(mgr, "memory");
#endif
	codec = subsystem_manager_get_api(mgr, "asset_codec");
	if (codec) {
		codec->register_codec("vscript", vscript_decode);
		codec->register_encoder("vscript", vscript_encode);
	}
	subsystem_manager_register(mgr, &vscript_desc);
}
