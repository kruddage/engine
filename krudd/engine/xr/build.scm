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
((library "xr"
   (sources "xr_state.c" "xr_session.c")
   (public "include")
   (private (root "core/include"))
   (link "log" "math" "renderer_webgl"))
 (native-only
  (executable "xr_test"
              (sources "xr_test.c")
              (private ".")
              (link "xr"))
  (test "xr" "xr_test")))
