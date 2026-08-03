/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * scene_script — host side of the scene-building layer.
 *
 * Registers the scene-* primitives the (scene ...) form calls, and drives one
 * build by handing a scene's source text to the image's scene-build. The
 * primitives spawn and bind entities in the world bound for the span of one
 * image call (saved and restored around the s7_call), mirroring the g_w
 * discipline entity_script.c uses for its per-tick primitives. Three calls
 * bind that way and no other code path does: a build (scene_script_build), an
 * event dispatch (scene_script_call), and the frame hook (scene_script_tick).
 * Save and restore rather than set and clear, because a primitive may build a
 * scene from inside a call that is already bound.
 *
 * Also here: script-define!, mesh-define! and material-define!, the authoring
 * twins of scene-script!, scene-mesh! and scene-material!. A scene binds a
 * script, a mesh or a material by catalog path; these are how a project puts one
 * at a path in the first place, so it need not depend on the engine having
 * seeded it.
 */
#include <entity/scene_script.h>

#include <entity/world.h>
#include <abi/asset_api.h>
#include <core/script.h>

#include "s7.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SCENE_SCRIPT_PI 3.14159265358979323846

/*
 * The world and catalog a build acts on, valid only for the span of one
 * scene_script_build. A primitive can only run synchronously inside that call,
 * so no primitive ever observes a stale pointer. The binding nests:
 * scene-build! runs a whole build from inside an image call that is already
 * bound, so scene_call_bound treats these as a stack, not as a flag.
 */
static struct world           *g_w;
static const struct asset_api *g_asset;

/*
 * The catalog the *-define! primitives read and write, bound once for the
 * session rather than per call. Authoring an asset is not part of building a
 * world: a project registers the assets it needs while its own source is being
 * evaluated, which is before — and outside — any scene_script_build. Left NULL
 * on a host with no asset subsystem, where the primitives are simply inert.
 */
static const struct asset_api     *g_catalog;
static const struct asset_mut_api *g_catalog_mut;

/* Longest params/hooks list a decl field carries; longer is truncated. */
#define SCENE_SCRIPT_DECL_MAX 64

/* Most params one defined asset reports, matching entity_script.c. */
#define SCENE_SCRIPT_MAX_PARAMS 32

/* Longest catalog path material-define! reads out of a (shader ...) clause. */
#define SCENE_SCRIPT_PATH_MAX 128

/*
 * The leading uint32 shader-ref every material's wire form starts with, before
 * the shader's std140 Material block — MATERIAL_HEADER_BYTES as the renderer
 * spells it (render/scene_renderer/scene_renderer.c).
 */
#define SCENE_SCRIPT_MATERIAL_HEADER 4u

/*
 * Ceiling on a packed material: the header, a std140 Material block, and the
 * optional [tex-ref][width][height] trailer. A Material uniform block anywhere
 * near this size would not fit a real UBO either, so a source that overruns it
 * is refused rather than truncated — a short blob would be read as "no texture
 * slot" and shade with garbage where the block ran out.
 */
#define SCENE_SCRIPT_MATERIAL_MAX 512u

/* First list arg as an entity id, or -1 when it is not an integer. */
static int32_t arg_id(s7_pointer args)
{
	s7_pointer a = s7_car(args);

	return s7_is_integer(a) ? (int32_t)s7_integer(a) : -1;
}

/* The n-th list arg coerced to double, or 0.0 when it is not a number. */
static double arg_real(s7_scheme *sc, s7_pointer args, int32_t n)
{
	s7_pointer a = s7_list_ref(sc, args, n);

	return s7_is_number(a) ? s7_number_to_real(sc, a) : 0.0;
}

/* The n-th list arg as a C string, or NULL when it is not a string. */
static const char *arg_str(s7_scheme *sc, s7_pointer args, int32_t n)
{
	s7_pointer a = s7_list_ref(sc, args, n);

	return s7_is_string(a) ? s7_string(a) : NULL;
}

/* True when id names a live entity in the bound world. */
static int id_ok(int32_t id)
{
	return g_w && id >= 0 && (uint32_t)id < g_w->count && g_w->alive[id];
}

/*
 * Resolve a catalog PATH (e.g. "builtin://mesh/box") to its stable asset id, or
 * 0 when unknown. A linear catalog scan — a scene binds a handful of assets at
 * build time, not per frame, so the cost never matters.
 */
static uint32_t resolve_asset(const char *path)
{
	struct asset_info info;
	uint32_t          n, i;

	if (!g_asset || !path)
		return 0;
	n = g_asset->count();
	for (i = 0; i < n; i++) {
		if (g_asset->info(i, &info) == 0 && info.path
		    && strcmp(info.path, path) == 0)
			return info.id;
	}
	return 0;
}

/*
 * (scene-spawn [parent]) -> id: a new entity with an identity transform and an
 * empty component mask. PARENT is the entity id to nest under, or -1 / omitted
 * for a root entity. A child's transform clauses are read as local to its parent
 * (world_tick composes the two each frame), so a composite piece — an X built
 * from two crossed bars — moves and scales as one when its parent does.
 */
static s7_pointer sp_scene_spawn(s7_scheme *sc, s7_pointer args)
{
	struct transform t;
	int32_t          parent = WORLD_NO_PARENT;

	if (!g_w)
		return s7_make_integer(sc, -1);
	if (s7_is_pair(args) && s7_is_integer(s7_car(args)))
		parent = (int32_t)s7_integer(s7_car(args));
	memset(&t, 0, sizeof(t));
	t.rotation[3] = 1.0f;                       /* identity quaternion */
	t.scale[0] = t.scale[1] = t.scale[2] = 1.0f;
	return s7_make_integer(sc, world_create_entity(g_w, parent, &t, 0u));
}

/*
 * (scene-xform! id px py pz rx ry rz sx sy sz): set id's authored local
 * transform — position, intrinsic X-Y-Z Euler rotation in degrees, and scale.
 * The Euler-to-quaternion conversion mirrors entity-set-euler!, but writes the
 * authored pose (local) rather than a frame's animated pose (world_xform).
 */
static s7_pointer sp_scene_xform(s7_scheme *sc, s7_pointer args)
{
	int32_t id = arg_id(args);

	if (id_ok(id)) {
		struct transform t;
		double rx = arg_real(sc, args, 4) * (SCENE_SCRIPT_PI / 180.0) * 0.5;
		double ry = arg_real(sc, args, 5) * (SCENE_SCRIPT_PI / 180.0) * 0.5;
		double rz = arg_real(sc, args, 6) * (SCENE_SCRIPT_PI / 180.0) * 0.5;
		double cx = cos(rx), sx = sin(rx);
		double cy = cos(ry), sy = sin(ry);
		double cz = cos(rz), sz = sin(rz);

		t.position[0] = (float)arg_real(sc, args, 1);
		t.position[1] = (float)arg_real(sc, args, 2);
		t.position[2] = (float)arg_real(sc, args, 3);
		t.rotation[0] = (float)(sx * cy * cz - cx * sy * sz);
		t.rotation[1] = (float)(cx * sy * cz + sx * cy * sz);
		t.rotation[2] = (float)(cx * cy * sz - sx * sy * cz);
		t.rotation[3] = (float)(cx * cy * cz + sx * sy * sz);
		t.scale[0] = (float)arg_real(sc, args, 7);
		t.scale[1] = (float)arg_real(sc, args, 8);
		t.scale[2] = (float)arg_real(sc, args, 9);
		world_set_transform(g_w, id, &t);
	}
	return s7_unspecified(sc);
}

/* (scene-mesh! id "path"): bind id's mesh by catalog path (sets COMPONENT_RENDER). */
static s7_pointer sp_scene_mesh(s7_scheme *sc, s7_pointer args)
{
	int32_t id = arg_id(args);

	if (id_ok(id))
		world_set_render_ref(g_w, id, resolve_asset(arg_str(sc, args, 1)));
	return s7_unspecified(sc);
}

/* (scene-material! id "path"): bind id's material by catalog path. */
static s7_pointer sp_scene_material(s7_scheme *sc, s7_pointer args)
{
	int32_t id = arg_id(args);

	if (id_ok(id))
		world_set_material_ref(g_w, id,
				       resolve_asset(arg_str(sc, args, 1)));
	return s7_unspecified(sc);
}

/* (scene-script! id "path"): bind id's behavior script by catalog path. */
static s7_pointer sp_scene_script(s7_scheme *sc, s7_pointer args)
{
	int32_t id = arg_id(args);

	if (id_ok(id))
		world_set_script_ref(g_w, id,
				     resolve_asset(arg_str(sc, args, 1)));
	return s7_unspecified(sc);
}

/*
 * Look PATH up in the session catalog, filling *out on a hit; returns the
 * stable id, or 0 when there is no catalog or no such path. The *-define!
 * twin of resolve_asset — the same linear scan, but over the catalog bound for
 * the session rather than the one bound for a build.
 */
static uint32_t catalog_lookup(const char *path, struct asset_info *out)
{
	uint32_t n, i;

	if (!g_catalog || !path)
		return 0;
	n = g_catalog->count();
	for (i = 0; i < n; i++) {
		if (g_catalog->info(i, out) == 0 && out->path
		    && strcmp(out->path, path) == 0)
			return out->id;
	}
	return 0;
}

/* Append WORD to BUF (cap bytes), comma-separated from what is there. */
static void decl_join(char *buf, uint32_t cap, const char *word)
{
	uint32_t n = (uint32_t)strlen(buf);

	if (n > 0 && n + 2 < cap) {
		buf[n++] = ',';
		buf[n++] = ' ';
		buf[n]   = '\0';
	}
	strncpy(buf + n, word, cap - n - 1);
	buf[cap - 1] = '\0';
}

/* SRC's hook clause names, comma-separated: "on-begin, on-tick". */
static void script_hook_list(const char *src, char *buf, uint32_t cap)
{
	s7_scheme *sc = script_s7();
	s7_pointer fn, res;

	buf[0] = '\0';
	if (!sc)
		return;
	fn = s7_name_to_value(sc, "script-hooks");
	if (!s7_is_procedure(fn))
		return;
	res = s7_call(sc, fn, s7_list(sc, 1, s7_make_string(sc, src)));
	for (; s7_is_pair(res); res = s7_cdr(res)) {
		if (s7_is_string(s7_car(res)))
			decl_join(buf, cap, s7_string(s7_car(res)));
	}
}

/*
 * SRC's declared parameter names, comma-separated: "amp, rate". QUERY is the
 * introspector for SRC's dialect — script_entity_params for a (script ...),
 * script_mesh_params for a (mesh ...) — since a (params ...) clause reads the
 * same wherever it appears but is reached through a different image entry.
 */
static void param_list(const char *src, char *buf, uint32_t cap,
		       int (*query)(const char *, struct shader_param *,
				    uint32_t, uint32_t *))
{
	struct shader_param p[SCENE_SCRIPT_MAX_PARAMS];
	int                 n, i;

	buf[0] = '\0';
	n = query(src, p, SCENE_SCRIPT_MAX_PARAMS, NULL);
	for (i = 0; i < n; i++)
		decl_join(buf, cap, p[i].name);
}

/*
 * Publish ID's catalog declaration — the format/hooks/params strings an asset
 * inspector reads back through describe() — derived from SRC itself.
 *
 * A seeded built-in takes these from a hand-written table keyed by path
 * (world/asset/asset_plugin.c). A script a project defines has no entry there
 * and needs none: every field in such an entry only restates the source, so it
 * is read back out of the source here. The parameters an entity actually EDITS
 * never came from that table at all — script_entity_params parses the
 * (params ...) clause at bind time — so they work whether or not this runs.
 */
static void script_declare(uint32_t id, const char *src)
{
	struct asset_decl_field f[3];
	char                    hooks[SCENE_SCRIPT_DECL_MAX];
	char                    params[SCENE_SCRIPT_DECL_MAX];
	uint32_t                n = 0;

	f[n].key   = "format";
	f[n].value = "krudd-script";
	n++;
	script_hook_list(src, hooks, sizeof(hooks));
	if (hooks[0]) {
		f[n].key   = "hooks";
		f[n].value = hooks;
		n++;
	}
	param_list(src, params, sizeof(params), script_entity_params);
	if (params[0]) {
		f[n].key   = "params";
		f[n].value = params;
		n++;
	}
	if (g_catalog_mut->set_decl)
		g_catalog_mut->set_decl(id, f, n);
}

/*
 * Publish ID's catalog declaration for a mesh, the geometry twin of
 * script_declare and derived from SRC the same way.
 *
 * Only what the source actually says: format/topology/attributes are constants
 * of the mesh format itself — every mesh_blob is a triangle list of
 * position/normal/uv0 vertices — and the params come out of the (params ...)
 * clause. The seeded entries also carry prose (surface, segments) and a
 * vertex/index fingerprint; neither is derivable without running the generator,
 * which is a scene's work, not a registration's, so a defined mesh reports
 * neither rather than reporting them wrong.
 */
static void mesh_declare(uint32_t id, const char *src)
{
	struct asset_decl_field f[4];
	char                    params[SCENE_SCRIPT_DECL_MAX];
	uint32_t                n = 0;

	f[n].key   = "format";
	f[n].value = "krudd-mesh";
	n++;
	f[n].key   = "topology";
	f[n].value = "triangles";
	n++;
	f[n].key   = "attributes";
	f[n].value = "position, normal, uv0";
	n++;
	param_list(src, params, sizeof(params), script_mesh_params);
	if (params[0]) {
		f[n].key   = "params";
		f[n].value = params;
		n++;
	}
	if (g_catalog_mut->set_decl)
		g_catalog_mut->set_decl(id, f, n);
}

/*
 * Register SIZE bytes at PATH as an authored asset of TYPE, returning its stable
 * catalog id or 0 when it could not be registered. The shared body of every
 * *-define! primitive, whose docstrings state the contract.
 *
 * A path already holding an authored asset of TYPE has its bytes replaced in
 * place, keeping the id. A read-only built-in path is refused, and so is a path
 * holding some other type: a mesh path that quietly came to hold script bytes
 * would render nothing and explain nothing, and no project means to do it.
 */
static uint32_t asset_define_data(const char *path, const void *data,
				  uint32_t size, int32_t type)
{
	struct asset_info info;
	uint32_t          id;

	if (!path || !data || !g_catalog_mut || !g_catalog_mut->create
	    || !g_catalog_mut->set_data)
		return 0;
	id = catalog_lookup(path, &info);
	if (id && (info.read_only || info.type != type))
		return 0;
	if (id)
		return g_catalog_mut->set_data(id, data, size) == 0 ? id : 0;
	return g_catalog_mut->create(path, type, data, size);
}

/*
 * The source-text form of asset_define_data, for the asset types whose stored
 * bytes ARE their source: a script and a mesh are baked on demand from the text
 * the project wrote. (A material is not — see material_define.)
 */
static uint32_t asset_define(const char *path, const char *src, int32_t type)
{
	if (!src)
		return 0;
	/* Sources store the NUL: get_data() hands back a C string. */
	return asset_define_data(path, src, (uint32_t)strlen(src) + 1, type);
}

/*
 * (script-define! "path" "src") -> the script's stable catalog id, or 0 when it
 * could not be registered.
 *
 * Registers SRC — one (script NAME ...) form — as an authored
 * ASSET_TYPE_SCRIPT asset at PATH, so a project brings its own entity scripts
 * instead of depending on the engine having seeded them. The path is then
 * bindable by (scene-script! id path) exactly like a built-in, and the script's
 * (params ...) clause introspects the same way, out of the source.
 *
 * Redefinition REPLACES. A second call on the same path overwrites the bytes of
 * the entry already there and keeps its stable id, so an entity bound to the
 * script picks the new source up on its next tick with nothing to rebind. That
 * is what iterating on a project wants — re-evaluating the source is how a
 * project is reloaded, and making the second load an error would mean a project
 * could only ever be loaded once. Redefining a path the engine seeded read-only
 * is refused (0): those belong to the engine, and shadowing one would change
 * every scene that binds it, not just this project's. So is a path already
 * holding an asset of another type — see asset_define.
 */
static s7_pointer sp_script_define(s7_scheme *sc, s7_pointer args)
{
	const char *src = arg_str(sc, args, 1);
	uint32_t    id;

	id = asset_define(arg_str(sc, args, 0), src, ASSET_TYPE_SCRIPT);
	if (id)
		script_declare(id, src);
	return s7_make_integer(sc, id);
}

/*
 * (mesh-define! "path" "src") -> the mesh's stable catalog id, or 0 when it
 * could not be registered.
 *
 * Registers SRC — one (mesh NAME [(params ...)] (generate () ...)) form — as an
 * authored ASSET_TYPE_MESH asset at PATH, so a project brings its own geometry
 * instead of depending on the engine having seeded it. The path is then
 * bindable by (scene-mesh! id path) exactly like a built-in: the renderer and
 * the picker resolve the bytes through mesh_script_generate the same way, and
 * the mesh's (params ...) clause introspects out of the source, not out of any
 * table keyed by path.
 *
 * Redefinition REPLACES, on script-define!'s reasoning: a second call on the
 * same path overwrites the bytes and keeps the stable id, so an entity already
 * bound picks up the new geometry with nothing to rebind — re-evaluating a
 * project's source is how it reloads, and an error there would mean it could
 * only ever load once. Redefining a path the engine seeded read-only is
 * refused (0), since shadowing a built-in would change every scene that binds
 * it, not just this project's; so is a path holding an asset of another type.
 */
static s7_pointer sp_mesh_define(s7_scheme *sc, s7_pointer args)
{
	const char *src = arg_str(sc, args, 1);
	uint32_t    id;

	id = asset_define(arg_str(sc, args, 0), src, ASSET_TYPE_MESH);
	if (id)
		mesh_declare(id, src);
	return s7_make_integer(sc, id);
}

/*
 * Publish ID's catalog declaration for a material, the third of the trio.
 *
 * Shorter than its siblings', and deliberately: a material has no parameter set
 * of its own to advertise. It names a shader, and that shader's Material block
 * IS its schema — the same fact the editor's material inspector is built on — so
 * naming the shader says everything a list of field names would, and says it in
 * a form an inspector can follow to the types, ranges and edit hints too. The
 * seeded checker entry is shaped the same way (format / shader / texture); the
 * seeded pbr ones also spell a couple of values, which a derived declaration has
 * no business restating from bytes the catalog already holds.
 */
static void material_declare(uint32_t id, const char *shader_path,
			     const char *src)
{
	struct asset_decl_field d[3];
	char                    path[SCENE_SCRIPT_PATH_MAX];
	/* Sized so the longest path plus " @ NNNNNNNNNN" cannot truncate. */
	char                    tex[SCENE_SCRIPT_PATH_MAX + 16];
	uint32_t                c = 0, tw = 0, th = 0;

	d[c].key   = "format";
	d[c].value = "krudd-material";
	c++;
	d[c].key   = "shader";
	d[c].value = shader_path;
	c++;
	if (script_material_texture(src, path, sizeof(path), &tw, &th) == 0) {
		/* "path @ 256" for the square bake the seeded checker entry
		 * spells that way; "path @ 256x128" when it is not square. */
		if (tw == th)
			snprintf(tex, sizeof(tex), "%s @ %u", path, tw);
		else
			snprintf(tex, sizeof(tex), "%s @ %ux%u", path, tw, th);
		d[c].key   = "texture";
		d[c].value = tex;
		c++;
	}
	if (g_catalog_mut->set_decl)
		g_catalog_mut->set_decl(id, d, c);
}

/*
 * Write the resolved fields F (n of them) into BUF as the material wire form:
 * [shader-ref u32][std140 Material block], returning the byte count, or 0 when
 * the block does not fit BUF. TOTAL is the block size the shader reports.
 *
 * The zero fill is load-bearing and matches the C seeders exactly: std140 leaves
 * gaps between fields (the pbr block pads 24..31 between roughness and emissive)
 * and the seeders memset before writing, so a defined material and a seeded one
 * agree byte for byte down to the padding. Each field is written at the offset
 * the shader's own introspection reported — nothing here knows a layout.
 *
 * An `int` field is written as an int32, not as the float its value arrived in:
 * the two share a 4-byte lane, and a shader reading a float bit pattern as an
 * integer is precisely the silently-wrong blob this whole path exists to rule
 * out.
 */
static uint32_t material_pack(const struct shader_param *f, int n,
			      uint32_t total, uint32_t shader_ref,
			      unsigned char *buf, uint32_t cap)
{
	uint32_t size = SCENE_SCRIPT_MATERIAL_HEADER + total;
	int      i, c;

	if (size > cap)
		return 0;
	memset(buf, 0, size);
	memcpy(buf, &shader_ref, sizeof(shader_ref));
	for (i = 0; i < n; i++) {
		int is_int = strcmp(f[i].type, "int") == 0;

		for (c = 0; c < (int)f[i].default_count; c++) {
			uint32_t off = SCENE_SCRIPT_MATERIAL_HEADER
				       + f[i].offset + (uint32_t)c * 4u;

			if (off + 4u > size)
				return 0;
			if (is_int) {
				int32_t v = (int32_t)f[i].edit_default[c];

				memcpy(buf + off, &v, sizeof(v));
			} else {
				memcpy(buf + off, &f[i].edit_default[c],
				       sizeof(float));
			}
		}
	}
	return size;
}

/*
 * Append SRC's optional (texture "PATH" W H) slot to BUF — the trailer the
 * renderer reads after the Material block: [tex-ref u32][width u32][height u32].
 * Returns the new byte count, SIZE unchanged when the source declares no slot,
 * or 0 when it declares one that cannot be honoured (an unknown path, a path
 * that is not a texture, or no room). A named-but-unresolvable texture is a
 * refusal and not a silent drop: a material that meant to show a texture and
 * quietly does not is the same class of wrong as a mis-packed field.
 */
static uint32_t material_pack_texture(const char *src, unsigned char *buf,
				      uint32_t size, uint32_t cap)
{
	char              path[SCENE_SCRIPT_PATH_MAX];
	struct asset_info info;
	uint32_t          w = 0, h = 0, tex;

	if (script_material_texture(src, path, sizeof(path), &w, &h) != 0)
		return size;
	tex = catalog_lookup(path, &info);
	if (!tex || info.type != ASSET_TYPE_TEXTURE)
		return 0;
	if (size + 3u * sizeof(uint32_t) > cap)
		return 0;
	memcpy(buf + size,      &tex, sizeof(tex));
	memcpy(buf + size + 4u, &w,   sizeof(w));
	memcpy(buf + size + 8u, &h,   sizeof(h));
	return size + 3u * (uint32_t)sizeof(uint32_t);
}

/*
 * Register SRC — one (material ...) form — at PATH, returning its stable catalog
 * id or 0 when it was refused. The body of material-define!, whose docstring
 * states the contract.
 *
 * Unlike script-define! and mesh-define! this stores no source text. A script
 * and a mesh are baked from their bytes on demand, so the bytes are the source;
 * a material's bytes are uploaded to a uniform buffer verbatim, per draw, so the
 * bytes are the packed std140 block. Packing here rather than at draw time is
 * also what makes a bad source a registration failure — the one moment a project
 * can be told about it — instead of a per-frame surprise.
 */
static uint32_t material_define(const char *path, const char *src)
{
	/*
	 * One slot past the cap, so a block with MORE fields than can be
	 * marshalled is distinguishable from one with exactly the cap. The
	 * former is refused: the fields past the cap would pack as zeros, which
	 * is the silently wrong blob again.
	 */
	struct shader_param f[SCENE_SCRIPT_MAX_PARAMS + 1];
	unsigned char       buf[SCENE_SCRIPT_MATERIAL_MAX];
	char                shader_path[SCENE_SCRIPT_PATH_MAX];
	struct asset_info   info;
	const char         *shader_src;
	uint32_t            shader_ref, total = 0, size, id;
	int                 n;

	if (!path || !src || !g_catalog || !g_catalog->get_data)
		return 0;
	if (script_material_shader(src, shader_path, sizeof(shader_path)) != 0)
		return 0;
	shader_ref = catalog_lookup(shader_path, &info);
	if (!shader_ref || info.type != ASSET_TYPE_SHADER)
		return 0;
	shader_src = (const char *)g_catalog->get_data(shader_ref, NULL);
	if (!shader_src)
		return 0;
	n = script_material_fields(src, shader_src, f,
				   SCENE_SCRIPT_MAX_PARAMS + 1, &total);
	if (n < 0 || n > SCENE_SCRIPT_MAX_PARAMS)
		return 0;
	size = material_pack(f, n, total, shader_ref, buf, sizeof(buf));
	if (!size)
		return 0;
	size = material_pack_texture(src, buf, size, sizeof(buf));
	if (!size)
		return 0;
	id = asset_define_data(path, buf, size, ASSET_TYPE_MATERIAL);
	if (id)
		material_declare(id, shader_path, src);
	return id;
}

/*
 * (material-define! "path" "src") -> the material's stable catalog id, or 0 when
 * it could not be registered.
 *
 * Registers SRC — one (material NAME (shader "PATH") (FIELD V ...) ...) form —
 * as an authored ASSET_TYPE_MATERIAL asset at PATH, so a project brings its own
 * materials instead of depending on the engine having seeded them. The path is
 * then bindable by (scene-material! id path) exactly like a built-in: the stored
 * bytes are the same wire form the C seeders write, so nothing downstream of the
 * catalog can tell the two apart.
 *
 * The source names FIELDS, never a layout. Offsets, padding and block size come
 * from the Material block of the shader the source names — the same
 * introspection the renderer sizes its Material UBO with — so a project's source
 * cannot get std140 wrong. What it can get wrong is naming, and every one of
 * those is REFUSED (0), registering nothing: text that is not a (material ...)
 * form, a missing or unresolvable (shader ...) clause, a shader path holding
 * something that is not a shader, a field the block does not declare, a field
 * given the wrong number of components, and a (texture ...) clause naming
 * something that is not a texture. A field the source simply omits is not an
 * error — it takes the shader's own authored (default ...) — which is a declared
 * value rather than a silent zero.
 *
 * Redefinition REPLACES, on script-define!'s reasoning: a second call on the
 * same path repacks the bytes and keeps the stable id, so an entity already
 * bound picks up the new look with nothing to rebind — re-evaluating a project's
 * source is how it reloads, and an error there would mean it could only ever
 * load once. Redefining a path the engine seeded read-only is refused (0), since
 * shadowing a built-in would change every scene that binds it, not just this
 * project's; so is a path holding an asset of another type.
 */
static s7_pointer sp_material_define(s7_scheme *sc, s7_pointer args)
{
	return s7_make_integer(sc, material_define(arg_str(sc, args, 0),
						   arg_str(sc, args, 1)));
}

/* (scene-name! id "name"): set id's human-readable label. */
static s7_pointer sp_scene_name(s7_scheme *sc, s7_pointer args)
{
	int32_t id = arg_id(args);

	if (id_ok(id))
		world_set_name(g_w, id, arg_str(sc, args, 1));
	return s7_unspecified(sc);
}

/*
 * (scene-entity-name id) -> the entity's name, or "" when id has none or is not
 * a live entity. Lets image-side game rules read the tag of the entity under a
 * click (a "cell-N" pad) and decide what to do — the read twin of scene-name!.
 */
static s7_pointer sp_scene_entity_name(s7_scheme *sc, s7_pointer args)
{
	int32_t     id   = arg_id(args);
	const char *name = id_ok(id) ? world_entity_name(g_w, (uint32_t)id) : NULL;

	return s7_make_string(sc, name ? name : "");
}

/*
 * (scene-entity-pos id) -> the entity's authored local position as a three-item
 * list (x y z), or #f when id is not a live entity. The read twin of the position
 * scene-xform! writes: game rules that relocate a picked entity (a chess piece
 * moving to a captured square) need the target's current spot, which only the
 * host knows. Top-level entities have no parent, so local is world here — the
 * pieces a game moves are all roots.
 */
static s7_pointer sp_scene_entity_pos(s7_scheme *sc, s7_pointer args)
{
	int32_t id = arg_id(args);

	if (id_ok(id)) {
		const float *p = g_w->local[id].position;

		return s7_list(sc, 3, s7_make_real(sc, p[0]),
			       s7_make_real(sc, p[1]), s7_make_real(sc, p[2]));
	}
	return s7_f(sc);
}

/*
 * (scene-outline! id) -> id. Marks entity ID as the game's outline target (the
 * renderer's selection-outline pass highlights it in-game), or clears it when ID
 * is -1 / not a live entity. This is how image-side rules light up the picked
 * piece before its move: the read side is host-only (the renderer), so there is
 * no scene-outline getter here — the rules only ever set it.
 */
static s7_pointer sp_scene_outline(s7_scheme *sc, s7_pointer args)
{
	int32_t id = arg_id(args);

	if (g_w)
		world_set_outline(g_w, id_ok(id) ? id : -1);
	return s7_make_integer(sc, id);
}

/*
 * (scene-selected) -> the selected entity id, or -1 when nothing is selected
 * (or no world is bound). The read side of the shared selection model — the
 * same world_get_selected the editor reads through entity_api's get_selected —
 * so image-side rules can act on the entity the picker and the editor already
 * agree on, rather than only on an id handed in as a dispatch argument. There
 * is no setter twin here: selection is the host's to arbitrate.
 */
static s7_pointer sp_scene_selected(s7_scheme *sc, s7_pointer args)
{
	(void)args;
	return s7_make_integer(sc, g_w ? world_get_selected(g_w) : -1);
}

/*
 * (scene-destroy-named! "name") -> count of matches destroyed. Tombstones every
 * live entity whose name equals NAME; a destroy cascades to descendants, so
 * removing a composite (a named X parent) takes its unnamed bars with it. This
 * is how a game clears the board — sweep away every "mark" — without tracking
 * spawned ids. The forward sweep is safe against the cascade: a child tombstoned
 * by its parent's destroy is already dead when its own index comes up.
 */
static s7_pointer sp_scene_destroy_named(s7_scheme *sc, s7_pointer args)
{
	const char *name = arg_str(sc, args, 0);
	uint32_t    i, n = 0;

	if (g_w && name) {
		for (i = 0; i < g_w->count; i++) {
			const char *en = g_w->alive[i]
				? world_entity_name(g_w, i) : NULL;

			if (en && strcmp(en, name) == 0) {
				world_destroy_entity(g_w, (int32_t)i);
				n++;
			}
		}
	}
	return s7_make_integer(sc, n);
}

/*
 * (scene-clear!) — empty the bound world: entities, the editor selection and
 * the game outline all go. This is entity_api.clear_world reached from Scheme;
 * that vtable slot (entity_plugin.c's scene_clear_world) is world_reset and
 * nothing else, so the two doors cannot drift. A launcher runs this before
 * building a different scene; with scene-build! beside it, a project in the
 * image can do the same for itself instead of needing a C plugin to hold its
 * load path.
 */
static s7_pointer sp_scene_clear(s7_scheme *sc, s7_pointer args)
{
	if (g_w)
		world_reset(g_w);
	return s7_unspecified(sc);
}

/*
 * True when SRC reads as a (scene ...) form. Asked of the image's scene-form?
 * rather than re-implemented here: it is the same reader scene-build runs a
 * moment later, and it is already wrapped in a catch there, so malformed text
 * answers #f instead of throwing out of a primitive. A missing predicate (an
 * image that never loaded scene_script.scm) answers no, which lands
 * scene-build! on the same -1 a missing scene-build would.
 */
static int src_is_scene_form(s7_scheme *sc, const char *src)
{
	s7_pointer p = s7_name_to_value(sc, "scene-form?");

	if (!s7_is_procedure(p))
		return 0;
	return s7_is_eq(s7_call(sc, p, s7_list(sc, 1, s7_make_string(sc, src))),
			s7_t(sc));
}

/*
 * (scene-build! src) -> entity count, or -1 when no world is bound, the image
 * is unusable, or SRC is not a (scene ...) form. Builds SRC into the world
 * bound for this call — the Scheme twin of entity_api.build_scene_scm, and the
 * second half of a project's load path.
 *
 * SRC is source text, not an already-read form: text is what
 * scene_script_build takes, what an embedded project file arrives as, and what
 * keeps this symmetric with script_eval. The cost is one extra pass of the
 * reader (the predicate above), paid once per scene load, not per frame.
 *
 * The build re-enters scene_call_bound underneath this primitive's own live
 * binding; that nests correctly because the binding is saved and restored.
 */
static s7_pointer sp_scene_build(s7_scheme *sc, s7_pointer args)
{
	const char *src = arg_str(sc, args, 0);

	if (!g_w || !src || !src_is_scene_form(sc, src))
		return s7_make_integer(sc, -1);
	return s7_make_integer(sc, scene_script_build(g_w, g_asset, src));
}

void scene_script_bind_catalog(const struct asset_api *asset,
			       const struct asset_mut_api *mut)
{
	g_catalog     = asset;
	g_catalog_mut = mut;
}

void scene_script_init(void)
{
	static int registered;
	s7_scheme *sc;

	if (registered)
		return;
	sc = script_s7();
	if (!sc)
		return;
	s7_define_function(sc, "scene-spawn", sp_scene_spawn, 0, 1, false,
			   "(scene-spawn [parent]) -> new entity id under parent");
	s7_define_function(sc, "scene-xform!", sp_scene_xform, 10, 0, false,
			   "(scene-xform! id px py pz rx ry rz sx sy sz)");
	s7_define_function(sc, "scene-mesh!", sp_scene_mesh, 2, 0, false,
			   "(scene-mesh! id path) bind mesh by catalog path");
	s7_define_function(sc, "scene-material!", sp_scene_material, 2, 0, false,
			   "(scene-material! id path) bind material by path");
	s7_define_function(sc, "scene-script!", sp_scene_script, 2, 0, false,
			   "(scene-script! id path) bind script by path");
	s7_define_function(sc, "script-define!", sp_script_define, 2, 0, false,
			   "(script-define! path src) -> id; register a "
			   "(script ...) source at a catalog path. A second "
			   "call on the same path replaces the source in "
			   "place, keeping the id; a read-only built-in path "
			   "is refused (0).");
	s7_define_function(sc, "mesh-define!", sp_mesh_define, 2, 0, false,
			   "(mesh-define! path src) -> id; register a "
			   "(mesh ...) source at a catalog path, bindable by "
			   "scene-mesh!. A second call on the same path "
			   "replaces the source in place, keeping the id so a "
			   "bound entity picks up the new geometry; a "
			   "read-only built-in path is refused (0).");
	s7_define_function(sc, "material-define!", sp_material_define, 2, 0,
			   false,
			   "(material-define! path src) -> id; pack a "
			   "(material ...) source to the std140 block of the "
			   "shader it names and register it at a catalog path, "
			   "bindable by scene-material!. A second call on the "
			   "same path repacks in place, keeping the id so a "
			   "bound entity picks up the new look; a malformed "
			   "source, an unknown field, a wrong component count "
			   "and a read-only built-in path are all refused (0).");
	s7_define_function(sc, "scene-name!", sp_scene_name, 2, 0, false,
			   "(scene-name! id name) set entity name");
	s7_define_function(sc, "scene-entity-name", sp_scene_entity_name, 1, 0,
			   false, "(scene-entity-name id) -> name string");
	s7_define_function(sc, "scene-entity-pos", sp_scene_entity_pos, 1, 0,
			   false, "(scene-entity-pos id) -> (x y z) or #f");
	s7_define_function(sc, "scene-outline!", sp_scene_outline, 1, 0, false,
			   "(scene-outline! id) mark id as the game outline (-1 clears)");
	s7_define_function(sc, "scene-selected", sp_scene_selected, 0, 0, false,
			   "(scene-selected) -> selected entity id, or -1");
	s7_define_function(sc, "scene-destroy-named!", sp_scene_destroy_named, 1,
			   0, false,
			   "(scene-destroy-named! name) destroy entities by name");
	s7_define_function(sc, "scene-clear!", sp_scene_clear, 0, 0, false,
			   "(scene-clear!) empty the world and its selection");
	s7_define_function(sc, "scene-build!", sp_scene_build, 1, 0, false,
			   "(scene-build! src) build a scene form -> count");
	registered = 1;
}

/*
 * Bind W/ASSET for the span of one image call — the scene-* primitives read
 * them through g_w/g_asset — invoke FN with ARGS, then put back whatever was
 * bound before. A primitive only runs synchronously inside this call, so it
 * never sees a stale pointer, and the world is exposed to Scheme only while a
 * build or a dispatched event is in flight.
 *
 * Save/restore, not set/clear: these calls nest. scene-build! is a primitive,
 * so it runs with its caller's binding live, and it enters scene_script_build,
 * which comes straight back here. Clearing to NULL on the way out of that inner
 * build would unbind the world underneath the dispatch still running around it,
 * and every scene-* call after it would silently no-op. Restoring the saved
 * value leaves the outermost call to do the unbinding, since it saved NULL.
 */
static s7_pointer scene_call_bound(struct world *w,
				   const struct asset_api *asset,
				   s7_pointer fn, s7_pointer args)
{
	struct world           *saved_w     = g_w;
	const struct asset_api *saved_asset = g_asset;
	s7_pointer              res;

	g_w     = w;
	g_asset = asset;
	res = s7_call(script_s7(), fn, args);
	g_w     = saved_w;
	g_asset = saved_asset;
	return res;
}

int32_t scene_script_build(struct world *w, const struct asset_api *asset,
			   const char *src)
{
	s7_scheme *sc;
	s7_pointer fn, res;
	int32_t    count;

	if (!w || !src)
		return -1;
	sc = script_s7();
	if (!sc)
		return -1;
	fn = s7_name_to_value(sc, "scene-build");
	if (!s7_is_procedure(fn))
		return -1;

	res = scene_call_bound(w, asset, fn,
			       s7_list(sc, 1, s7_make_string(sc, src)));
	count = s7_is_integer(res) ? (int32_t)s7_integer(res) : -1;
	return count;
}

int32_t scene_script_call(struct world *w, const struct asset_api *asset,
			  const char *fn, int32_t arg)
{
	s7_scheme *sc;
	s7_pointer f, res;

	if (!w || !fn)
		return -1;
	sc = script_s7();
	if (!sc)
		return -1;
	f = s7_name_to_value(sc, fn);
	if (!s7_is_procedure(f))
		return -1;
	res = scene_call_bound(w, asset, f,
			       s7_list(sc, 1, s7_make_integer(sc, arg)));
	return s7_is_integer(res) ? (int32_t)s7_integer(res) : 0;
}

void scene_script_tick(struct world *w, const struct asset_api *asset)
{
	s7_scheme *sc;
	s7_pointer fn;

	if (!w)
		return;
	sc = script_s7();
	if (!sc)
		return;
	fn = s7_name_to_value(sc, "tick");
	if (!s7_is_procedure(fn))
		return;
	scene_call_bound(w, asset, fn, s7_nil(sc));
}
