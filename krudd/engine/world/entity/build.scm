; SPDX-License-Identifier: GPL-2.0-or-later
((library "entity_plugin"
   (sources "entity.c" "entity_plugin.c" "entity_script.c"
            "scene_script.c" "scene_edit.c")
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
                       (root "world/edit")
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
              (private "." "include" (root "abi") (root "base/math/include")
                       (root "core/include")
                       (root "base/memory/include")
                       (root "world/asset/include")
                       (raw "../third_party"))
              (link "script" "memory"))
  (test "entity_script" "entity_script_test")
  (executable "scene_script_test"
              (sources "scene_script_test.c" "scene_script.c" "entity.c")
              (private "." "include" (root "abi") (root "base/math/include")
                       (root "core/include")
                       (root "base/memory/include")
                       (raw "../third_party"))
              (link "script" "memory"))
  (test "scene_script" "scene_script_test")))
