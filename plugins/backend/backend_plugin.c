/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "backend_record.h"
#include "backend_api.h"
#include "asset_api.h"
#include "log_api.h"
#include "memory_api.h"
#include "subsystem.h"
#include "subsystem_manager.h"
#include "async_subsystem.h"

#include <string.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include "log.h"
static const struct log_api native_log = { log_write };
#endif

#ifdef __EMSCRIPTEN__
static const struct log_api    *g_log;
static const struct memory_api *g_mem;
#else
static const struct log_api    *g_log = &native_log;
#endif

static struct subsystem_manager *g_mgr;

#ifdef __EMSCRIPTEN__

/*
 * IndexedDB bridge.  These EM_JS functions are defined in the main module
 * (modules/core/plugin_abi.c) and imported here — a side module's own EM_JS
 * would become a throwing stub, so the bodies must live in the main module.
 * The interface is deliberately plain C->JS: loaded records are staged in a
 * JS-side queue that we drain with peek/pop, and we learn when the open has
 * finished by polling krudd_idb_state() from emscripten_async_call — never by
 * JS calling back into this side module by name.
 */
extern void krudd_idb_open(void);
extern int  krudd_idb_state(void);
extern int  krudd_idb_peek(uint32_t *id_out, int32_t *type_out,
			   char *path_out, uint32_t path_cap);
extern void krudd_idb_pop(uint8_t *dst);
extern void krudd_idb_put(uint32_t id, const char *path, int32_t type,
			  const void *data, uint32_t size);
extern void krudd_idb_del(uint32_t id);

/* Path buffer for peek; matches ASSET_PATH_MAX in the asset plugin. */
#define BACKEND_PATH_MAX 256

static void (*g_ready_done)(void *ctx);

/*
 * Drain every staged record into the catalog as an authored project asset.
 * Runs once, after the open completes, before we report ready.
 */
static void rehydrate_drain(void)
{
	const struct asset_mut_api *am;
	char                        path[BACKEND_PATH_MAX];
	uint32_t                    id;
	int32_t                     type;
	int                         len;
	uint8_t                    *buf;

	am = subsystem_manager_get_api(g_mgr, "asset_mut");
	if (!am) {
		g_log->write(LOG_LEVEL_WARN,
			     "backend: asset_mut unavailable; cannot rehydrate");
		return;
	}

	while ((len = krudd_idb_peek(&id, &type, path, sizeof(path))) >= 0) {
		buf = NULL;
		if (len > 0) {
			buf = g_mem ? g_mem->alloc((size_t)len) : NULL;
			if (!buf) {
				/* Drop this record rather than spin. */
				krudd_idb_pop(NULL);
				g_log->write(LOG_LEVEL_WARN,
					     "backend: out of memory rehydrating %s",
					     path);
				continue;
			}
		}
		krudd_idb_pop(buf);

		if (am->create(path, type, buf, (uint32_t)len) != 0)
			g_log->write(LOG_LEVEL_INFO,
				     "backend: rehydrated %s", path);
		else
			g_log->write(LOG_LEVEL_WARN,
				     "backend: rehydration failed for %s", path);

		if (buf)
			g_mem->free(buf);
	}
}

/*
 * Poll the IndexedDB open until it resolves.  Reschedules itself while the
 * open is in flight; on success rehydrates, on failure clears the persist
 * capability — then reports the subsystem ready either way.
 */
static void backend_poll(void *ctx)
{
	int state = krudd_idb_state();

	if (state == 0) {
		emscripten_async_call(backend_poll, ctx, 16);
		return;
	}

	if (state == 2) {
		backend_record_mark_idb_unavailable();
		g_log->write(LOG_LEVEL_WARN,
			     "backend: IndexedDB unavailable; persistence disabled");
	} else {
		rehydrate_drain();
		g_log->write(LOG_LEVEL_INFO, "backend: ready");
	}

	if (g_ready_done)
		g_ready_done(ctx);
}

#endif /* __EMSCRIPTEN__ */

/* ------------------------------------------------------------------ */
/* backend_api vtable                                                  */
/* ------------------------------------------------------------------ */

static uint32_t backend_get_caps(void)
{
	return backend_record_get_caps();
}

static int32_t backend_persist_asset(uint32_t id, const char *path,
				     int32_t type, const void *bytes,
				     uint32_t size)
{
	if (backend_record_validate(id, path, bytes, size) != 0) {
		if (size > BACKEND_RECORD_MAX)
			g_log->write(LOG_LEVEL_WARN,
				     "backend: persist_asset id=%u size=%u "
				     "exceeds BACKEND_RECORD_MAX", id, size);
		return -1;
	}
#ifdef __EMSCRIPTEN__
	krudd_idb_put(id, path, type, bytes, size);
	return 0;
#else
	(void)type;
	return -1;
#endif
}

static int32_t backend_delete_asset(uint32_t id)
{
	if (!(backend_record_get_caps() & BACKEND_CAP_PROJECT_PERSIST))
		return -1;
	if (id == 0)
		return -1;
#ifdef __EMSCRIPTEN__
	krudd_idb_del(id);
	return 0;
#else
	return -1;
#endif
}

static const struct backend_api g_backend_api = {
	.get_caps      = backend_get_caps,
	.persist_asset = backend_persist_asset,
	.delete_asset  = backend_delete_asset,
};

/* ------------------------------------------------------------------ */
/* Subsystem lifecycle                                                 */
/* ------------------------------------------------------------------ */

static void backend_async_init(void (*done)(void *ctx), void *ctx)
{
#ifdef __EMSCRIPTEN__
	g_ready_done = done;
	krudd_idb_open();
	emscripten_async_call(backend_poll, ctx, 16);
#else
	/*
	 * Native: no IndexedDB.  Clear PROJECT_PERSIST so a caller querying
	 * get_caps() sees no persistence, then report ready immediately.
	 */
	backend_record_mark_idb_unavailable();
	done(ctx);
#endif
}

static void backend_shutdown(void)
{
	g_log->write(LOG_LEVEL_INFO, "backend: shutdown");
}

static const struct async_subsystem desc = {
	.name       = "backend",
	.api        = &g_backend_api,
	.async_init = backend_async_init,
	.tick       = NULL,
	.shutdown   = backend_shutdown,
};

/* ------------------------------------------------------------------ */
/* Plugin entry point                                                  */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
void plugin_entry(struct subsystem_manager *mgr)
#else
void backend_plugin_entry(struct subsystem_manager *mgr)
#endif
{
#ifdef __EMSCRIPTEN__
	g_log = subsystem_manager_get_api(mgr, "log");
	g_mem = subsystem_manager_get_api(mgr, "memory");
#endif
	g_mgr = mgr;
	subsystem_manager_register_async(mgr, &desc);
}
