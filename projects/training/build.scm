; SPDX-License-Identifier: GPL-2.0-or-later
;;! The whole room is training.scm. Like every project it is Scheme and no C, so
;;! this directory declares three things: that the build ships the room, how the
;;! source reaches the test, and the test. The contract those declarations are
;;! drawn from is projects/README.md.
;;!
;;! Shipped, not staged, and that is the only choice this file makes: shipped
;;! and booted-into are different questions and only the second is scarce.
;;! `default` holds the staged slot. The (embed ...) is for the test alone —
;;! TRAINING_SCM is included by training_test.c and by nothing else, so the test
;;! drives the real source with no filesystem under it and the shipped WASM
;;! module pays nothing for it.
((project-source "training.scm")
 (embed "training.scm" "training_scm.h" "TRAINING_SCM")

 (native-only
  ;;! The harness is game/project's own: project_host.c compiled in so the
  ;;! (project ...) form registers on the real launcher registry, and game_load
  ;;! driving the host's load path over a stand-in "scene" api. Deliberately
  ;;! LEANER than chess's — no asset catalog, no mesh_script, no entity_script.
  ;;! The room registers two materials and binds no entity script, and an
  ;;! unbound material still spawns every entity its scene declares, so the
  ;;! geometry this test is about is fully readable without a catalog behind it.
  ;;! Same shape as game/project's own project_host_test for the same reason.
  (executable "training_test"
              (sources "training_test.c"
                       (root "game/project/project_host.c")
                       (root "world/entity/scene_script.c")
                       (root "world/entity/entity.c"))
              (private "." (root "abi/include") (root "core/include")
                       (root "world/entity/include") (root "base/math/include")
                       (root "base/memory/include") (root "game/host/include")
                       (root "game/project/include")
                       (raw "../third_party") (raw "${generated}"))
              (link "script" "subsystem_manager" "game" "log" "memory"))
  (test "training" "training_test")))
