; SPDX-License-Identifier: GPL-2.0-or-later
((native-only
  (executable "math_test"
              (sources "math_test.c" "math.c" "camera.c"
                       (raw "${generated}/math_gen.c"))
              (private (root "abi"))
              (link "m"))
  (test "math" "math_test")))
