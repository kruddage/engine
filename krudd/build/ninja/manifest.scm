; SPDX-License-Identifier: GPL-2.0-or-later
;; scm-lint:off
;
; The manifest: every directory krudd owns, as a path relative to
; krudd/build/ninja/. The spec for each lives at krudd/build/ninja/<dir>/build.scm.
; Keep this list in sync with .gitignore.
;
; This is a bare datum (a list of strings) read by krudd/build/build.scm (the
; driver) and krudd/build/ninja/resolve.scm (the resolver), the one source of
; truth for what krudd owns.
;; scm-lint:on

("modules/core"
 "modules/log"
 "modules/memory"
 "modules/math"
 "modules/renderer"
 "modules/scene_plugin"
 "modules/edit_plugin"
 "modules/entity_plugin"
 "modules/renderer_null"
 "modules/renderer_webgl"
 "modules/frame_graph"
 "modules/scene_renderer"
 "modules/asset"
 "modules/imgui_plugin"
 "modules/kruddboard")
