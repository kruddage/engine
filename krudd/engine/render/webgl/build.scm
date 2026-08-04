; SPDX-License-Identifier: GPL-2.0-or-later
;;! include/webgl/renderer_webgl.h is the module's public surface: the
;;! host-declared-backbuffer bookkeeping (webgl_declare_backbuffer and
;;! friends), which is plain state with no GL call in it, so it builds and is
;;! native-tested without a GL context — the render-pass logic that reads it
;;! stays private, reached only through the vtable this module registers.
((library "renderer_webgl"
   (sources "renderer_webgl.c")
   (public "include")
   (private (raw "${generated}") (root "core/include"))
   (link "log" "memory" "subsystem" "subsystem_manager" "script"))
 (native-only
  (executable "renderer_webgl_test"
              (sources "renderer_webgl_test.c")
              (private "." (raw "${generated}"))
              (link "renderer_webgl" "log" "memory" "subsystem_manager"))
  (test "renderer_webgl" "renderer_webgl_test")))
