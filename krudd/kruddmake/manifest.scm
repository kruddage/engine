; SPDX-License-Identifier: GPL-2.0-or-later

;;! The directories that carry a build.scm, in dependency order — a module may
;;! only reach for one listed above it. The directory groups say which tier a
;;! module sits in, so the layout on disk carries the same ordering this list
;;! does.
;;!
;;! resolve-check-tiers reads this list and fails generation on any `(library
;;! … (link …))` edge that inverts it, so the rule above is enforced rather
;;! than remembered (#923). Executables are exempt: nothing links one, so
;;! core's `index` linking every backend is the main-module link, not a tier
;;! reaching downward.
;;!
;;! Most entries are modules: Scheme spec + C, building libraries, executables
;;! and tests. A few build nothing and are listed only because they declare code
;;! generation — an `(embed)`, an `(emit-interface-header)`, a
;;! `(configure-file)`. Those declarations used to have nowhere to live, so they
;;! sat as literals in the generator and drifted out of sync with the list of
;;! sources the `regen` edge watches (#779, #787). A directory that declares a
;;! build fact gets a spec, whether or not it also builds something.
;;!
;;!   abi/      the plugin vtables every tier includes, and nothing else. Hand-
;;!             written headers only — nothing to build and nothing to generate,
;;!             so for a long time it carried no spec at all and was absent from
;;!             this list. That is right for a build system and wrong for a
;;!             dependency graph: it is the highest fan-in node in the tree, and
;;!             while it was unlisted the check below could not have an opinion
;;!             about it. It now carries an `interface-library` — a public
;;!             surface and nothing else, emitting no ninja edge — and is first
;;!             here, which is what "everything may reach for abi, abi reaches
;;!             for nothing" looks like as a position (#919). It includes no
;;!             module's headers: where a vtable needs a type it owns no
;;!             definition of, it forward-declares the tag rather than reaching
;;!             down into the module that implements it.
;;!   base/     no engine concepts at all — logging, allocation, arithmetic.
;;!             Includes the spatial types (struct transform, struct mat4),
;;!             which are geometry rather than world data model, so base/ can
;;!             stay strictly below world/. It links nothing outside itself,
;;!             which is what the bottom of the order looks like.
;;!   core/     the engine itself: subsystems, the s7 script host and image,
;;!             and the boot path. Not the shells — see shell/ below. Listed
;;!             after base/ because `script` logs: 26 libraries link `log` and
;;!             32 link `memory`, which is the fan-in of a tier below the
;;!             engine core, not above it. core/ was first here until the check
;;!             below had an opinion about it (#923).
;;!   world/    the scene and its data model: entities, assets, editing.
;;!   render/   the backends and the passes that drive them, plus the two
;;!             Scheme sources the renderer generates from: renderer.scm (the
;;!             backend interface header) and shader/ (the shader DSL and its
;;!             GLSL/WGSL transpiler). Neither builds a target; both are listed,
;;!             for the one codegen declaration each.
;;!   audio/    the mixer and its device backends.
;;!   ui/       the editor chrome: immediate-mode gui, viewport, kruddboard.
;;!   game/     host/ is the launcher registry and project/ the generic host
;;!             that runs a game written as one (project ...) form. No game
;;!             lives here any more — see projects/ below. project_test/ is the
;;!             harness those projects are driven through under test, packaged
;;!             once instead of assembled by hand in every directory that has a
;;!             project in it (#1035). It is native-only — a harness must not
;;!             enter the WASM image — and it is last of the three because it
;;!             links the world/ libraries the other two do not, which is a
;;!             tier fact the check below reads rather than takes on trust.
;;!   shell/    the host the engine runs inside: web/, the browser page (PWA
;;!             manifest, service worker, icons, the emscripten shell
;;!             template). Last of the engine tiers on purpose — a shell may
;;!             reach for anything, and nothing may reach for a shell. web/
;;!             builds no targets — its assets are copied by the generator —
;;!             but it is listed, for the shell template it configures.
;;!   projects/ the games, plus `default` — the scene the page opens on, which
;;!             is a project rather than something the renderer seeds (#1034).
;;!             NOT an engine tier and not under krudd/engine at
;;!             all: these entries resolve against the repository root, which
;;!             is what the `projects/` prefix means to rz-spec-path. A project
;;!             is a single .scm the engine loads at runtime (#976), so it is
;;!             content the build stages, not a module the engine links —
;;!             which is why it sits at the top level next to krudd/ rather
;;!             than inside it. They are listed here for two reasons only: one
;;!             of them claims the (staged-project ...) slot, and each declares
;;!             a native test that drives its rules. Last in the order because
;;!             a project may reach for anything and nothing may reach for a
;;!             project; nothing here declares a library, so no tier edge can
;;!             point at one in any case — and resolve-check-projects is what
;;!             keeps that true rather than merely observed. The rest of what a
;;!             project may and may not do is projects/README.md.



("abi"
 "base/log"
 "base/memory"
 "base/math"
 "core"
 "world/edit"
 "world/entity"
 "world/asset"
 "render"
 "render/shader"
 "render/null"
 "render/webgl"
 "render/webgpu"
 "render/frame_graph"
 "render/particles"
 "render/scene_renderer"
 "audio"
 "ui/kruddboard"
 "ui/kruddgui"
 "ui/viewport"
 "game/host"
 "game/project"
 "game/project_test"
 "shell/web"
 "projects/default"
 "projects/chess")
