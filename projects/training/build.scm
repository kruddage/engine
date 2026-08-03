; SPDX-License-Identifier: GPL-2.0-or-later
;;! The whole room is training.scm. Like every project it is Scheme and no C, so
;;! this directory declares three things: that the build ships the room, how the
;;! source reaches the test, and the test.
;;!
;;! (project-source ...) is what puts the room on the page: it copies the .scm
;;! into assets/ beside index.html and names it in assets/projects.json, which is
;;! the list the shell's Load Project control offers. Without it the room would
;;! build, test green and be unreachable — the page cannot list a directory over
;;! HTTP, so a project the build did not write down does not exist as far as the
;;! shell is concerned.
;;!
;;! Not (staged-project ...): that is the same thing PLUS embedding the source
;;! into the image under the fixed symbol core/engine.c boots from, and it is
;;! single-occupancy. Chess holds it (see projects/chess/build.scm) because the
;;! boot path wants a project that plays. Shipped and booted-into are different
;;! questions and only the second is scarce.
;;!
;;! The (embed ...) is for the test alone: TRAINING_SCM is included by
;;! training_test.c and by nothing else, so it lets the test drive the real
;;! source with no filesystem under it and costs the shipped WASM module nothing.
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
