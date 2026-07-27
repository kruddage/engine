; SPDX-License-Identifier: GPL-2.0-or-later
((library "game"
   (sources "game.c")
   (public "include"))
 (native-only
  (executable "game_test"
              (sources "game_test.c" "game.c")
              (private "include"))
  (test "game" "game_test")))
