; SPDX-License-Identifier: GPL-2.0-or-later
;;! The project test harness, as a library rather than as a paragraph every
;;! project copies into its own build.scm (#1035).
;;!
;;! What was here before this module: five directories each compiling in
;;! game/project's project_host.c and world/entity's scene_script.c,
;;! entity_script.c and entity.c, behind up to nine (private (root …)) include
;;! roots and seven (link …) names, plus forty lines of C apiece standing up the
;;! same stand-in "scene" vtable. Every one of those declarations was correct
;;! and none of them was interesting, which is the definition of something that
;;! belongs in one place. A consumer now writes (link "krudd_project_test") and
;;! includes one header; the include roots it needs to write a test at all ride
;;! in on the (public …) below, so there is no list here to keep in step with a
;;! list over there.
;;!
;;! NATIVE-ONLY, and that is the load-bearing word in this file. A harness is
;;! for tests, and the WASM image is what every visitor downloads — so this
;;! library must never be reachable from `index`. native-only is what says so:
;;! rz-spec-targets and ninja-emit-form both descend into it, so the library is
;;! declared, resolved and tier-checked exactly like any other, but the wasm
;;! emitter only ever renders the link closure of `index`, which this is not in
;;! and (nothing linking it being in the image) cannot get into.
;;!
;;! The four engine sources are compiled IN rather than linked, which is what
;;! the harness has always done and is not merely inherited. project_host.c is
;;! also `project_host`, and entity.c / scene_script.c / entity_script.c are
;;! also `entity_plugin` — but a test wants the world behind its OWN "scene"
;;! api, not behind the plugin that publishes the live one, and linking
;;! entity_plugin to reach three of its five sources is asking the linker not to
;;! pull the other two. Compiling them here keeps the harness's dependencies
;;! visible and its members its own. It is safe against duplicate symbols
;;! because this is an archive: nothing that links it also links `project_host`
;;! or `entity_plugin`, and if something ever does, the fix is at that link and
;;! not here.
;;!
;;! The (link …) list is what a consumer inherits, so it carries the fat half's
;;! names too — asset_plugin for the real catalog project_test_catalog.c stands
;;! up, and mesh_script for the bridge a bound mesh source resolves to geometry
;;! through, which a project test reading geometry back calls itself. Neither
;;! costs a lean test anything: an archive is pulled member by member, so a
;;! binary that never calls project_test_init_with_catalog pulls no member of
;;! either. That is the same property the catalog half's whole design rests on
;;! — see the note in include/project_test/project_test.h.
;;!
;;! (public …) lists more than this library's own headers on purpose: it is the
;;! surface a project test writes against — the world it reads, the launcher it
;;! loads through, the project host it evaluates by. Publishing them here is
;;! what lets a consumer declare no include roots at all. The private roots are
;;! the compiled-in sources' own needs: world/asset and world/entity for the two
;;! private headers the catalog half includes (asset.h, entity_script.h),
;;! ../third_party for s7.h, and ${generated} for the embedded Scheme sources
;;! project_host.c and scene_script.c evaluate.
((native-only
  (library "krudd_project_test"
    (sources "project_test.c" "project_test_catalog.c"
             (root "game/project/project_host.c")
             (root "world/entity/scene_script.c")
             (root "world/entity/entity_script.c")
             (root "world/entity/entity.c"))
    (public "include" (root "abi/include") (root "base/math/include")
            (root "core/include") (root "world/entity/include")
            (root "game/host/include") (root "game/project/include"))
    (private "." (root "world/asset") (root "world/entity")
             (raw "../third_party") (raw "${generated}"))
    (link "asset_plugin" "mesh_script" "script" "subsystem_manager"
          "game" "log" "memory"))))
