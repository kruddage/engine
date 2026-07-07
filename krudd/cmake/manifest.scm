; SPDX-License-Identifier: GPL-2.0-or-later
;
; The manifest: every directory krudd owns, as a path relative to krudd/cmake/
; (which matches the add_subdirectory() layout in the root spec). The spec for
; each lives at krudd/cmake/<dir>/CMakeLists.scm. Keep this list in sync with
; .gitignore.
;
; This is a bare datum (a list of strings) read by both build.scm (the CMake
; backend) and krudd/ninja (the Ninja backend), so the two emitters share one
; source of truth for what krudd owns.

("modules/core"
 "modules/log"
 "modules/memory"
 "plugins/math"
 "plugins/renderer"
 "plugins/cas"
 "plugins/branch"
 "plugins/snapshot"
 "plugins/hello_plugin"
 "plugins/scene_plugin"
 "plugins/vscript"
 "plugins/shader_graph"
 "plugins/edit_plugin"
 "plugins/entity_plugin"
 "plugins/renderer_null"
 "plugins/renderer_webgl"
 "plugins/frame_graph"
 "plugins/scene_renderer"
 "plugins/asset"
 "plugins/backend"
 "plugins/imgui_plugin"
 "plugins/kruddboard")
