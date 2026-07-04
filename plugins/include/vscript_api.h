/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VSCRIPT_API_H
#define VSCRIPT_API_H

#include <stdint.h>

/*
 * Visual-scripting graph core, published as the "vscript" subsystem api:
 *
 *   const struct vscript_api *vs = subsystem_manager_get_api(mgr, "vscript");
 *   vscript_graph_t g = vs->create(VSCRIPT_TARGET_SHADER);
 *   int32_t uv  = vs->add_node(g, "uv", NULL);
 *   int32_t out = vs->add_node(g, "output", NULL);
 *   vs->connect(g, uv, 0, out, 0);
 *
 * The core is target-agnostic: it owns nodes, typed ports, connections, a
 * node-type registry, and topological traversal.  Domain vocabulary (the
 * shader node set, sub-issue 2) is contributed through register_node_type;
 * the core ships no domain nodes itself.  Codegen backends (the GLSL ES 3.00
 * backend, sub-issue 3) consume a graph through the introspection calls
 * below and topo_order.
 *
 * Port types are opaque uint32_t tags the domain defines (see the shader type
 * lattice in shader_graph.h).  The core treats them as equality-checked tags
 * only: a connection is rejected unless the driving output port's type equals
 * the driven input port's type.  Tag 0 is reserved as "invalid".
 */

/* Opaque graph handle; never dereferenced across the plugin boundary. */
typedef struct vscript_graph *vscript_graph_t;

/*
 * Target discriminator, carried in the .vscript header and mirrored to an
 * asset declaration (key VSCRIPT_TARGET_DECL_KEY) so consumers can filter a
 * graph asset without decoding it.  Shaders are the first backend; game-script
 * targets are a sibling epic.
 */
#define VSCRIPT_TARGET_INVALID    0
#define VSCRIPT_TARGET_SHADER     1
#define VSCRIPT_TARGET_GAMESCRIPT 2

/* Decl key under which a graph asset advertises its target (see target()). */
#define VSCRIPT_TARGET_DECL_KEY "target"

/* A named, typed port on a node type. */
struct vscript_port {
	const char *name;
	uint32_t    type; /* domain type tag; 0 = invalid */
};

/*
 * A node type contributed to the registry.  name, inputs and outputs must
 * have static lifetime (string literals / static arrays) — the registry
 * retains the pointers.  user is opaque domain metadata (e.g. a codegen
 * template) the core stores and hands back through node_user().
 */
struct vscript_node_type {
	const char                *name;
	const struct vscript_port *inputs;
	const struct vscript_port *outputs;
	const void                *user;
	uint32_t                   input_count;
	uint32_t                   output_count;
};

struct vscript_api {
	/* Create an empty graph for target; NULL on OOM. */
	vscript_graph_t (*create)(uint32_t target);
	/* Destroy a graph and free its storage. */
	void            (*destroy)(vscript_graph_t g);

	/*
	 * Register a node type into the process-wide registry.  Domains call
	 * this at plugin_entry.  Returns 0 on success, -1 on a duplicate name,
	 * a full registry, or a malformed descriptor.
	 */
	int32_t (*register_node_type)(const struct vscript_node_type *desc);

	/*
	 * Add a node of the named type, carrying an optional param string
	 * (e.g. a constant literal or a uniform name).  Returns the new stable
	 * node id (>= 1), or -1 if the type is unknown or the graph is full.
	 */
	int32_t (*add_node)(vscript_graph_t g, const char *type_name,
			    const char *param);

	/*
	 * Wire an output port to an input port.  Rejects (returns -1) an
	 * unknown node, an out-of-range port, a type mismatch, or an input
	 * that already has a driver (single driver per input).  Returns 0 on
	 * success.  Cycles are detected by validate()/topo_order().
	 */
	int32_t (*connect)(vscript_graph_t g, int32_t from_node,
			   uint32_t from_port, int32_t to_node,
			   uint32_t to_port);

	/* 0 if the graph is a well-formed DAG, -1 if it contains a cycle. */
	int32_t (*validate)(vscript_graph_t g);
	/*
	 * Write node ids in a topological (drivers-before-dependents) order
	 * into out (capacity max).  Returns the node count, or -1 on a cycle
	 * or if max is too small.
	 */
	int32_t (*topo_order)(vscript_graph_t g, int32_t *out, uint32_t max);

	/* The graph's target discriminator (VSCRIPT_TARGET_*). */
	uint32_t (*target)(vscript_graph_t g);
	/* 0 if the graph's target equals want, -1 otherwise. */
	int32_t  (*require_target)(vscript_graph_t g, uint32_t want);

	/* --- Introspection consumed by codegen backends --------------- */

	/* Registered type name of a node, or NULL if the id is unknown. */
	const char *(*node_type_name)(vscript_graph_t g, int32_t node);
	/* The node's param string, or NULL if none / unknown id. */
	const char *(*node_param)(vscript_graph_t g, int32_t node);
	/* The node type's opaque domain metadata, or NULL. */
	const void *(*node_user)(vscript_graph_t g, int32_t node);
	/* Port counts for a node (0 if the id is unknown). */
	uint32_t    (*node_input_count)(vscript_graph_t g, int32_t node);
	uint32_t    (*node_output_count)(vscript_graph_t g, int32_t node);
	/* Type tag of a port; 0 if the node/port is out of range. */
	uint32_t    (*port_type)(vscript_graph_t g, int32_t node,
				 int32_t is_output, uint32_t port);
	/*
	 * Resolve the driver of an input port.  On a connected input, writes
	 * the driving node id and output port and returns 0; returns -1 if the
	 * input is unconnected or out of range.  out_node / out_port may be
	 * NULL.
	 */
	int32_t     (*input_source)(vscript_graph_t g, int32_t node,
				    uint32_t port, int32_t *out_node,
				    uint32_t *out_port);
	/* Number of live nodes in the graph. */
	uint32_t    (*node_count)(vscript_graph_t g);
};

#endif /* VSCRIPT_API_H */
