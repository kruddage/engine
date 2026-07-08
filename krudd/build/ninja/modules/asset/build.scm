; SPDX-License-Identifier: GPL-2.0-or-later
((library "asset_plugin"
	(sources "asset_plugin.c" "primitives_blob.c" "asset_edit.c"
		(raw "${generated}/primitives.scm.c"))
	(public "." (root "modules/include"))
	(private (raw "${generated}") (raw "../../third_party"))
	(link "log" "memory" "subsystem" "subsystem_manager" "script" "m"))
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
		(sources "asset_edit_test.c" (root "modules/edit_plugin/edit.c"))
		(private (root "modules/edit_plugin"))
		(link "asset_plugin" "log" "memory"))
	(test "asset_edit" "asset_edit_test")
	(executable "asset_shader_test" (sources "asset_shader_test.c")
		(link "asset_plugin" "log" "memory"))
	(test "asset_shader" "asset_shader_test")
	(executable "asset_mesh_test" (sources "asset_mesh_test.c")
		(link "asset_plugin" "log" "memory"))
	(test "asset_mesh" "asset_mesh_test")

	(library "primitives_ref"
		(sources "primitives.c")
		(public "." (root "modules/include")))
	(executable "primitive_test" (sources "primitive_test.c")
		(link "primitives_ref" "m"))
	(test "primitive" "primitive_test")

	(library "primitives_scheme"
		(sources "primitives_blob.c" (raw "${generated}/primitives.scm.c"))
		(public "." (root "modules/include"))
		(private (raw "${generated}") (raw "../../third_party"))
		(link "script"))
	(executable "primitive_scheme_test" (sources "primitive_test.c")
		(link "primitives_scheme" "m"))
	(test "primitive_scheme" "primitive_scheme_test")))
