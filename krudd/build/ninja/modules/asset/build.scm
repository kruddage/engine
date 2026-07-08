; SPDX-License-Identifier: GPL-2.0-or-later
;
; Asset catalog — enumeration, mutation, codec registration; built-in primitive
; geometry (primitives.c).
((library "asset_plugin"
	(sources "asset_plugin.c" "primitives.c" "asset_edit.c")
	(public "." (root "modules/include"))
	(link "log" "memory" "subsystem" "subsystem_manager" "m"))
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
	; Undo/redo recording for authored assets: drive the same record path
	; the plugin uses (asset_edit lives in asset_plugin) against the real
	; catalog, with the edit history linked directly.
	(executable "asset_edit_test"
		(sources "asset_edit_test.c" (root "modules/edit_plugin/edit.c"))
		(private (root "modules/edit_plugin"))
		(link "asset_plugin" "log" "memory"))
	(test "asset_edit" "asset_edit_test")
	(executable "asset_shader_test" (sources "asset_shader_test.c")
		(link "asset_plugin" "log" "memory"))
	(test "asset_shader" "asset_shader_test")
	(executable "asset_mesh_test" (sources "asset_mesh_test.c")
		(link "asset_plugin" "log" "memory"))
	(test "asset_mesh" "asset_mesh_test")))
