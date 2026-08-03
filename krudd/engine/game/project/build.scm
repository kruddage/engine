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
  ;;! real launcher registry, and a real world behind a stand-in "scene" api —
  ;;! which is why it compiles world/entity's scene_script.c and entity.c in
  ;;! rather than linking a plugin. No GPU and no asset catalog: a project's
  ;;! mesh/material paths resolve to "unbound", which still spawns every entity
  ;;! its scene declares.
  (executable "project_host_test"
              (sources "project_host_test.c" "project_host.c"
                       (root "world/entity/scene_script.c")
                       (root "world/entity/entity.c"))
              (private "." "include" (root "abi/include") (root "core/include")
                       (root "world/entity/include") (root "base/math/include")
                       (root "base/memory/include") (root "game/host/include")
                       (raw "../third_party") (raw "${generated}"))
              (link "script" "subsystem_manager" "game" "log" "memory"))
  (test "project_host" "project_host_test")))
