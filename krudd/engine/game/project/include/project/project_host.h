/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PROJECT_HOST_H
#define PROJECT_HOST_H

#include <stdint.h>

struct subsystem_manager;

/*
 * project_host — runs a project: a whole game written as one (project ...) form
 * of Scheme, with no C plugin of its own behind it.
 *
 * A built-in game's C plugin is four things and no more: it evaluates its rules
 * into the shared image at register time, offers a display name on the
 * launcher, and — when chosen — clears the world, builds its scene and starts
 * a fresh game; then each frame it checks whether it is the loaded game,
 * notices that the engine's selection has CHANGED, and hands the new entity to
 * its rules.
 *
 * Every one of those became writable in Scheme as the primitives landed
 * (#977-#980), so none of them is a reason for a game to be C any more. This
 * module is those four things once, generically, driven by a form instead of by
 * a compiled plugin. It is a host, not a game: nothing here knows any game's
 * name, and adding a game adds no C at all.
 *
 * The form and its clauses are documented in game/project/project.scm, which is
 * the image half of this module; what follows is the design behind the hook set
 * it offers, which is the decision this module exists to make.
 *
 *
 * THE HOOKS, AND WHY THE HOST OWNS THE SELECTION EDGE
 *
 * A project declares (on-load ...), (on-tick ...) and (on-selected ...). The
 * first two are unarguable — a load callback and a frame callback are what the
 * launcher and the frame loop already are. The third is the decision, because a
 * project could write it itself: (scene-selected) is a primitive, on-tick runs
 * every frame with the live world bound, and the whole of it is five lines of
 * "remember the last value, fire when it differs". The alternative design —
 * offer only (on-tick ...) and let each project write those five lines — is the
 * more honest reading of "the engine gets out of the way", and it was the
 * default going in. This module does not take it, for three reasons:
 *
 *   1. The state whose edge it is belongs to the host. The ray pick, the
 *      selection it writes and the editor that shares it are all engine
 *      machinery; (scene-selected) reports a LEVEL — what is selected now, and
 *      the same answer again next frame while the player still holds a piece.
 *      A project that debounces that is not expressing a rule of its game, it
 *      is compensating for the shape of an engine reading. There is exactly one
 *      correct way to do it, so leaving it to every project is asking each of
 *      them to get the same thing right, silently, forever.
 *
 *   2. The baseline has to be re-armed on load, and that is the part a project
 *      would get wrong. A project's own edge detector remembers a selection
 *      from the last time it was the loaded game; come back to it later and the
 *      first click on that same entity is not an edge and does nothing. The bug
 *      only appears on the second load, which is exactly the class of thing a
 *      host retires once (project-open re-arms it from the world itself).
 *
 *   3. The host already owns the sibling half of this. The active-project gate
 *      below is host-owned by the same argument, and the built-in plugin that
 *      makes that check needed a seven-line comment to explain why its tick had
 *      to make it at all. Owning "when your hooks are meaningful" while leaving
 *      "what a click is" to the project would be a line drawn nowhere.
 *
 * This is not the engine failing to get out of the way, because (on-selected
 * ...) is not knowledge of any game: it is the engine reporting its OWN event
 * in event form, the way (tick) already reports the frame. And the escape hatch
 * is real rather than rhetorical — on-tick runs every frame with the world
 * bound and (scene-selected) is right there, so a project that wants different
 * semantics (fire while held, fire on release, fire only on a second click)
 * writes its own and omits on-selected. The declarative hook is the default,
 * not the only door.
 *
 * What the host deliberately does NOT decide is everything past the edge: which
 * entity a click means, whether the move was legal, what it sounds like
 * (audio-play! is the project's to call), what lights up. A host that owned any
 * of those would be a game.
 *
 *
 * THE GATE
 *
 * Every registered subsystem ticks every frame regardless of what the launcher
 * loaded, so a project's hooks run only while its own launcher entry is the
 * loaded one — the host makes that check, once, on behalf of all of them. A
 * project therefore never mentions game_active_index(), and a project that is
 * merely registered is inert.
 *
 *
 * REACHING SCHEME, AND WHY game/host STAYS PLAIN C
 *
 * The launcher registry is deliberately C with no idea that s7 exists, and it
 * stays that way: (game-register! name thunk) registers ONE C trampoline
 * through the ordinary game_register(), for every project. The trampoline works
 * out which project it was called for from game_active_index(), which game.h
 * documents as being set before the load callback runs.
 *
 * The awkward part is not the callback but the binding: a hook must run with
 * the live world bound or every scene-* primitive in it is a no-op, and the one
 * door to that is entity_api's dispatch_scm(NAME, ARG) — which names a
 * procedure rather than taking one. So a registered load thunk is bound to a
 * generated global (project-load-<index>) and dispatched by that name, and the
 * per-frame hooks go through one fixed name with the launcher index as the
 * argument. The alternative — an abi entry that takes an s7_pointer — would put
 * the interpreter into the plugin vtable that every tier includes, to save this
 * module a snprintf.
 */

/*
 * Register the "project" subsystem: the (game-register! ...) primitive, the
 * project image, and the per-frame tick that runs the loaded project's hooks.
 * Resolves the "scene" api (the entity plugin) for the world binding, so the
 * entity plugin must already be registered — and, since projects put entries on
 * the launcher, this must run before a boot default is opened.
 */
void project_host_plugin_entry(struct subsystem_manager *mgr);

/*
 * Evaluate SRC — a project source — and return the launcher index the
 * (project ...) form in it took, or -1 if it registered nothing. Every
 * top-level form in SRC is evaluated, so a project file may carry its rules
 * around its project form as readily as inside a (rules ...) clause.
 *
 * A source that does not read, one that carries no project form, and one whose
 * form is malformed or partial all answer -1 with a log line saying which: a
 * project is user input, not a compiled-in plugin, so being wrong about one is
 * a report and not a fault. Nothing is half-registered by a refusal.
 *
 * Where the source comes from is the caller's business — this module holds no
 * opinion about files, and the tests hand it a string.
 */
int32_t project_host_eval(const char *src);

/*
 * Evaluate SRC and open the project it registered, returning that project's
 * launcher index or -1. This is project_host_eval followed by the launcher
 * click, which is the whole of what "load this file" means: the previous
 * project's world is cleared and this one's scene is built in its place, its
 * on-load runs, and from the next frame its hooks are the ones that run.
 *
 * It is the door for a project that arrives while the engine is already up — a
 * file the player picked, or one the page fetched out of the site's assets/
 * because ?game= named a project this image does not carry — as opposed to the
 * project the build shipped staged, which core evaluates at boot through
 * project_host_eval so that a ?game= for its name resolves without a fetch. The
 * distinction is not cosmetic, and it is the reason this is a second entry
 * point rather than a flag:
 *
 *   THE DOOR OWNS ONE LAUNCHER ENTRY. What comes through it is whatever the
 *   player last opened, and there is one of those, so a second project renames
 *   that entry rather than taking another beside it. Loading five files leaves
 *   one entry, not five stale ones, and never exhausts the registry. What the
 *   build ships — the staged project, and anything a plugin registered — keeps
 *   its own entry: those are facts about this engine, and a file someone opened
 *   for a minute does not displace them. A source whose project name is already
 *   on the launcher (opening the staged project's own .scm out of assets/, say)
 *   reuses the entry it already has, so that path is a reload rather than a
 *   duplicate.
 *
 *   REPLACEMENT IS THE POINT, LAYERING IS THE BUG. The world goes because
 *   project-open clears it before building; the old project's hooks go because
 *   only the loaded project's record is dispatched (project-host-tick), and its
 *   record is dropped when its slot is re-registered. What cannot go is what
 *   the previous project defined at top level in the shared image: a project is
 *   evaluated into the rootlet, and Scheme has no unloading. Two projects that
 *   prefix their definitions (the convention everywhere in this tree) do not
 *   collide; two that both define `reset` do, and the second one wins.
 *
 * A source that does not read, carries no project form, or is refused for any
 * of the reasons project_host_eval lists opens nothing and answers -1, leaving
 * the project that was already loaded running. A bad file is the expected case
 * for a button that opens arbitrary .scm, not an edge case.
 */
int32_t project_host_load(const char *src);

#endif /* PROJECT_HOST_H */
