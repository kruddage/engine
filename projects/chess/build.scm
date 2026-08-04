; SPDX-License-Identifier: GPL-2.0-or-later
;;! The whole game is chess.scm — there is no C here any more, so this
;;! directory declares three things: that the build ships the game, how the
;;! source reaches the test, and the test. The contract those declarations are
;;! drawn from is projects/README.md; what the two shipping declarations mean is
;;! resolve.scm's note on rz-codegen-kinds.
;;!
;;! Shipped, not staged. Chess held the staged slot while the bare URL had no
;;! project to open and the boot path wanted one that plays; since #1034 the
;;! bare URL opens `default` — the scene the renderer used to seed from C — and
;;! chess is a `?game=` away like every other shipped project, which costs it
;;! one fetch of this file and buys back the WASM image every visitor downloads.
;;! Being staged was never what made it reachable: `project-source` is, and any
;;! number of projects may declare one.
;;!
;;! The (embed ...) is for the test alone — CHESS_SCM is included by
;;! chess_test.c and by nothing else, so the test drives the real source with no
;;! filesystem under it and the shipped WASM module pays nothing for it. It used
;;! to read the staged embed instead, which was free while chess was the staged
;;! one and is not this file's to borrow now.
((project-source "chess.scm")
 (embed "chess.scm" "chess_scm.h" "CHESS_SCM")

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
