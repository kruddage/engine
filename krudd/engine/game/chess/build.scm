; SPDX-License-Identifier: GPL-2.0-or-later
;;! The game's scene and rules, embedded into the s7 image and evaluated by
;;! chess.c — the Scheme is the game, the C is the plugin shell around it.
((embed "scene.scm" "chess_scene_scm.h" "CHESS_SCENE_SCM")
 (embed "rules.scm" "chess_rules_scm.h" "CHESS_RULES_SCM")

 (library "chess_game"
   (sources "chess.c")
   (public "." (root "abi") (root "world/entity/include") (root "base/math/include") (root "core/include") (root "game/host"))
   (private (raw "${generated}"))
   (link "subsystem_manager" "script" "game"))
 (native-only
  (executable "chess_test"
              (sources "chess_test.c"
                       (root "world/entity/scene_script.c")
                       (root "world/entity/entity.c"))
              (private "." (root "abi") (root "world/entity/include") (root "base/math/include")
                       (root "world/entity")
                       (root "core/include")
                       (root "base/memory/include")
                       (raw "../third_party")
                       (raw "${generated}"))
              (link "script" "memory"))
  (test "chess" "chess_test")))
