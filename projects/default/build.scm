; SPDX-License-Identifier: GPL-2.0-or-later
;;! The whole of it is default.scm, so this directory declares two things: the
;;! source this build ships as its staged project, and the test that drives it.
;;! What a project may and may not declare is projects/README.md; what the
;;! shipping declarations mean is resolve.scm's note on rz-codegen-kinds.
;;!
;;! Why DEFAULT holds the staged slot, since exactly one project may and the
;;! contract does not care which: staged is the project the bare URL boots into,
;;! and this is the project that exists to BE that. It is embedded so the page
;;! opens on a scene with no round trip, which is the whole of what staging buys
;;! — and it is the cheapest thing in the tree to embed, being one scene clause
;;! with no rules, no meshes and no materials of its own. Chess held the slot
;;! while the boot needed a project that plays (#976); since #1034 the boot
;;! needs a project that OPENS, chess is a `?game=` away like every other
;;! shipped project, and the demo that used to be seeded from C is what the site
;;! opens on. Nothing about the declaration knows any of that: it embeds under a
;;! fixed symbol core/engine.c evaluates without learning which project it got.
;;!
;;! No (embed ...) of its own. A staged project is already embedded — under
;;! STAGED_PROJECT_SCM, from the declaration below — so the test includes that
;;! header rather than asking the build for a second copy of the same bytes.
;;! Its siblings each declare one because they are shipped, not staged, and have
;;! no embed to borrow.
((staged-project "default.scm")

 (native-only
  ;;! The FAT harness — the one chess uses — and here it is the point
  ;;! rather than a cost. Every mesh, material
  ;;! and script this project names is a BUILT-IN: it defines none of its own,
  ;;! so what is worth testing is precisely that those catalog paths still
  ;;! resolve and still bind, which is unaskable without the real catalog behind
  ;;! it. Hence asset_plugin (which seeds the built-ins), mesh_script (the
  ;;! bridge a bound mesh source resolves through), and entity_script.c (the
  ;;! driver that ticks a bound behaviour script); the rest is game/project's
  ;;! own harness, exactly as the siblings assemble it.
  (executable "default_test"
              (sources "default_test.c"
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
  (test "default" "default_test")))
