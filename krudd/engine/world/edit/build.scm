; SPDX-License-Identifier: GPL-2.0-or-later
((library "edit_plugin"
   (sources "edit.c" "edit_plugin.c")
   (public "include" (root "abi/include") (root "core/include"))
   (link "subsystem_manager"))
 (native-only
  (executable "edit_test"
              (sources "edit_test.c" "edit.c")
              (private "include" (root "abi/include"))
              (link "memory"))
  (test "edit" "edit_test")))
