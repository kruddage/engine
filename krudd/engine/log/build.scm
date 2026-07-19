; SPDX-License-Identifier: GPL-2.0-or-later
((library "log"
   (sources "log.c" (root "core/ring_buf.c"))
   (public "include" (root "abi"))
   (private (root "core/include")))
 (native-only
  (executable "log_test"
              (sources "log_test.c")
              (link "log"))
  (test "log" "log_test")))
