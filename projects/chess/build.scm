; SPDX-License-Identifier: GPL-2.0-or-later
;;! The whole game is chess.scm — there is no C here any more, so this
;;! directory declares exactly two things: the source this build ships as its
;;! staged project, and the test that drives it.
;;!
;;! What a project may and may not declare is projects/README.md; what the two
;;! shipping declarations mean is resolve.scm's note on rz-codegen-kinds. The
;;! only thing left to say here is why CHESS is the one holding the staged slot,
;;! since exactly one project may and the contract does not care which: it is
;;! the sibling with a finished game behind it, and the boot path needs a
;;! project that plays. Nothing about the declaration knows that — it embeds
;;! under a fixed symbol core/engine.c evaluates without learning which project
;;! it got (#976), which is what keeps a game's name out of generic C.
((staged-project "chess.scm")

 (native-only
  ;;! The test drives chess.scm the way the engine does: project_host.c is
  ;;! compiled in so the (project ...) form registers on the real launcher
  ;;! registry, and game_load runs the host's own load path over a stand-in
  ;;! "scene" api. Hence game, subsystem_manager, game/project's and game/host's
  ;;! include roots, and third_party for the s7.h project_host.c registers its
  ;;! one primitive against.
  ;;!
  ;;! The camera, piece-mesh and army-material checks drive the real asset
  ;;! catalog: chess.scm registers its camera script, its six piece meshes and
  ;;! its two piece materials into it through script-define! / mesh-define! /
  ;;! material-define!, so proving the scene still binds them needs the real
  ;;! thing — which is also where material-define! finds the pbr shader it packs
  ;;! against — plus the entity-script driver that ticks a bound script and the
  ;;! mesh_script bridge that turns a bound mesh source into geometry. Hence
  ;;! asset_plugin, mesh_script, and the two world/asset include roots.
  (executable "chess_test"
              (sources "chess_test.c"
                       (root "game/project/project_host.c")
                       (root "world/entity/scene_script.c")
                       (root "world/entity/entity_script.c")
                       (root "world/entity/entity.c"))
              (private "." (root "abi/include") (root "world/entity/include") (root "base/math/include")
                       (root "world/entity")
                       (root "core/include")
                       (root "base/memory/include")
                       (root "world/asset")
                       (root "world/asset/include")
                       (root "game/host/include")
                       (root "game/project/include")
                       (raw "../third_party")
                       (raw "${generated}"))
              (link "asset_plugin" "mesh_script" "script" "memory" "log"
                    "game" "subsystem_manager"))
  (test "chess" "chess_test")))
