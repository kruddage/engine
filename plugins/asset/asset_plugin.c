/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "asset.h"
#include "asset_api.h"
#include "asset_codec_api.h"
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log_api.h"
#include "memory_api.h"

#include <string.h>
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/fetch.h>
#else
#include <stdio.h>
#include "log.h"
#include "memory.h"
static const struct log_api    native_log = { log_write };
static const struct memory_api native_mem = {
	mem_alloc, mem_alloc_zero, mem_free,
	mem_pool_create, mem_pool_alloc, mem_pool_free, mem_pool_destroy,
};
#endif

#define ASSET_CACHE_MAX  64
#define CODEC_TABLE_MAX  16

/*
 * Authored assets may carry a small declaration set (e.g. a shader's
 * stage/dialect).  Stored inline so describe() can hand out pointers into
 * the entry's own storage, valid until the entry is evicted or re-declared.
 */
#define ASSET_DECL_MAX     8
#define ASSET_DECL_KEY_MAX 24
#define ASSET_DECL_VAL_MAX 48

struct asset_decl_store {
	char key[ASSET_DECL_KEY_MAX];
	char val[ASSET_DECL_VAL_MAX];
};

struct asset_entry {
	char     path[ASSET_PATH_MAX];
	uint8_t *data;
	uint32_t size;
	int32_t  refs;
	int32_t  state;    /* asset_state */
	int32_t  kind;     /* ASSET_KIND_* */
	int32_t  read_only;
	int32_t  type;     /* ASSET_TYPE_* */
	uint32_t id;       /* stable identity; never 0, never reused */
	int32_t  origin;   /* ASSET_ORIGIN_* */
	struct asset_decl_store decl[ASSET_DECL_MAX];
	uint32_t ndecl;    /* authored declaration field count; 0 = none */
};

struct codec_entry {
	char  ext[16];
	void *(*decode)(const void *bytes, uint32_t size);
};

static struct asset_entry cache[ASSET_CACHE_MAX];
static int32_t            cache_count;

static struct codec_entry codec_table[CODEC_TABLE_MAX];
static int32_t            codec_count;

static uint32_t next_asset_id = 1; /* 0 is reserved for "none" */

#ifdef __EMSCRIPTEN__
static const struct log_api    *g_log;
static const struct memory_api *g_mem;
#else
static const struct log_api    *g_log = &native_log;
static const struct memory_api *g_mem = &native_mem;
#endif

static struct asset_entry *find_entry(const char *path)
{
	int32_t i;

	for (i = 0; i < cache_count; i++) {
		if (strncmp(cache[i].path, path, ASSET_PATH_MAX) == 0)
			return &cache[i];
	}
	return NULL;
}

static struct asset_entry *find_entry_by_id(uint32_t id)
{
	int32_t i;

	for (i = 0; i < cache_count; i++) {
		if (cache[i].id == id)
			return &cache[i];
	}
	return NULL;
}

static struct asset_entry *alloc_entry(const char *path)
{
	struct asset_entry *e;

	if (cache_count >= ASSET_CACHE_MAX)
		return NULL;
	e = &cache[cache_count++];
	strncpy(e->path, path, ASSET_PATH_MAX - 1);
	e->path[ASSET_PATH_MAX - 1] = '\0';
	e->data      = NULL;
	e->size      = 0;
	e->refs      = 0;
	e->state     = ASSET_PENDING;
	e->kind      = ASSET_KIND_NORMAL;
	e->read_only = 0;
	e->type      = ASSET_TYPE_UNKNOWN;
	e->id        = next_asset_id++;
	e->origin    = ASSET_ORIGIN_FETCHED;
	e->ndecl     = 0;
	return e;
}

static void evict_entry(struct asset_entry *e)
{
	if (e->data)
		g_mem->free(e->data);
	*e = cache[--cache_count];
}

/* ------------------------------------------------------------------ */
/* Built-in primitive asset library                                    */
/* ------------------------------------------------------------------ */

static const char *builtin_paths[] = {
	"builtin://cube",
	"builtin://sphere",
	"builtin://plane",
	"builtin://pyramid",
};

#define BUILTIN_COUNT \
	((int32_t)(sizeof(builtin_paths) / sizeof(builtin_paths[0])))

/*
 * Default proof-of-life shader: a GLSL ES 3.00 vertex/fragment pair seeded
 * as built-in shader assets.  These carry the same source the WebGL renderer
 * draws the demo triangle with; the renderer fetches them through the asset
 * vtable rather than embedding the strings, exercising the "shaders are
 * assets" delivery seam.  Dialect/stage metadata rides on the describe()
 * decl-fields below.
 */
static const char *TRIANGLE_VERT_SRC =
	"#version 300 es\n"
	"layout(location = 0) in vec3 a_pos;\n"
	"layout(location = 1) in vec3 a_color;\n"
	"layout(std140) uniform Globals { vec4 u_tint; };\n"
	"out vec3 v_color;\n"
	"void main() {\n"
	"	v_color = a_color * u_tint.rgb;\n"
	"	gl_Position = vec4(a_pos, 1.0);\n"
	"}\n";

static const char *TRIANGLE_FRAG_SRC =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec3 v_color;\n"
	"out vec4 frag_color;\n"
	"void main() {\n"
	"	frag_color = vec4(v_color, 1.0);\n"
	"}\n";

static int builtins_seeded;

/*
 * Seed one built-in shader asset from NUL-terminated GLSL source.  A heap
 * copy of the source (including its trailing NUL) becomes the asset's bytes,
 * so consumers can use get_data() directly as a C string and shutdown frees
 * it uniformly with fetched assets.  size counts the stored bytes (NUL
 * included).
 */
static void seed_shader(const char *path, const char *src)
{
	struct asset_entry *e;
	uint32_t            n;

	e = alloc_entry(path);
	if (!e)
		return;
	n = (uint32_t)strlen(src) + 1;
	e->data = g_mem->alloc(n);
	if (!e->data) {
		e->state = ASSET_ERROR;
		return;
	}
	memcpy(e->data, src, n);
	e->size      = n;
	e->state     = ASSET_LOADED;
	e->kind      = ASSET_KIND_PRIMITIVE;
	e->read_only = 1;
	e->type      = ASSET_TYPE_SHADER;
}

static void seed_builtins(void)
{
	int32_t            i;
	struct asset_entry *e;

	if (builtins_seeded)
		return;
	builtins_seeded = 1;

	for (i = 0; i < BUILTIN_COUNT; i++) {
		e = alloc_entry(builtin_paths[i]);
		if (!e)
			continue;
		e->state     = ASSET_LOADED;
		e->kind      = ASSET_KIND_PRIMITIVE;
		e->read_only = 1;
		e->type      = ASSET_TYPE_MESH;
	}

	seed_shader("builtin://shader/triangle.vert", TRIANGLE_VERT_SRC);
	seed_shader("builtin://shader/triangle.frag", TRIANGLE_FRAG_SRC);
}

#ifdef __EMSCRIPTEN__

static void on_fetch_success(emscripten_fetch_t *fetch)
{
	struct asset_entry *e = (struct asset_entry *)fetch->userData;

	e->data = g_mem->alloc((size_t)fetch->numBytes);
	if (e->data) {
		memcpy(e->data, fetch->data, (size_t)fetch->numBytes);
		e->size  = (uint32_t)fetch->numBytes;
		e->state = ASSET_LOADED;
		g_log->write(LOG_LEVEL_INFO, "asset: loaded %s (%u bytes)",
			     e->path, e->size);
	} else {
		e->state = ASSET_ERROR;
		g_log->write(LOG_LEVEL_INFO,
			     "asset: out of memory loading %s", e->path);
	}
	emscripten_fetch_close(fetch);
}

static void on_fetch_error(emscripten_fetch_t *fetch)
{
	struct asset_entry *e = (struct asset_entry *)fetch->userData;

	e->state = ASSET_ERROR;
	g_log->write(LOG_LEVEL_INFO, "asset: error loading %s (HTTP %d)",
		     e->path, fetch->status);
	emscripten_fetch_close(fetch);
}

static void start_fetch(struct asset_entry *e)
{
	emscripten_fetch_attr_t attr;

	emscripten_fetch_attr_init(&attr);
	strncpy(attr.requestMethod, "GET", sizeof(attr.requestMethod) - 1);
	attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
	attr.userData   = e;
	attr.onsuccess  = on_fetch_success;
	attr.onerror    = on_fetch_error;
	emscripten_fetch(&attr, e->path);
}

#else /* native: synchronous filesystem read */

static void start_fetch(struct asset_entry *e)
{
	FILE    *f;
	long     len;
	uint8_t *buf;
	size_t   n;

	f = fopen(e->path, "rb");
	if (!f) {
		e->state = ASSET_ERROR;
		return;
	}

	fseek(f, 0, SEEK_END);
	len = ftell(f);
	rewind(f);

	if (len < 0) {
		fclose(f);
		e->state = ASSET_ERROR;
		return;
	}

	buf = g_mem->alloc((size_t)len);
	if (!buf) {
		fclose(f);
		e->state = ASSET_ERROR;
		return;
	}

	n = fread(buf, 1, (size_t)len, f);
	fclose(f);

	if (n != (size_t)len) {
		g_mem->free(buf);
		e->state = ASSET_ERROR;
		return;
	}

	e->data  = buf;
	e->size  = (uint32_t)len;
	e->state = ASSET_LOADED;
}

#endif /* __EMSCRIPTEN__ */

void asset_request(const char *path)
{
	struct asset_entry *e = find_entry(path);

	if (e) {
		e->refs++;
		return;
	}
	e = alloc_entry(path);
	if (!e) {
		g_log->write(LOG_LEVEL_INFO, "asset: cache full, dropping %s",
			     path);
		return;
	}
	e->refs = 1;
	start_fetch(e);
}

asset_state asset_state_of(const char *path)
{
	struct asset_entry *e = find_entry(path);

	return e ? (asset_state)e->state : ASSET_ERROR;
}

const void *asset_get(const char *path, uint32_t *out_size)
{
	struct asset_entry *e = find_entry(path);

	if (!e || e->state != ASSET_LOADED)
		return NULL;
	if (out_size)
		*out_size = e->size;
	return e->data;
}

void asset_release(const char *path)
{
	struct asset_entry *e = find_entry(path);

	if (!e || e->refs <= 0)
		return;
	if (e->read_only)
		return;
	e->refs--;
	/* Don't evict while a fetch is in flight — callback still holds *e. */
	if (e->refs == 0 && e->state != ASSET_PENDING)
		evict_entry(e);
}

void asset_codec_register(const char *ext,
			  void *(*decode)(const void *bytes, uint32_t size))
{
	if (codec_count >= CODEC_TABLE_MAX)
		return;
	strncpy(codec_table[codec_count].ext, ext,
		sizeof(codec_table[0].ext) - 1);
	codec_table[codec_count].ext[sizeof(codec_table[0].ext) - 1] = '\0';
	codec_table[codec_count].decode = decode;
	codec_count++;
}

void *asset_codec_get_typed(const char *path)
{
	const char *dot;
	const char *ext;
	const void *bytes;
	uint32_t    size;
	int32_t     i;

	dot = strrchr(path, '.');
	if (!dot)
		return NULL;
	ext = dot + 1;

	bytes = asset_get(path, &size);
	if (!bytes)
		return NULL;

	for (i = 0; i < codec_count; i++) {
		if (strncmp(codec_table[i].ext, ext,
			    sizeof(codec_table[0].ext) - 1) == 0)
			return codec_table[i].decode(bytes, size);
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Catalog enumeration API                                             */
/* ------------------------------------------------------------------ */

uint32_t asset_catalog_count(void)
{
	return (uint32_t)cache_count;
}

static void fill_info(struct asset_info *out, const struct asset_entry *e)
{
	out->path      = e->path;
	out->state     = e->state;
	out->size      = e->size;
	out->refs      = e->refs;
	out->kind      = e->kind;
	out->read_only = e->read_only;
	out->type      = e->type;
	out->id        = e->id;
	out->origin    = e->origin;
}

int32_t asset_catalog_info(uint32_t i, struct asset_info *out)
{
	if ((int32_t)i >= cache_count || !out)
		return -1;
	fill_info(out, &cache[i]);
	return 0;
}

int32_t asset_catalog_find(uint32_t id, struct asset_info *out)
{
	struct asset_entry *e;

	if (!out)
		return -1;
	e = find_entry_by_id(id);
	if (!e)
		return -1;
	fill_info(out, e);
	return 0;
}

const void *asset_catalog_get_data(uint32_t id, uint32_t *out_size)
{
	struct asset_entry *e;

	e = find_entry_by_id(id);
	if (!e || e->state != ASSET_LOADED)
		return NULL;
	if (out_size)
		*out_size = e->size;
	return e->data;
}

/* ------------------------------------------------------------------ */
/* Built-in declaration descriptors                                    */
/* ------------------------------------------------------------------ */

static const struct asset_decl_field cube_decl[] = {
	{ "topology",   "triangles"             },
	{ "vertices",   "24"                    },
	{ "indices",    "36"                    },
	{ "attributes", "position, normal, uv0" },
	{ "bounds.min", "{ -0.5, -0.5, -0.5 }" },
	{ "bounds.max", "{ 0.5, 0.5, 0.5 }"    },
};

static const struct asset_decl_field sphere_decl[] = {
	{ "topology",      "triangles"             },
	{ "segments",      "rings 16, sectors 32"  },
	{ "vertices",      "561"                   },
	{ "indices",       "2880"                  },
	{ "attributes",    "position, normal, uv0" },
	{ "bounds.radius", "0.5"                   },
};

static const struct asset_decl_field plane_decl[] = {
	{ "topology",   "triangles"             },
	{ "vertices",   "4"                     },
	{ "indices",    "6"                     },
	{ "attributes", "position, normal, uv0" },
	{ "normal",     "{ 0, 1, 0 }"           },
};

static const struct asset_decl_field pyramid_decl[] = {
	{ "topology",   "triangles"             },
	{ "vertices",   "16"                    },
	{ "indices",    "18"                    },
	{ "attributes", "position, normal, uv0" },
};

/*
 * Shader assets carry dialect + stage so the renderer can select the right
 * compiler without inspecting the source.  This is the forward-looking bit:
 * a second dialect (e.g. WGSL) slots in here without the renderer changing.
 */
static const struct asset_decl_field triangle_vert_decl[] = {
	{ "dialect", "glsl_es_300" },
	{ "stage",   "vertex"      },
};

static const struct asset_decl_field triangle_frag_decl[] = {
	{ "dialect", "glsl_es_300" },
	{ "stage",   "fragment"    },
};

struct builtin_desc {
	const char                   *path;
	const struct asset_decl_field *fields;
	uint32_t                       count;
};

#define ARRAY_SIZE(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

static const struct builtin_desc builtin_descs[] = {
	{ "builtin://cube",    cube_decl,    ARRAY_SIZE(cube_decl)    },
	{ "builtin://sphere",  sphere_decl,  ARRAY_SIZE(sphere_decl)  },
	{ "builtin://plane",   plane_decl,   ARRAY_SIZE(plane_decl)   },
	{ "builtin://pyramid", pyramid_decl, ARRAY_SIZE(pyramid_decl) },
	{ "builtin://shader/triangle.vert", triangle_vert_decl,
	  ARRAY_SIZE(triangle_vert_decl) },
	{ "builtin://shader/triangle.frag", triangle_frag_decl,
	  ARRAY_SIZE(triangle_frag_decl) },
};

#define BUILTIN_DESC_COUNT ARRAY_SIZE(builtin_descs)

/*
 * Synthesize a shader's declaration from its path when none was set
 * explicitly (e.g. after a reload from persistence, which restores bytes and
 * type but not in-memory decl).  Stage derives from the file extension;
 * dialect defaults to the only one we speak.  All strings are static
 * literals, valid for the process lifetime.
 */
static uint32_t shader_decl_from_path(const char *path,
				      struct asset_decl_field *out,
				      uint32_t max)
{
	const char *dot;
	uint32_t    n = 0;

	out[n].key   = "dialect";
	out[n].value = "glsl_es_300";
	n++;
	if (n >= max)
		return n;

	dot = strrchr(path, '.');
	out[n].key   = "stage";
	out[n].value = (dot && strcmp(dot, ".vert") == 0) ? "vertex"
							  : "fragment";
	n++;
	return n;
}

uint32_t asset_catalog_describe(uint32_t i,
				struct asset_decl_field *out,
				uint32_t max)
{
	const struct asset_entry *e;
	uint32_t    d;
	uint32_t    n;

	if ((int32_t)i >= cache_count || !out || max == 0)
		return 0;

	e = &cache[i];

	/* Authored assets carry their own (editor-set) declaration. */
	if (e->ndecl > 0) {
		n = (e->ndecl > max) ? max : e->ndecl;
		for (d = 0; d < n; d++) {
			out[d].key   = e->decl[d].key;
			out[d].value = e->decl[d].val;
		}
		return n;
	}

	/* Built-in primitives carry static descriptors keyed by path. */
	for (d = 0; d < BUILTIN_DESC_COUNT; d++) {
		if (strncmp(builtin_descs[d].path, e->path,
			    ASSET_PATH_MAX) != 0)
			continue;
		n = builtin_descs[d].count;
		if (n > max)
			n = max;
		memcpy(out, builtin_descs[d].fields,
		       n * sizeof(struct asset_decl_field));
		return n;
	}

	/* A shader with no explicit decl still reports stage + dialect. */
	if (e->type == ASSET_TYPE_SHADER)
		return shader_decl_from_path(e->path, out, max);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Mutation API — authored project assets                              */
/* ------------------------------------------------------------------ */

uint32_t asset_mut_create(const char *path, int32_t type,
			  const void *bytes, uint32_t size)
{
	struct asset_entry *e;
	uint8_t            *buf;

	if (!path || (!bytes && size > 0))
		return 0;
	if (find_entry(path))
		return 0;
	e = alloc_entry(path);
	if (!e)
		return 0;

	if (size > 0) {
		buf = g_mem->alloc((size_t)size);
		if (!buf) {
			/* Undo the alloc_entry; swap-remove ourselves. */
			cache_count--;
			return 0;
		}
		memcpy(buf, bytes, (size_t)size);
		e->data = buf;
	}

	e->size      = size;
	e->state     = ASSET_LOADED;
	e->kind      = ASSET_KIND_NORMAL;
	e->read_only = 0;
	e->origin    = ASSET_ORIGIN_AUTHORED;
	e->type      = type;
	e->refs      = 1;
	return e->id;
}

int32_t asset_mut_inject(uint32_t id, const char *path, int32_t type,
			 const void *bytes, uint32_t size)
{
	struct asset_entry *e;
	uint8_t            *buf;

	if (id == 0 || !path || (!bytes && size > 0))
		return -1;
	if (find_entry(path) || find_entry_by_id(id))
		return -1;
	e = alloc_entry(path);
	if (!e)
		return -1;

	if (size > 0) {
		buf = g_mem->alloc((size_t)size);
		if (!buf) {
			cache_count--;   /* undo the alloc_entry */
			return -1;
		}
		memcpy(buf, bytes, (size_t)size);
		e->data = buf;
	}

	e->id        = id;   /* restore the persisted identity, not a fresh one */
	e->size      = size;
	e->state     = ASSET_LOADED;
	e->kind      = ASSET_KIND_NORMAL;
	e->read_only = 0;
	e->origin    = ASSET_ORIGIN_AUTHORED;
	e->type      = type;
	e->refs      = 1;

	/* Keep the allocator monotonic and collision-free with the restored id. */
	if (id >= next_asset_id)
		next_asset_id = id + 1u;
	return 0;
}

int32_t asset_mut_set_data(uint32_t id, const void *bytes, uint32_t size)
{
	struct asset_entry *e;
	uint8_t            *buf;

	e = find_entry_by_id(id);
	if (!e || e->origin != ASSET_ORIGIN_AUTHORED)
		return -1;
	if (size > 0 && !bytes)
		return -1;

	if (size > 0) {
		buf = g_mem->alloc((size_t)size);
		if (!buf)
			return -1;
		memcpy(buf, bytes, (size_t)size);
	} else {
		buf = NULL;
	}

	if (e->data)
		g_mem->free(e->data);
	e->data  = buf;
	e->size  = size;
	e->state = ASSET_LOADED;
	return 0;
}

int32_t asset_mut_destroy(uint32_t id)
{
	struct asset_entry *e;

	e = find_entry_by_id(id);
	if (!e || e->origin != ASSET_ORIGIN_AUTHORED)
		return -1;
	evict_entry(e);
	return 0;
}

int32_t asset_mut_set_decl(uint32_t id, const struct asset_decl_field *fields,
			   uint32_t n)
{
	struct asset_entry *e;
	uint32_t            k;

	e = find_entry_by_id(id);
	if (!e || e->origin != ASSET_ORIGIN_AUTHORED)
		return -1;
	if (n > ASSET_DECL_MAX)
		return -1;
	if (n > 0 && !fields)
		return -1;

	/* Validate up front so a bad field leaves the existing decl intact. */
	for (k = 0; k < n; k++) {
		if (!fields[k].key || !fields[k].value)
			return -1;
	}

	for (k = 0; k < n; k++) {
		strncpy(e->decl[k].key, fields[k].key, ASSET_DECL_KEY_MAX - 1);
		e->decl[k].key[ASSET_DECL_KEY_MAX - 1] = '\0';
		strncpy(e->decl[k].val, fields[k].value, ASSET_DECL_VAL_MAX - 1);
		e->decl[k].val[ASSET_DECL_VAL_MAX - 1] = '\0';
	}
	e->ndecl = n;
	return 0;
}

static const struct asset_api catalog_api = {
	.count    = asset_catalog_count,
	.info     = asset_catalog_info,
	.describe = asset_catalog_describe,
	.find     = asset_catalog_find,
	.get_data = asset_catalog_get_data,
};

static const struct asset_codec_api codec_api = {
	.register_codec = asset_codec_register,
	.get_typed      = asset_codec_get_typed,
};

static const struct asset_mut_api mut_api = {
	.create   = asset_mut_create,
	.set_data = asset_mut_set_data,
	.destroy  = asset_mut_destroy,
	.set_decl = asset_mut_set_decl,
	.inject   = asset_mut_inject,
};

static const struct subsystem codec_desc = {
	.name = "asset_codec",
	.api  = &codec_api,
};

static const struct subsystem mut_desc = {
	.name = "asset_mut",
	.api  = &mut_api,
};

void asset_init(void)
{
	seed_builtins();
	g_log->write(LOG_LEVEL_INFO, "asset: init");
}

static void asset_tick(void)
{
	/* WASM fetch callbacks update state directly; nothing to drain. */
}

static void asset_shutdown(void)
{
	int32_t i;

	for (i = 0; i < cache_count; i++) {
		if (cache[i].data)
			g_mem->free(cache[i].data);
	}
	cache_count = 0;
	g_log->write(LOG_LEVEL_INFO, "asset: shutdown");
}

static const struct subsystem desc = {
	.name     = "asset",
	.api      = &catalog_api,
	.init     = asset_init,
	.tick     = asset_tick,
	.shutdown = asset_shutdown,
};

#ifdef __EMSCRIPTEN__
void plugin_entry(struct subsystem_manager *mgr)
#else
void asset_plugin_entry(struct subsystem_manager *mgr)
#endif
{
#ifdef __EMSCRIPTEN__
	g_log = subsystem_manager_get_api(mgr, "log");
	g_mem = subsystem_manager_get_api(mgr, "memory");
#endif
	subsystem_manager_register(mgr, &desc);
	subsystem_manager_register(mgr, &codec_desc);
	subsystem_manager_register(mgr, &mut_desc);
}
