; SPDX-License-Identifier: GPL-2.0-or-later

;;! The module list, in dependency order — a module may only reach for one
;;! listed above it. The directory groups say which tier a module sits in, so
;;! the layout on disk carries the same ordering this list does:
;;!
;;!   abi/      the plugin vtables every tier includes, and nothing else. Not a
;;!             module (no sources to build), so it is not listed here. It
;;!             includes no module's headers: where a vtable needs a type it
;;!             owns no definition of, it forward-declares the tag rather than
;;!             reaching down into the module that implements it.
;;!   core/     the engine itself: subsystems, the s7 script host and image,
;;!             and the boot path. Not the shells — see shell/ below.
;;!   base/     no engine concepts at all — logging, allocation, arithmetic.
;;!             Includes the spatial types (struct transform, struct mat4),
;;!             which are geometry rather than world data model, so base/ can
;;!             stay strictly below world/.
;;!   world/    the scene and its data model: entities, assets, editing.
;;!   render/   the backends and the passes that drive them, plus the two
;;!             Scheme sources the renderer generates from: renderer.scm (the
;;!             backend interface header) and shader/ (the shader DSL and its
;;!             GLSL/WGSL transpiler). Neither is a module — they have no
;;!             sources to build — so neither is listed here.
;;!   audio/    the mixer and its device backends.
;;!   ui/       the editor chrome: immediate-mode gui, viewport, kruddboard.
;;!   game/     host/ is the launcher registry; its siblings are the games
;;!             that register with it.
;;!   shell/    the hosts the engine runs inside: qt/ is the Qt-and-Vulkan
;;!             native editor, web/ the browser page (PWA manifest, service
;;!             worker, icons, the emscripten shell template). Last in the
;;!             order on purpose — a shell may reach for anything, and nothing
;;!             may reach for a shell. web/ builds no targets, so like abi/ it
;;!             is not listed below; its assets are copied by the generator.

("core"
 "base/log"
 "base/memory"
 "base/math"
 "world/edit"
 "world/entity"
 "world/asset"
 "render/null"
 "render/webgl"
 "render/webgpu"
 "render/vulkan"
 "render/frame_graph"
 "render/particles"
 "render/scene_renderer"
 "audio"
 "ui/kruddboard"
 "ui/kruddgui"
 "ui/viewport"
 "game/host"
 "game/tictactoe"
 "game/chess"
 "shell/qt")
