/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "asset.h"
#include "asset_api.h"
#include "asset_codec_api.h"
#include "builtin_scripts.h"
#include "builtin_mesh_scripts.h"
#include "asset_edit.h"
#include "edit_api.h"
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
	void *(*encode)(const void *typed, uint32_t *out_size);
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

/*
 * The "edit" undo/redo service, resolved lazily. The asset plugin loads before
 * edit_plugin, so "edit" is not registered yet at our plugin_entry — we stash
 * the manager and look it up on the first authored mutation instead. g_edit
 * stays NULL when the service is absent (e.g. native unit tests), in which case
 * the mutation still happens and simply isn't recorded (no hard dependency).
 */
static struct subsystem_manager *g_mgr;
static const struct edit_api    *g_edit;

static const struct edit_api *resolve_edit(void)
{
	if (!g_edit && g_mgr)
		g_edit = subsystem_manager_get_api(g_mgr, "edit");
	return g_edit;
}

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
/* Built-in mesh script asset library                                  */
/* ------------------------------------------------------------------ */

/*
 * Built-in shaders, authored in the krudd shader DSL (a Scheme S-expression
 * carrying both stages and a shared IO model).  This source — not GLSL — is
 * what the asset stores, so the editor shows the DSL when you click a built-in
 * shader and the same bytes feed native and WebGPU backends later; the WebGL
 * renderer lowers the DSL to GLSL ES 3.00 at bind time (see script.h's
 * shader_transpile).  describe()'s decl-fields below advertise the source
 * format and which stages are present.
 *
 * Scene shader: the entity-driven pipeline the scene renderer (#172) binds.
 * Consumes the mesh_vertex layout (position/normal/uv0), a std140 Camera
 * block carrying view_proj and per-draw model, and a std140 Material block
 * carrying a single per-draw base_color tint (#materials v0 — the smallest
 * possible material parameter, with no fixed-function state or textures
 * yet). Each block binds at the GL index matching its declaration order
 * among the blocks a stage actually uses (see scene_renderer.c), so Camera
 * (vertex-only) lands at slot 0 and Material (fragment-only) at slot 1. The
 * fragment stage shades from the world normal (readable 3D, no lighting)
 * then multiplies in the material tint.
 */
static const char *SCENE_SHADER_SRC =
	"(shader scene\n"
	"  (inputs\n"
	"    (a_pos    vec3 (location 0))\n"
	"    (a_normal vec3 (location 1))\n"
	"    (a_uv0    vec2 (location 2)))\n"
	"  (uniforms\n"
	"    (Camera (block 0) (layout std140)\n"
	"      (view_proj mat4)\n"
	"      (model     mat4))\n"
	"    (Material (block 1) (layout std140)\n"
	"      (base_color vec4 (edit color))))\n"
	"  (varyings\n"
	"    (v_normal vec3))\n"
	"  (targets\n"
	"    (frag_color vec4 (location 0)))\n"
	"  (vertex\n"
	"    (set v_normal (* (mat3 model) a_normal))\n"
	"    (set position (* view_proj model (vec4 a_pos 1.0))))\n"
	"  (fragment\n"
	"    (let* ((n    (normalize v_normal))\n"
	"           (base (+ 0.5 (* 0.5 n)))\n"
	"           (diff (max (dot n (normalize (vec3 0.5 0.8 0.4))) 0.0))\n"
	"           (col  (* base (+ 0.35 (* 0.65 diff)))))\n"
	"      (set frag_color (vec4 (* col (swizzle base_color rgb)) 1.0)))))\n";

/*
 * Built-in default material — opaque white, the multiplicative identity the
 * scene shader's base_color expects.  This is the material the world scene binds
 * to every entity by default, so "no material assigned" is never the resting
 * state — an entity always points at a real, inspectable material.  It is stored
 * in the same v3 wire form an authored material uses: a leading shader-ref (the
 * scene shader — a material always names its shader) followed by the shader's
 * std140 Material block (here one vec4 base_color).  The renderer and editor read
 * it with no special case.
 */
static const float DEFAULT_MATERIAL_COLOR[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

static int builtins_seeded;

/*
 * Seed one built-in shader asset from NUL-terminated DSL source.  A heap
 * copy of the source (including its trailing NUL) becomes the asset's bytes,
 * so consumers can use get_data() directly as a C string and shutdown frees
 * it uniformly with fetched assets.  size counts the stored bytes (NUL
 * included).
 */
static uint32_t seed_shader(const char *path, const char *src)
{
	struct asset_entry *e;
	uint32_t            n;

	e = alloc_entry(path);
	if (!e)
		return 0;
	n = (uint32_t)strlen(src) + 1;
	e->data = g_mem->alloc(n);
	if (!e->data) {
		e->state = ASSET_ERROR;
		return 0;
	}
	memcpy(e->data, src, n);
	e->size      = n;
	e->state     = ASSET_LOADED;
	e->kind      = ASSET_KIND_PRIMITIVE;
	e->read_only = 1;
	e->type      = ASSET_TYPE_SHADER;
	return e->id;
}

/*
 * Seed one built-in material asset in the v3 wire form: a leading uint32
 * shader-ref (the asset id of the shader that owns its parameters) followed by
 * the shader's std140 Material block — here the single vec4 base_color.  The
 * renderer uploads those trailing bytes to the Material UBO as-is and the editor
 * derives its widgets from the referenced shader, so a material carries no schema
 * of its own; shutdown frees the bytes like any other asset.
 */
static void seed_material(const char *path, uint32_t shader_ref,
			  const float rgba[4])
{
	struct asset_entry *e;
	uint32_t            n;

	e = alloc_entry(path);
	if (!e)
		return;
	n = (uint32_t)(sizeof(uint32_t) + 4 * sizeof(float));
	e->data = g_mem->alloc(n);
	if (!e->data) {
		e->state = ASSET_ERROR;
		return;
	}
	memcpy(e->data, &shader_ref, sizeof(shader_ref));
	memcpy((unsigned char *)e->data + sizeof(shader_ref), rgba,
	       4 * sizeof(float));
	e->size      = n;
	e->state     = ASSET_LOADED;
	e->kind      = ASSET_KIND_PRIMITIVE;
	e->read_only = 1;
	e->type      = ASSET_TYPE_MATERIAL;
}

/*
 * Seed one built-in entity script from NUL-terminated Scheme source, the same
 * shape as seed_shader: the (script NAME ...) text becomes the asset's bytes so
 * the entity-script driver can read it back as a C string via get_data().  The
 * asset is what an entity's script_ref points at; ASSET_TYPE_SCRIPT tags it for
 * the script-only picker.
 */
static void seed_script(const char *path, const char *src)
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
	e->type      = ASSET_TYPE_SCRIPT;
}

/*
 * Seed one built-in mesh from NUL-terminated Scheme source, the same shape as
 * seed_script: the (mesh NAME (generate () ...)) text becomes the asset's
 * bytes, so a consumer resolves it to a real mesh_blob on demand via
 * mesh_script_generate() (asset/mesh_script.c) rather than at seed time —
 * mirroring how a shader asset stores DSL source, not compiled GLSL. There is
 * no separate "mesh script" type: every ASSET_TYPE_MESH asset is one of
 * these, full stop.
 */
static void seed_mesh(const char *path, const char *src)
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
	e->type      = ASSET_TYPE_MESH;
}

static void seed_builtins(void)
{
	if (builtins_seeded)
		return;
	builtins_seeded = 1;

	/*
	 * Every built-in mesh, the four classic primitives included, is
	 * authored the same way — there is no hardcoded C mesh generator.
	 * seed_mesh stores each (mesh NAME (generate () ...)) source
	 * verbatim; a consumer resolves it to a real mesh_blob on demand via
	 * mesh_script_generate().
	 */
	seed_mesh("builtin://mesh/cube",    CUBE_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/sphere",  SPHERE_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/plane",   PLANE_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/pyramid", PYRAMID_MESH_SCRIPT_SRC);
	seed_mesh("builtin://mesh/grid",    GRID_MESH_SCRIPT_SRC);

	{
		uint32_t scene_shader =
			seed_shader("builtin://shader/scene", SCENE_SHADER_SRC);

		seed_material("builtin://material/default", scene_shader,
			      DEFAULT_MATERIAL_COLOR);
	}

	seed_script("builtin://script/spinner", SPINNER_SCRIPT_SRC);
	seed_script("builtin://script/bounce",  BOUNCE_SCRIPT_SRC);
	seed_script("builtin://script/wobble",  WOBBLE_SCRIPT_SRC);
	seed_script("builtin://script/pulse",   PULSE_SCRIPT_SRC);
	seed_script("builtin://script/orbit-camera", ORBIT_CAMERA_SCRIPT_SRC);
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

/* Find the codec slot for ext, or NULL. */
static struct codec_entry *find_codec(const char *ext)
{
	int32_t i;

	for (i = 0; i < codec_count; i++) {
		if (strncmp(codec_table[i].ext, ext,
			    sizeof(codec_table[0].ext) - 1) == 0)
			return &codec_table[i];
	}
	return NULL;
}

/* Find the codec slot for ext, appending a fresh one if absent (or NULL if
 * the table is full).  Both register_codec and register_encoder attach to the
 * same slot, so one codec can hold both directions. */
static struct codec_entry *find_or_add_codec(const char *ext)
{
	struct codec_entry *e = find_codec(ext);

	if (e)
		return e;
	if (codec_count >= CODEC_TABLE_MAX)
		return NULL;
	e = &codec_table[codec_count++];
	strncpy(e->ext, ext, sizeof(e->ext) - 1);
	e->ext[sizeof(e->ext) - 1] = '\0';
	e->decode = NULL;
	e->encode = NULL;
	return e;
}

void asset_codec_register(const char *ext,
			  void *(*decode)(const void *bytes, uint32_t size))
{
	struct codec_entry *e = find_or_add_codec(ext);

	if (e)
		e->decode = decode;
}

void asset_codec_register_encoder(const char *ext,
				  void *(*encode)(const void *typed,
						  uint32_t *out_size))
{
	struct codec_entry *e = find_or_add_codec(ext);

	if (e)
		e->encode = encode;
}

void *asset_codec_decode_bytes(const char *ext, const void *bytes,
			       uint32_t size)
{
	struct codec_entry *e = find_codec(ext);

	if (!e || !e->decode)
		return NULL;
	return e->decode(bytes, size);
}

void *asset_codec_encode(const char *ext, const void *typed,
			 uint32_t *out_size)
{
	struct codec_entry *e = find_codec(ext);

	if (!e || !e->encode)
		return NULL;
	return e->encode(typed, out_size);
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
	{ "format",     "krudd-mesh"            },
	{ "topology",   "triangles"             },
	{ "vertices",   "24"                    },
	{ "indices",    "36"                    },
	{ "attributes", "position, normal, uv0" },
	{ "bounds.min", "{ -0.5, -0.5, -0.5 }" },
	{ "bounds.max", "{ 0.5, 0.5, 0.5 }"    },
};

static const struct asset_decl_field sphere_decl[] = {
	{ "format",        "krudd-mesh"            },
	{ "topology",      "triangles"             },
	{ "segments",      "rings 16, sectors 32"  },
	{ "vertices",      "561"                   },
	{ "indices",       "2880"                  },
	{ "attributes",    "position, normal, uv0" },
	{ "bounds.radius", "0.5"                   },
};

static const struct asset_decl_field plane_decl[] = {
	{ "format",     "krudd-mesh"            },
	{ "topology",   "triangles"             },
	{ "vertices",   "4"                     },
	{ "indices",    "6"                     },
	{ "attributes", "position, normal, uv0" },
	{ "normal",     "{ 0, 1, 0 }"           },
};

static const struct asset_decl_field pyramid_decl[] = {
	{ "format",     "krudd-mesh"            },
	{ "topology",   "triangles"             },
	{ "vertices",   "16"                    },
	{ "indices",    "18"                    },
	{ "attributes", "position, normal, uv0" },
};

/*
 * A shader asset is one DSL source holding every stage, so it advertises its
 * source format and the stages it defines (not a single stage per asset).  The
 * renderer lowers the DSL to whatever its backend speaks; a WebGPU/WGSL backend
 * slots in without the asset — or this metadata — changing.
 */
static const struct asset_decl_field scene_shader_decl[] = {
	{ "format", "krudd-shader"     },
	{ "stages", "vertex, fragment" },
};

/*
 * The built-in default material advertises its single parameter — the opaque
 * white base_color the scene shader multiplies in — the way the shader
 * built-ins advertise their format/stages.
 */
static const struct asset_decl_field default_material_decl[] = {
	{ "format",     "krudd-material"          },
	{ "base_color", "{ 1, 1, 1, 1 }"         },
	{ "shader",     "builtin://shader/scene" },
};

/*
 * A script asset is one (script NAME ...) Scheme form.  It advertises its
 * source format and the lifecycle hooks the built-in defines, the way a shader
 * advertises its stages.
 */
static const struct asset_decl_field spinner_script_decl[] = {
	{ "format", "krudd-script"      },
	{ "hooks",  "on-begin, on-tick" },
	{ "params", "speed"             },
};

static const struct asset_decl_field bounce_script_decl[] = {
	{ "format", "krudd-script" },
	{ "hooks",  "on-tick"      },
	{ "params", "height, rate" },
};

static const struct asset_decl_field wobble_script_decl[] = {
	{ "format", "krudd-script" },
	{ "hooks",  "on-tick"      },
	{ "params", "amp, rate"    },
};

/* pulse also advertises its authored parameters, the way the material decl
 * advertises base_color — a script's params are first-class, like a shader's. */
static const struct asset_decl_field pulse_script_decl[] = {
	{ "format", "krudd-script"    },
	{ "hooks",  "on-tick"         },
	{ "params", "amp, rate"       },
};

static const struct asset_decl_field orbit_camera_script_decl[] = {
	{ "format", "krudd-script"          },
	{ "hooks",  "on-tick"               },
	{ "params", "radius, height, speed" },
};

/*
 * A mesh asset is one (mesh NAME (generate () ...)) Scheme form. It
 * advertises its source format, the way a shader/script advertises theirs;
 * unlike a script there is no hook set to enumerate — a mesh always carries
 * exactly one generate clause.
 */
static const struct asset_decl_field grid_mesh_decl[] = {
	{ "format",   "krudd-mesh" },
	{ "topology", "triangles"  },
	{ "vertices", "25"         },
	{ "indices",  "96"         },
};

struct builtin_desc {
	const char                   *path;
	const struct asset_decl_field *fields;
	uint32_t                       count;
};

#define ARRAY_SIZE(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

static const struct builtin_desc builtin_descs[] = {
	{ "builtin://mesh/cube",    cube_decl,    ARRAY_SIZE(cube_decl)    },
	{ "builtin://mesh/sphere",  sphere_decl,  ARRAY_SIZE(sphere_decl)  },
	{ "builtin://mesh/plane",   plane_decl,   ARRAY_SIZE(plane_decl)   },
	{ "builtin://mesh/pyramid", pyramid_decl, ARRAY_SIZE(pyramid_decl) },
	{ "builtin://shader/scene", scene_shader_decl,
	  ARRAY_SIZE(scene_shader_decl) },
	{ "builtin://material/default", default_material_decl,
	  ARRAY_SIZE(default_material_decl) },
	{ "builtin://script/spinner", spinner_script_decl,
	  ARRAY_SIZE(spinner_script_decl) },
	{ "builtin://script/bounce", bounce_script_decl,
	  ARRAY_SIZE(bounce_script_decl) },
	{ "builtin://script/wobble", wobble_script_decl,
	  ARRAY_SIZE(wobble_script_decl) },
	{ "builtin://script/pulse", pulse_script_decl,
	  ARRAY_SIZE(pulse_script_decl) },
	{ "builtin://script/orbit-camera", orbit_camera_script_decl,
	  ARRAY_SIZE(orbit_camera_script_decl) },
	{ "builtin://mesh/grid", grid_mesh_decl,
	  ARRAY_SIZE(grid_mesh_decl) },
};

#define BUILTIN_DESC_COUNT ARRAY_SIZE(builtin_descs)

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

/* ------------------------------------------------------------------ */
/* Undo/redo recording — thin wrappers over the raw mutation ops        */
/* ------------------------------------------------------------------ */

/*
 * The "asset_mut" vtable points at these wrappers, not the raw ops, so every
 * caller of the mutable API is recorded. Each captures the asset's pre-edit
 * state, performs the raw mutation, then hands the before-state to
 * asset_edit_record(), which captures the after-state and pushes one reversible
 * command. Only a mutation that actually took effect is recorded. The raw ops
 * stay recording-free so the command's own apply/revert (which call them) never
 * re-enter the history — the same separation the scene adapter keeps between its
 * wrappers and the world ops.
 */
static uint32_t rec_create(const char *path, int32_t type,
			   const void *bytes, uint32_t size)
{
	const struct edit_api *edit   = resolve_edit();
	struct asset_snapshot *before = edit ? asset_snapshot_absent(g_mem)
					     : NULL;
	uint32_t               id;

	id = asset_mut_create(path, type, bytes, size);
	if (id != 0)
		asset_edit_record(edit, g_mem, id, before, "Create Asset", 0);
	else
		asset_snapshot_free(before, g_mem);
	return id;
}

static int32_t rec_set_data(uint32_t id, const void *bytes, uint32_t size)
{
	const struct edit_api *edit   = resolve_edit();
	struct asset_snapshot *before = edit ? asset_snapshot_capture(id, g_mem)
					     : NULL;
	int32_t                rc;

	rc = asset_mut_set_data(id, bytes, size);
	if (rc == 0)
		asset_edit_record(edit, g_mem, id, before, "Edit Asset",
				  asset_edit_key(id));
	else
		asset_snapshot_free(before, g_mem);
	return rc;
}

static int32_t rec_destroy(uint32_t id)
{
	const struct edit_api *edit   = resolve_edit();
	struct asset_snapshot *before = edit ? asset_snapshot_capture(id, g_mem)
					     : NULL;
	int32_t                rc;

	rc = asset_mut_destroy(id);
	if (rc == 0)
		asset_edit_record(edit, g_mem, id, before, "Delete Asset", 0);
	else
		asset_snapshot_free(before, g_mem);
	return rc;
}

static const struct asset_api catalog_api = {
	.count    = asset_catalog_count,
	.info     = asset_catalog_info,
	.describe = asset_catalog_describe,
	.find     = asset_catalog_find,
	.get_data = asset_catalog_get_data,
};

static const struct asset_codec_api codec_api = {
	.register_codec   = asset_codec_register,
	.get_typed        = asset_codec_get_typed,
	.register_encoder = asset_codec_register_encoder,
	.decode_bytes     = asset_codec_decode_bytes,
	.encode           = asset_codec_encode,
};

/*
 * create / set_data / destroy route through the recording wrappers so authored
 * edits land on the undo timeline. set_decl (metadata) and inject (id-preserving
 * rehydration from persistence, before the user authors anything) are not user
 * gestures and stay on the raw ops — inject in particular is what undo itself
 * calls to bring a destroyed asset back, so recording it would be circular.
 */
static const struct asset_mut_api mut_api = {
	.create   = rec_create,
	.set_data = rec_set_data,
	.destroy  = rec_destroy,
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

void asset_plugin_entry(struct subsystem_manager *mgr)
{
#ifdef __EMSCRIPTEN__
	g_log = subsystem_manager_get_api(mgr, "log");
	g_mem = subsystem_manager_get_api(mgr, "memory");
#endif
	/* Stash for the lazy "edit" lookup: it registers after us. */
	g_mgr = mgr;
	subsystem_manager_register(mgr, &desc);
	subsystem_manager_register(mgr, &codec_desc);
	subsystem_manager_register(mgr, &mut_desc);
}
