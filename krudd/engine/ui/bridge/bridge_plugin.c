/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The bridge's wasm face: one static instance, and the four entry points the
 * editor's client calls.
 *
 * wasm-only. There is no host outside the browser that would drive this, and a
 * native build linking it would export functions nothing can reach. The pure
 * half is bridge.c, which the native test drives directly.
 *
 * ## Why the buffer is handed out rather than allocated per call
 *
 * The client writes its tape straight into the module's heap at
 * krudd_bridge_buffer() and then calls krudd_bridge_exchange() with the byte
 * count. No malloc, no copy, no ownership question — the buffer belongs to
 * this module for the life of the page, and the client only ever borrows it
 * between one write and the exchange that immediately follows.
 *
 * The reply goes the other way for the same reason: exchange() returns a
 * pointer into the bridge's own reply buffer, valid until the next exchange.
 * The client is expected to UTF8ToString it immediately, which it does, once
 * per frame.
 */
#include "bridge.h"

#include "subsystem.h"
#include "subsystem_manager.h"

#include <emscripten.h>

#include <stdint.h>

static struct bridge g_bridge;

static const struct entity_api   *g_entity;
static const struct edit_api     *g_edit;
static const struct asset_api    *g_asset;
static const struct camera_api   *g_camera;
static const struct viewport_api *g_viewport;
static const struct gizmo_api    *g_gizmo;

/*
 * The GAME / EDITOR switch, in the main module (core/plugin_abi.c).
 *
 * Declared rather than included: it is a pair of plain C symbols with no
 * header, the same way scene_renderer and entity_plugin reach it, and it is
 * shared-symbol rather than a vtable because kruddgui writes it too.
 */
void krudd_set_editor_mode(int on);
int  krudd_editor_mode(void);

static void set_editor_mode(int32_t on)
{
	krudd_set_editor_mode(on ? 1 : 0);
}

static int32_t get_editor_mode(void)
{
	return krudd_editor_mode();
}

static void bridge_subsystem_init(void)
{
	struct bridge_host host;

	host.entity          = g_entity;
	host.edit            = g_edit;
	host.asset           = g_asset;
	host.camera          = g_camera;
	host.viewport        = g_viewport;
	host.gizmo           = g_gizmo;
	host.set_editor_mode = set_editor_mode;
	host.get_editor_mode = get_editor_mode;
	bridge_init(&g_bridge, &host);
}

static void bridge_subsystem_shutdown(void)
{
	bridge_init(&g_bridge, NULL);
}

/*
 * No tick. The bridge does its work inside the exchange the editor drives, so
 * a page with no editor attached pays nothing for it — which is the state
 * every shipped game runs in.
 */
static const struct subsystem bridge_desc = {
	.name     = "bridge",
	.api      = NULL,
	.init     = bridge_subsystem_init,
	.tick     = NULL,
	.shutdown = bridge_subsystem_shutdown,
};

void bridge_plugin_entry(struct subsystem_manager *mgr)
{
	/* Resolve before register(), which calls init at once. */
	g_entity = subsystem_manager_get_api(mgr, "scene");
	g_edit   = subsystem_manager_get_api(mgr, "edit");
	/* The catalog, for the inspector's derivation — an asset's (params ...)
	 * clause lives in its source text and this is the way to it (#951). */
	g_asset  = subsystem_manager_get_api(mgr, "asset");
	/*
	 * The viewport's three (#949). All registered before this entry — see
	 * the boot order in core/engine.c — and each optional, because every
	 * command that needs one is a no-op without it.
	 */
	g_camera   = subsystem_manager_get_api(mgr, "camera");
	g_viewport = subsystem_manager_get_api(mgr, "viewport");
	g_gizmo    = subsystem_manager_get_api(mgr, "gizmo");
	subsystem_manager_register(mgr, &bridge_desc);
}

/* The wire version this build speaks. The client refuses to drive a mismatch. */
EMSCRIPTEN_KEEPALIVE int32_t krudd_bridge_protocol(void)
{
	return BRIDGE_PROTOCOL;
}

/* Where to write a tape. Stable for the life of the module. */
EMSCRIPTEN_KEEPALIVE uint8_t *krudd_bridge_buffer(void)
{
	return g_bridge.tape;
}

/* How many bytes that buffer holds. */
EMSCRIPTEN_KEEPALIVE int32_t krudd_bridge_capacity(void)
{
	return BRIDGE_TAPE_BYTES;
}

/*
 * Serve the tape currently in the buffer. Returns the reply document, valid
 * until the next call.
 */
EMSCRIPTEN_KEEPALIVE const char *krudd_bridge_exchange(int32_t len)
{
	return bridge_exchange(&g_bridge, g_bridge.tape, len);
}
