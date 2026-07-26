; SPDX-License-Identifier: GPL-2.0-or-later

;;! The module list, in dependency order — a module may only reach for one
;;! listed above it. The directory groups say which tier a module sits in, so
;;! the layout on disk carries the same ordering this list does:
;;!
;;!   abi/      the shared plugin/ABI headers every tier includes. Not a module
;;!             (no sources to build), so it is not listed here.
;;!   core/     the engine itself: subsystems, the s7 script image, the boot
;;!             paths and the browser/Qt shells.
;;!   base/     no engine concepts at all — logging, allocation, arithmetic.
;;!   world/    the scene and its data model: entities, assets, editing.
;;!   render/   the backends and the passes that drive them.
;;!   audio/    the mixer and its device backends.
;;!   ui/       the editor chrome: immediate-mode gui, viewport, kruddboard.
;;!   game/     host/ is the launcher registry; its siblings are the games
;;!             that register with it.
;;!   shader/   the shader DSL and its transpiler. Scheme only, so like abi/
;;!             it is not a module and is not listed here.

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
 "game/chess")
