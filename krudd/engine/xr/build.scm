; SPDX-License-Identifier: GPL-2.0-or-later
;;! include/xr/xr.h is the module's public surface: the session's state, the
;;! per-frame view list, and the two calls that enter and leave one. The
;;! browser glue behind it (xr_session.c's EM_JS) is private, and so is
;;! xr_bridge.h — the seam that glue calls back through, which the native test
;;! drives in its place.
;;!
;;! That split is why there are two sources rather than one. xr_state.c is the
;;! session state machine and the view-list bookkeeping, with no emscripten in
;;! it at all, so it builds and is tested natively — no browser, no headset,
;;! no GL context — the way renderer_webgl_test tests the declare-backbuffer
;;! bookkeeping the same way. xr_session.c is the half that only exists in a
;;! browser, kept as small as the WebXR API allows.
;;!
;;! The link on renderer_webgl is the #990 seam: a session names its layer's
;;! framebuffer as the backbuffer, which is a call into that backend's own
;;! public surface rather than through the gpu_api vtable (an external
;;! framebuffer name is a GLES concept with nothing to say to WebGPU). It is
;;! also what makes this module WebGL-only in the build and not just in a
;;! comment.
;;!
;;! scene_renderer is the #989 seam and arrives as an include path rather than
;;! a link, for the same reason core/include does: the definitions are in the
;;! WASM main module already — scene_renderer and xr are both (wasm-modules
;;! ...) of core's `index`, and engine.c's plugin table is what pulls
;;! scene_renderer in — so a link edge would add nothing there and would break
;;! the native build, where scene_renderer is wasm-only and has no archive to
;;! link. xr_test supplies its own definitions instead, which is the point of
;;! the seam: what this module hands the renderer each frame is then something
;;! a native run can read back and hold to account, rather than something only
;;! a headset can see.
((library "xr"
   (sources "xr_state.c" "xr_session.c")
   (public "include")
   (private (root "core/include")
            (root "render/scene_renderer/include"))
   (link "log" "math" "renderer_webgl"))
 (native-only
  (executable "xr_test"
              (sources "xr_test.c")
              (private "." (root "render/scene_renderer/include"))
              (link "xr"))
  (test "xr" "xr_test")))
