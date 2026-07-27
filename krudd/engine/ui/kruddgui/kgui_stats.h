/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KGUI_STATS_H
#define KGUI_STATS_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * kgui_stats — the (krudd-stats) accessor the perf HUD reads.
 *
 * The HUD (kruddgui-perf-hud-draw in kruddgui.scm) is canvas, not chrome:
 * kruddgui.cpp calls it every tick in both modes, because a hitch has to be
 * visible in a game's play view where the DOM chrome deliberately does not
 * exist. Its one engine input is this accessor.
 *
 * It lives here rather than beside the other primitives in kruddgui.cpp
 * because that file is wasm-only, so nothing native could link the real
 * binding and every test had to stub it. The stub then drifted from a binding
 * that had stopped existing at all: the accessor went with kruddboard.cpp in
 * #661, so in the shipping build the HUD's opening (krudd-stats) hit an
 * unbound symbol, the error was swallowed inside the host's s7_call, and the
 * HUD drew nothing every frame for a whole release without leaving a trace
 * (#911). Split out, kgui_perf_test.c drives the same code the browser does.
 *
 * This is the one accessor of the ~70 that went with kruddboard that comes
 * back. The rest are #902's job, over a typed JS bridge rather than an s7
 * shim into a parked Scheme image.
 */

struct s7_scheme;
struct stats_api;

/*
 * Hands back the live "stats" vtable, or NULL when no stats subsystem is
 * registered. Called per invocation rather than cached, so a host whose stats
 * subsystem registers after kruddgui still resolves it, and one that has none
 * keeps reporting so for the life of the process.
 */
typedef const struct stats_api *(*kgui_stats_resolve_fn)(void);

/*
 * Define (krudd-stats) in SC's rootlet, reading through RESOLVE.
 *
 *   (krudd-stats) -> (fps frame-ms frame-count)   stats subsystem present
 *                 -> #f                            stats subsystem absent
 *
 * fps is the rolling average and frame-ms the last frame's wall time — the
 * headline number and the graph's sample. Returning #f rather than a list of
 * zeroes is what lets the HUD tell "no stats" apart from "0 fps" and draw
 * nothing, which is the documented behaviour off the browser.
 *
 * A NULL SC is a no-op; a NULL RESOLVE registers the accessor as permanently
 * #f. Calling this twice rebinds the resolver, which is what a test suite
 * driving one process-global interpreter needs.
 */
void kgui_stats_register(struct s7_scheme *sc, kgui_stats_resolve_fn resolve);

#ifdef __cplusplus
}
#endif

#endif /* KGUI_STATS_H */
