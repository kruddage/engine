; SPDX-License-Identifier: GPL-2.0-or-later
;;! The entity and scene script sources, embedded into the s7 image — the
;;! source-of-truth for the entity_script.c / scene_script.c bridges below.
((embed "entity_script.scm" "entity_script_scm.h" "ENTITY_SCRIPT_SCM")
 (embed "scene_script.scm" "scene_script_scm.h" "SCENE_SCRIPT_SCM")

 (library "entity_plugin"
   (sources "entity.c" "entity_plugin.c" "entity_script.c"
            "scene_script.c" "scene_edit.c")
   (public "include" (root "abi/include") (root "base/math/include")
           (root "core/include"))
   (private (raw "../third_party"))
   (link "memory" "subsystem_manager" "script"))
 (native-only
  (executable "entity_test"
              (sources "entity_test.c" "entity.c")
              (private "." "include" (root "abi/include") (root "base/math/include")
                       (root "core/include")
                       (root "base/memory/include"))
              (link "memory"))
  (test "entity" "entity_test")
  (executable "scene_edit_test"
              (sources "scene_edit_test.c" "scene_edit.c" "entity.c"
                       (root "world/edit/edit.c"))
              (private "." "include" (root "abi/include") (root "base/math/include")
                       (root "world/edit/include")
                       (root "core/include")
                       (root "base/memory/include"))
              (link "memory"))
  (test "scene_edit" "scene_edit_test")
  ;;! (root "world/asset/include") is for builtin_scripts.h — the entity-script source
  ;;! the asset seeder embeds, which this test asserts against so the two can
  ;;! never drift. It lives next to that seeder rather than in abi/, since it
  ;;! is asset content and not part of the plugin ABI.
  (executable "entity_script_test"
              (sources "entity_script_test.c" "entity_script.c" "entity.c")
              (private "." "include" (root "abi/include") (root "base/math/include")
                       (root "core/include")
                       (root "base/memory/include")
                       (root "world/asset/include")
                       (raw "../third_party"))
              (link "script" "memory"))
  (test "entity_script" "entity_script_test")
  (executable "scene_script_test"
              (sources "scene_script_test.c" "scene_script.c" "entity.c")
              (private "." "include" (root "abi/include") (root "base/math/include")
                       (root "core/include")
                       (root "base/memory/include")
                       (raw "../third_party"))
              (link "script" "memory"))
  (test "scene_script" "scene_script_test")
  ;;! script-define! against the REAL asset catalog rather than a stand-in: the
  ;;! catalog is what carries the static table of built-in declarations, so only
  ;;! the real one can prove a runtime-defined script needs no entry in it. Hence
  ;;! the asset_plugin link and (root "world/asset") for its private asset.h.
  (executable "script_define_test"
              (sources "script_define_test.c" "scene_script.c"
                       "entity_script.c" "entity.c")
              (private "." "include" (root "abi/include") (root "base/math/include")
                       (root "core/include")
                       (root "base/memory/include")
                       (root "world/asset")
                       (raw "../third_party"))
              (link "asset_plugin" "script" "memory" "log"))
  (test "script_define" "script_define_test")
  ;;! mesh-define!, the geometry twin, against the same real catalog and for the
  ;;! same reason. It also links the mesh_script bridge and takes
  ;;! (root "world/asset/include") for its header: proving a defined mesh renders
  ;;! identically to a seeded one means resolving both to real mesh_blobs, which
  ;;! is what mesh_script_generate does for the renderer and the picker.
  (executable "mesh_define_test"
              (sources "mesh_define_test.c" "scene_script.c" "entity.c")
              (private "." "include" (root "abi/include") (root "base/math/include")
                       (root "core/include")
                       (root "base/memory/include")
                       (root "world/asset")
                       (root "world/asset/include")
                       (raw "../third_party"))
              (link "asset_plugin" "mesh_script" "script" "memory" "log"))
  (test "mesh_define" "mesh_define_test")))
