; SPDX-License-Identifier: GPL-2.0-or-later
;;! The entity and scene script sources, embedded into the s7 image — the
;;! source-of-truth for the entity_script.c / scene_script.c bridges below.
((embed "entity_script.scm" "entity_script_scm.h" "ENTITY_SCRIPT_SCM")
 (embed "scene_script.scm" "scene_script_scm.h" "SCENE_SCRIPT_SCM")

 (library "entity_plugin"
   (sources "entity.c" "entity_plugin.c" "entity_script.c"
            "scene_script.c" "scene_edit.c" "scene_save.c")
   (public "include" (root "abi") (root "base/math/include")
           (root "core/include"))
   (private (raw "../third_party"))
   (link "memory" "subsystem_manager" "script"))
 (native-only
  (executable "entity_test"
              (sources "entity_test.c" "entity.c")
              (private "." "include" (root "abi") (root "base/math/include")
                       (root "core/include")
                       (root "base/memory/include"))
              (link "memory"))
  (test "entity" "entity_test")
  (executable "scene_edit_test"
              (sources "scene_edit_test.c" "scene_edit.c" "entity.c"
                       (root "world/edit/edit.c"))
              (private "." "include" (root "abi") (root "base/math/include")
                       (root "world/edit/include")
                       (root "core/include")
                       (root "base/memory/include"))
              (link "memory"))
  (test "scene_edit" "scene_edit_test")
  ;;! The writer only. Whether what it writes loads back is scene_script_test's
  ;;! assertion, because that test owns the reader and the s7 image it needs —
  ;;! splitting them means a broken round-trip names the half that broke.
  (executable "scene_save_test"
              (sources "scene_save_test.c" "scene_save.c" "entity.c")
              (private "." "include" (root "abi") (root "base/math/include")
                       (root "core/include")
                       (root "base/memory/include"))
              (link "memory"))
  (test "scene_save" "scene_save_test")
  ;;! (root "world/asset/include") is for builtin_scripts.h — the entity-script source
  ;;! the asset seeder embeds, which this test asserts against so the two can
  ;;! never drift. It lives next to that seeder rather than in abi/, since it
  ;;! is asset content and not part of the plugin ABI.
  (executable "entity_script_test"
              (sources "entity_script_test.c" "entity_script.c" "entity.c")
              (private "." "include" (root "abi") (root "base/math/include")
                       (root "core/include")
                       (root "base/memory/include")
                       (root "world/asset/include")
                       (raw "../third_party"))
              (link "script" "memory"))
  (test "entity_script" "entity_script_test")
  (executable "scene_script_test"
              (sources "scene_script_test.c" "scene_script.c" "entity.c"
                       "scene_save.c")
              (private "." "include" (root "abi") (root "base/math/include")
                       (root "core/include")
                       (root "base/memory/include")
                       (raw "../third_party"))
              (link "script" "memory"))
  (test "scene_script" "scene_script_test")))
