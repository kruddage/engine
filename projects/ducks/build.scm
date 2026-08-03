; SPDX-License-Identifier: GPL-2.0-or-later
;;! The whole game is ducks.scm. Like every project it is Scheme and no C, so
;;! this directory declares two things: how the source reaches the test, and the
;;! test.
;;!
;;! (embed ...) rather than (staged-project ...): the staged slot is
;;! single-occupancy and chess holds it (see projects/chess/build.scm). What
;;! this booth needs is narrower — its source available to a native test with no
;;! filesystem under it — and that is what an embed is for. The generated
;;! DUCKS_SCM is included by ducks_test.c and by nothing else, so the game costs
;;! the shipped WASM module nothing; it is reached at runtime the way any
;;! unstaged project is, through the Load Project control.
((embed "ducks.scm" "ducks_scm.h" "DUCKS_SCM")

 (native-only
  ;;! The FAT harness, the one chess uses rather than the lean one training
  ;;! does, and for the reason chess needs it: this game's conveyor, gun pivot
  ;;! and crosshair are ENTITY SCRIPTS, and an entity script only runs if the
  ;;! catalog script-define! registered it into is really there. So the real
  ;;! asset catalog is compiled in (asset_plugin — also where material-define!
  ;;! finds the pbr shader it packs against), along with the entity-script
  ;;! driver that ticks a bound script and the mesh_script bridge that turns a
  ;;! bound mesh source into geometry. Hence the two world/asset include roots
  ;;! and the entity_script.c source; the rest is game/project's own harness.
  (executable "ducks_test"
              (sources "ducks_test.c"
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
  (test "ducks" "ducks_test")))
