; SPDX-License-Identifier: GPL-2.0-or-later
;;! math.scm is the source-of-truth for the scalar helpers: its (define-c-fn)
;;! bodies are lowered to the C compiled into the library below.
((emit-math-module "math.scm" "math_gen.c")

 (library "math"
   (sources "math.c" "camera.c" (raw "${generated}/math_gen.c"))
   (public "include")
   (private (raw "${generated}"))
   (link "m"))
 (native-only
  (executable "math_test"
              (sources "math_test.c" "math.c" "camera.c"
                       (raw "${generated}/math_gen.c"))
              (private "include")
              (link "m"))
  (test "math" "math_test")))
