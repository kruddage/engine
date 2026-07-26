; SPDX-License-Identifier: GPL-2.0-or-later
((library "math"
   (sources "math.c" "camera.c" (raw "${generated}/math_gen.c"))
   (private (root "abi") (raw "${generated}"))
   (link "m"))
 (native-only
  (executable "math_test"
              (sources "math_test.c" "math.c" "camera.c"
                       (raw "${generated}/math_gen.c"))
              (private (root "abi"))
              (link "m"))
  (test "math" "math_test")))
