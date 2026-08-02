/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KGUI_ACCESSORS_H
#define KGUI_ACCESSORS_H

#ifdef __cplusplus
extern "C" {
#endif

struct s7_scheme;
struct subsystem_manager;

/*
 * kgui_accessors — the krudd-* primitives kruddgui.scm reads the live engine
 * through, and the reason its panel set can draw again.
 *
 * kruddgui.scm was authored against a set of accessors kruddboard registered;
 * kruddboard went in #661 and took them with it, which is why kruddgui-draw sat
 * parked and even the perf HUD called an unbound (krudd-stats). Nothing about
 * those accessors was kruddboard-specific: every one of them is a projection of
 * a subsystem api the engine still publishes (log, stats, scene, asset,
 * asset_mut, edit, mesh_preview) or of script.h's parameter introspection. This
 * module is that projection, and nothing else — it holds no editor state beyond
 * the gizmo mode noted below, and no drawing.
 *
 * It is plain C and depends only on s7, script.h, the subsystem manager and the
 * abi/ headers, so it builds and is tested natively (kgui_accessors_test) rather
 * than only inside the wasm-only kruddgui library that consumes it.
 */
void kgui_accessors_register(struct s7_scheme *sc,
			     const struct subsystem_manager *mgr);

#ifdef __cplusplus
}
#endif

#endif /* KGUI_ACCESSORS_H */
