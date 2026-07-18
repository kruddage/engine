/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WEBGPU_TEXTURE_REGISTRY_H
#define WEBGPU_TEXTURE_REGISTRY_H

#include <stdint.h>

/*
 * texture_registry — the id table behind the WebGPU backend's texture-handle.
 *
 * The gpu_api hands a UI layer an opaque u32 naming a texture, because the id
 * crosses into Scheme (kgui-image takes it as an s7 number) and so cannot be a
 * pointer. GL satisfies that for free — its texture names are already integers
 * — but WebGPU has nothing of the kind, so the backend keeps this table and
 * issues indices into it.
 *
 * It is deliberately free of WebGPU types: the interesting part is the id
 * algebra, not the graphics, and keeping it separable means it is tested
 * natively in CI. The native WebGPU backend needs an out-of-tree Dawn checkout
 * and is skipped without one, so logic left inside renderer_webgpu.c is logic
 * CI never compiles, let alone exercises.
 *
 * An id packs slot + 1 in its low 16 bits (so a valid id is never 0) and the
 * slot's generation in its high 16. The generation is the whole point: forget
 * bumps it, so an id minted before a texture was destroyed no longer matches
 * and resolves to nothing, rather than to whatever later took the slot. Without
 * it a recycled slot would silently hand the UI an unrelated texture — a wrong
 * picture instead of a missing one, and the wrong picture is the worse failure
 * because nothing reports it.
 */

/* Slots available. Only textures a UI composites by id are interned, not every
 * texture the engine makes, so this is sized for previews and bakes. */
#define TEXREG_CAPACITY 64

/*
 * The id naming OBJ, interning it if it is not already known. Idempotent: a
 * live object keeps the id it was given, so a caller interning every frame (the
 * scene preview does) neither exhausts the table nor changes the id under the
 * UI holding it. Returns 0 for a NULL object or a full table — never evicts a
 * live entry, since that would corrupt whoever still holds its id.
 */
uint32_t texreg_intern(void *obj);

/*
 * The object ID names, or NULL when the id is 0, malformed, out of range, or
 * names an object forgotten since the id was issued.
 */
void *texreg_resolve(uint32_t id);

/*
 * Drop OBJ, invalidating every id that named it. An object that was never
 * interned is simply absent, so this is safe to call on every destroy.
 */
void texreg_forget(void *obj);

/* Drop everything. For tests and backend teardown, not the steady state. */
void texreg_reset(void);

#endif /* WEBGPU_TEXTURE_REGISTRY_H */
