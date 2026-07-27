; SPDX-License-Identifier: GPL-2.0-or-later
((library "particles"
   (sources "particles.c")
   (public "." (root "abi"))
   (private (raw "${generated}"))
   (link "m"))
 (native-only
  (executable "particles_test"
              (sources "particles_test.c" "particles.c")
              (private "." (raw "${generated}"))
              (link "m"))
  (test "particles" "particles_test")))
