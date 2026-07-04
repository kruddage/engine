/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VSCRIPT_H
#define VSCRIPT_H

#include "vscript_api.h"
#include "subsystem_manager.h"

#include <stdint.h>

/*
 * Visual-scripting graph core (sub-issue 1 of the #242 epic).  The public
 * cross-plugin surface is the "vscript" service vtable in vscript_api.h; the
 * declarations here are the native-build entry point and the pieces the
 * codec and native tests reach directly.
 */

/* Native accessor for the service vtable (tests + native consumers). */
const struct vscript_api *vscript_native_api(void);

/*
 * Binary .vscript codec.  decode returns a freshly allocated vscript_graph_t
 * (NULL on a malformed buffer or an unregistered node type); encode serializes
 * a graph to a freshly allocated byte buffer, writing its size to *out_size.
 * Registered against asset_codec under the "vscript" extension.
 */
void *vscript_decode(const void *bytes, uint32_t size);
void *vscript_encode(const void *typed, uint32_t *out_size);

/*
 * Canonical decl value for a target discriminator, or NULL for an invalid
 * target.  A consumer mirrors this under VSCRIPT_TARGET_DECL_KEY via
 * asset_mut.set_decl so a graph asset can be filtered by target without being
 * decoded; decode + require_target is the authoritative check.
 */
const char *vscript_target_decl_value(uint32_t target);

#ifndef __EMSCRIPTEN__
void vscript_plugin_entry(struct subsystem_manager *mgr);
#endif

#endif /* VSCRIPT_H */
