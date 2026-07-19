; SPDX-License-Identifier: GPL-2.0-or-later
((library "asset_plugin"
   (sources "asset_plugin.c" "asset_edit.c")
   (public "." (root "abi"))
   (link "log" "memory" "subsystem" "subsystem_manager" "m"))
 ;;! The mesh-script bridge (source -> mesh_blob) as a shared library, so both the
 ;;! renderer (upload) and the viewport bridge (click-to-pick raycast) resolve one
 ;;! copy of mesh_script_generate rather than each compiling the source (which
 ;;! would duplicate the symbol in the single WASM module).
 (library "mesh_script"
   (sources "mesh_script.c")
   (public "." (root "abi"))
   (private (root "core/include") (raw "../third_party"))
   (link "script"))
 ;;! The texture-script bridge (source -> texture_blob) as a shared library, the
 ;;! pixel twin of mesh_script: the renderer bakes an ASSET_TYPE_TEXTURE asset
 ;;! through one copy of texture_script_generate rather than each consumer
 ;;! compiling the source into the single WASM module.
 (library "texture_script"
   (sources "texture_script.c")
   (public "." (root "abi"))
   (private (root "core/include") (raw "../third_party"))
   (link "script"))
 ;;! The sound-script bridge (source -> sound_blob) as a shared library, the
 ;;! audio twin of texture_script: a consumer bakes an ASSET_TYPE_SOUND asset
 ;;! through one copy of sound_script_generate rather than each compiling the
 ;;! source into the single WASM module.
 (library "sound_script"
   (sources "sound_script.c")
   (public "." (root "abi"))
   (private (root "core/include") (raw "../third_party"))
   (link "script"))
 (native-only
  (executable "asset_test" (sources "asset_test.c")
              (link "asset_plugin" "log" "memory"))
  (test "asset" "asset_test")
  (executable "asset_codec_test" (sources "asset_codec_test.c")
              (link "asset_plugin" "log" "memory"))
  (test "asset_codec" "asset_codec_test")
  (executable "asset_api_test" (sources "asset_api_test.c")
              (link "asset_plugin" "log" "memory"))
  (test "asset_api" "asset_api_test")
  (executable "asset_mut_test" (sources "asset_mut_test.c")
              (link "asset_plugin" "log" "memory"))
  (test "asset_mut" "asset_mut_test")
  (executable "asset_edit_test"
              (sources "asset_edit_test.c" (root "edit/edit.c"))
              (private (root "edit"))
              (link "asset_plugin" "log" "memory"))
  (test "asset_edit" "asset_edit_test")
  (executable "asset_shader_test" (sources "asset_shader_test.c")
              (link "asset_plugin" "log" "memory"))
  (test "asset_shader" "asset_shader_test")
  (executable "asset_mesh_test" (sources "asset_mesh_test.c")
              (link "asset_plugin" "log" "memory"))
  (test "asset_mesh" "asset_mesh_test")

  (executable "mesh_script_test"
              (sources "mesh_script_test.c")
              (private "." (root "abi") (raw "../third_party"))
              (link "mesh_script" "script" "memory" "log"))
  (test "mesh_script" "mesh_script_test")

  (executable "texture_script_test"
              (sources "texture_script_test.c")
              (private "." (root "abi") (raw "../third_party"))
              (link "texture_script" "script" "memory" "log"))
  (test "texture_script" "texture_script_test")

  (executable "sound_script_test"
              (sources "sound_script_test.c")
              (private "." (root "abi") (raw "../third_party"))
              (link "sound_script" "script" "memory" "log"))
  (test "sound_script" "sound_script_test")))
