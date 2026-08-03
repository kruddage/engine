; SPDX-License-Identifier: GPL-2.0-or-later
;;! The whole show is carwash.scm — there is no C here, the same way there is
;;! none in game/chess. This directory declares the source this build serves and
;;! the test that drives it.
;;!
;;! (served-project ...) is "ship this project beside index.html": the source is
;;! copied into assets/ and named in the project index the shell's Load Project
;;! control reads, and nothing is compiled into the image. It is the declaration
;;! for every project after the first, since (staged-project ...) — which also
;;! embeds, so the page can boot straight into a game with no network round trip
;;! — is single-occupancy by construction and game/chess holds it. An attract
;;! mode is the right shape for the slot it does not have: it is what you put on
;;! when nobody has chosen anything, not what a page opens on.
;;!
;;! The (embed ...) beside it is the test's, not the engine's, which is why it
;;! sits inside (native-only ...). chess_test reaches its source through the
;;! STAGED_PROJECT_SCM the staged declaration already embeds; a served project
;;! embeds nothing, so a test that wants to hand the source to project_host_eval
;;! has to ask for the bytes itself. One file still, read two ways.
((served-project "carwash.scm")

 (native-only
  (embed "carwash.scm" "carwash_scm.h" "CARWASH_SCM")

  ;;! The test drives carwash.scm the way the engine does: project_host.c is
  ;;! compiled in so the (project ...) form registers on the real launcher
  ;;! registry, and game_load runs the host's own load path over a stand-in
  ;;! "scene" api. Hence game, subsystem_manager, game/project's and game/host's
  ;;! include roots, and third_party for the s7.h project_host.c registers its
  ;;! one primitive against.
  ;;!
  ;;! The camera, car and tool checks drive the real asset catalog: carwash.scm
  ;;! registers five scripts and nine materials into it through script-define! /
  ;;! material-define!, and the paint it repacks every frame is the point of the
  ;;! whole project — so proving the show is on screen needs the real thing,
  ;;! which is also where material-define! finds the pbr shader it packs against,
  ;;! plus the entity-script driver that ticks a bound script. Hence
  ;;! asset_plugin and the two world/asset include roots.
  (executable "carwash_test"
              (sources "carwash_test.c"
                       (root "game/project/project_host.c")
                       (root "world/entity/scene_script.c")
                       (root "world/entity/entity_script.c")
                       (root "world/entity/entity.c"))
              (private "." (root "abi/include") (root "world/entity/include")
                       (root "base/math/include")
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
  (test "carwash" "carwash_test")))
