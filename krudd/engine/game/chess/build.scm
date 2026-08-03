; SPDX-License-Identifier: GPL-2.0-or-later
;;! The whole game is chess.scm — there is no C here any more, so this
;;! directory declares exactly two things: the source embedded into the image,
;;! and the test that drives it.
;;!
;;! The embed is named for what the source IS to the build (the project this
;;! image ships staged) rather than for chess, because core/engine.c evaluates
;;! STAGED_PROJECT_SCM at boot without knowing which project it got — that is
;;! what keeps "the site boots straight to a playable board with no network
;;! round trip" (a non-goal of #976 to remove) from putting a game's name back
;;! into generic C. Exactly one directory may claim the name: a second embed
;;! writing the same generated header is a build error (resolve-check-codegen),
;;! so the staged slot cannot silently be taken twice. #984 layers loading a
;;! project off disk on top of this, with the staged one as the fallback.
((embed "chess.scm" "staged_project_scm.h" "STAGED_PROJECT_SCM")

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
