; SPDX-License-Identifier: GPL-2.0-or-later
;;! The project image — the (project ...) form and the host's Scheme half —
;;! embedded into the s7 image the way every other DSL source is, so a build
;;! with no filesystem still carries it. project_host.c evaluates it at init and
;;! calls into it by name.
((embed "project.scm" "project_scm.h" "PROJECT_SCM")

 ;;! A host, not a game: it registers no launcher entry of its own and knows no
 ;;! game's name. abi/include is for entity_api.h — the vtable it reaches the
 ;;! live world through — and third_party for s7.h, since the one primitive it
 ;;! registers takes a Scheme procedure.
 (library "project_host"
   (sources "project_host.c")
   (public "include")
   (private (root "abi/include") (raw "../third_party") (raw "${generated}"))
   (link "script" "subsystem_manager" "game" "log"))
 (native-only
  ;;! Drives a project end to end from a source string: the real s7 image, the
  ;;! real launcher registry, and a real world behind a stand-in "scene" api.
  ;;! All of which is now game/project_test's, so this declaration is one link
  ;;! and nothing else — no compiled-in engine sources and no include roots
  ;;! (#1035). The harness is packaged where it can be shared, and the module
  ;;! that owns the host it drives is the first thing to prove it, ahead of the
  ;;! projects that will follow it there.
  ;;!
  ;;! The LEAN half: project_test_init rather than the catalog one, because a
  ;;! project's mesh and material paths resolving to "unbound" still spawns
  ;;! every entity its scene declares, and what this test is about is the host,
  ;;! not the assets.
  (executable "project_host_test"
              (sources "project_host_test.c")
              (link "krudd_project_test"))
  (test "project_host" "project_host_test")))
