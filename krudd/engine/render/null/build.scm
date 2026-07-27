; SPDX-License-Identifier: GPL-2.0-or-later
;;! renderer_null.h is the one header here anything outside includes — the
;;! headless backend both the frame-graph and scene-renderer tests drive their
;;! passes against — so it is the module's surface. renderer_null.c stays
;;! private: the backend is reached through the vtable it registers.
((library "renderer_null"
   (sources "renderer_null.c")
   (public "include")
   (private (raw "${generated}"))
   (link "log" "subsystem" "subsystem_manager"))
 (native-only
  (executable "renderer_null_test"
              (sources "renderer_null_test.c")
              (private "." (raw "${generated}"))
              (link "renderer_null" "log" "subsystem_manager"))
  (test "renderer_null" "renderer_null_test")))
