/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "backend_record.h"
#include "backend_api.h"
#include "asset_api.h"
#include "log_api.h"
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
#else
static const struct log_api    *g_log = &native_log;
#endif

static struct subsystem_manager *g_mgr;

#ifdef __EMSCRIPTEN__

/*
 * Startup rehydration context.  Passed to the JS enumerate callback so
 * it can resolve asset_mut and reinject each stored record.
 */
struct rehydrate_ctx {
	void (*done)(void *ctx);
	void *done_ctx;
};

static struct rehydrate_ctx g_rh_ctx;

/*
 * idb_open_and_enumerate — open "krudd-project" (creating it when
 * absent) and drive one C callback per stored record, then fire the
 * async-ready done callback.
 *
 * Called once at plugin_entry time; drives startup rehydration before
 * the subsystem is marked ready so the editor never sees a stale world.
 *
 * JS object store layout (version: 1):
 *   { version: 1, id: <uint32>, path: <string>,
 *     type: <int32>, data: <ArrayBuffer> }
 * Key: id (keyPath "id").
 *
 * The C callbacks called from JS:
 *   backend_on_record(id, path_ptr, type, data_ptr, size) — one per record
 *   backend_on_open_done()  — IDB open + enumerate finished (or failed)
 *   backend_on_idb_error()  — IDB unavailable; clears CAP_PROJECT_PERSIST
 */
EM_JS(void, idb_open_and_enumerate, (void), {
	var DB_NAME  = "krudd-project";
	var DB_VER   = 1;
	var STORE    = "assets";

	var req = indexedDB.open(DB_NAME, DB_VER);

	req.onupgradeneeded = function(ev) {
		var db = ev.target.result;
		if (!db.objectStoreNames.contains(STORE))
			db.createObjectStore(STORE, { keyPath: "id" });
	};

	req.onerror = function() {
		_backend_on_idb_error();
	};

	req.onsuccess = function(ev) {
		var db = ev.target.result;
		var tx = db.transaction(STORE, "readonly");
		var st = tx.objectStore(STORE);
		var cr = st.openCursor();

		cr.onerror = function() {
			_backend_on_open_done();
		};

		cr.onsuccess = function(cev) {
			var cursor = cev.target.result;
			if (!cursor) {
				_backend_on_open_done();
				return;
			}
			var rec = cursor.value;

			/* Version guard: skip unrecognised versions */
			var ver = (rec.version >>> 0);
			if (_backend_record_check_version(ver) !== 0) {
				cursor.continue();
				return;
			}

			var id   = (rec.id   >>> 0);
			var type = (rec.type | 0);

			/* Encode path to WASM heap */
			var pathStr  = rec.path || "";
			var pathLen  = lengthBytesUTF8(pathStr) + 1;
			var pathPtr  = _malloc(pathLen);
			if (!pathPtr) { cursor.continue(); return; }
			stringToUTF8(pathStr, pathPtr, pathLen);

			/* Copy ArrayBuffer bytes to WASM heap */
			var ab    = rec.data;
			var size  = ab ? ab.byteLength : 0;
			var dataPtr = 0;
			if (size > 0) {
				dataPtr = _malloc(size);
				if (!dataPtr) {
					_free(pathPtr);
					cursor.continue();
					return;
				}
				HEAPU8.set(new Uint8Array(ab), dataPtr);
			}

			_backend_on_record(id, pathPtr, type, dataPtr, size);
			_free(pathPtr);
			if (dataPtr) _free(dataPtr);

			cursor.continue();
		};
	};
})

/*
 * idb_persist — insert or update a single record in "assets".
 * Keyed by id.  data_ptr/size are copied into an ArrayBuffer so IDB
 * takes ownership and the WASM heap can be freed immediately after.
 */
EM_JS(void, idb_persist, (uint32_t id, const char *path_ptr, int32_t type,
			  const void *data_ptr, uint32_t size), {
	var DB_NAME = "krudd-project";
	var DB_VER  = 1;
	var STORE   = "assets";

	var path = UTF8ToString(path_ptr);
	var ab   = new ArrayBuffer(size);
	if (size > 0)
		new Uint8Array(ab).set(HEAPU8.subarray(data_ptr, data_ptr + size));

	var rec = { version: 1, id: id >>> 0, path: path,
		    type: type | 0, data: ab };

	var req = indexedDB.open(DB_NAME, DB_VER);
	req.onsuccess = function(ev) {
		var db = ev.target.result;
		var tx = db.transaction(STORE, "readwrite");
		tx.objectStore(STORE).put(rec);
	};
})

/*
 * idb_delete — remove the record with the given id.
 */
EM_JS(void, idb_delete, (uint32_t id), {
	var DB_NAME = "krudd-project";
	var DB_VER  = 1;
	var STORE   = "assets";

	var req = indexedDB.open(DB_NAME, DB_VER);
	req.onsuccess = function(ev) {
		var db = ev.target.result;
		var tx = db.transaction(STORE, "readwrite");
		tx.objectStore(STORE).delete(id >>> 0);
	};
})

/*
 * Called by the JS cursor for each stored record.
 * Resolves asset_mut and calls create(); id / type match the stored
 * values so the catalog gets the same stable id back.
 */
EMSCRIPTEN_KEEPALIVE
void backend_on_record(uint32_t id, const char *path,
		       int32_t type, const void *bytes, uint32_t size)
{
	const struct asset_mut_api *am;

	(void)id; /* asset_mut.create() issues a new id; we log discrepancy */
	am = subsystem_manager_get_api(g_mgr, "asset_mut");
	if (!am) {
		g_log->write(LOG_LEVEL_WARN,
			     "backend: asset_mut not available during rehydration");
		return;
	}
	if (!path || !path[0]) {
		g_log->write(LOG_LEVEL_WARN,
			     "backend: skipping record with empty path");
		return;
	}
	if (am->create(path, type, bytes, size) == 0) {
		g_log->write(LOG_LEVEL_WARN,
			     "backend: rehydration failed for %s", path);
	} else {
		g_log->write(LOG_LEVEL_INFO,
			     "backend: rehydrated %s", path);
	}
}

/*
 * Called by JS once the IDB open + full cursor traversal is complete.
 * Fires the async-ready done callback so the subsystem manager marks
 * the backend ready and drives any on_ready listeners.
 */
EMSCRIPTEN_KEEPALIVE
void backend_on_open_done(void)
{
	g_log->write(LOG_LEVEL_INFO, "backend: ready");
	if (g_rh_ctx.done)
		g_rh_ctx.done(g_rh_ctx.done_ctx);
}

/*
 * Called by JS when IndexedDB is unavailable (private-browsing, etc.).
 * Clears CAP_PROJECT_PERSIST so the editor can hide its Save button,
 * then still fires done so the subsystem becomes ready in no-persist mode.
 */
EMSCRIPTEN_KEEPALIVE
void backend_on_idb_error(void)
{
	backend_record_mark_idb_unavailable();
	g_log->write(LOG_LEVEL_WARN,
		     "backend: IndexedDB unavailable; persistence disabled");
	if (g_rh_ctx.done)
		g_rh_ctx.done(g_rh_ctx.done_ctx);
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
	idb_persist(id, path, type, bytes, size);
	return 0;
#else
	/* Native: no IndexedDB; no-op. */
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
	idb_delete(id);
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
	g_rh_ctx.done     = done;
	g_rh_ctx.done_ctx = ctx;
	idb_open_and_enumerate();
#else
	/*
	 * Native: no IndexedDB.  Clear PROJECT_PERSIST so the native
	 * shell can query get_caps() and see no persistence available,
	 * then fire done immediately.
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
#endif
	g_mgr = mgr;
	subsystem_manager_register_async(mgr, &desc);
}
