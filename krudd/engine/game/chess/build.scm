; SPDX-License-Identifier: GPL-2.0-or-later
;;! The game's scene and rules, embedded into the s7 image and evaluated by
;;! chess.c — the Scheme is the game, the C is the plugin shell around it.
((embed "scene.scm" "chess_scene_scm.h" "CHESS_SCENE_SCM")
 (embed "rules.scm" "chess_rules_scm.h" "CHESS_RULES_SCM")

 (library "chess_game"
   (sources "chess.c")
   ;;! No public surface of its own — chess.c carries no header, and the game
   ;;! is reached through the registry it registers with rather than by
   ;;! including anything from here. What is listed is the re-export of what
   ;;! the plugin's own signatures are written in.
   (public (root "abi/include") (root "world/entity/include") (root "base/math/include") (root "core/include") (root "game/host/include"))
   (private (raw "${generated}"))
   (link "subsystem_manager" "script" "game"))
 (native-only
  ;;! The camera check drives the real asset catalog: rules.scm registers its
  ;;! camera script into it through script-define!, so proving the Camera entity
  ;;! still binds and eases needs the real thing, plus the entity-script driver
  ;;! that ticks a bound script. Hence asset_plugin and (root "world/asset").
  (executable "chess_test"
              (sources "chess_test.c"
                       (root "world/entity/scene_script.c")
                       (root "world/entity/entity_script.c")
                       (root "world/entity/entity.c"))
              (private "." (root "abi/include") (root "world/entity/include") (root "base/math/include")
                       (root "world/entity")
                       (root "core/include")
                       (root "base/memory/include")
                       (root "world/asset")
                       (raw "../third_party")
                       (raw "${generated}"))
              (link "asset_plugin" "script" "memory" "log"))
  (test "chess" "chess_test")))
