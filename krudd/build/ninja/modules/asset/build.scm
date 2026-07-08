; SPDX-License-Identifier: GPL-2.0-or-later
;
; Asset catalog — enumeration, mutation, codec registration; built-in primitive
; geometry (krudd/build/modules/primitives.scm, packed by primitives_blob.c).
((library "asset_plugin"
	;; Built-in geometry comes from krudd/build/modules/primitives.scm now:
	;; primitives_blob.c packs the Scheme-marshaled vertex/index arrays into a
	;; mesh_blob through the generated primitives.scm.c shim. Linking "script"
	;; drags in the s7 interpreter; ${generated} carries the shim and
	;; primitives_gen.h, and ../../third_party carries s7.h for the shim.
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
	(test "asset_mesh" "asset_mesh_test")

	;; One spec, two proofs (as md_parse does): the same primitive_test.c runs
	;; against the golden C reference (primitives.c) and the Scheme port
	;; (primitives.scm via primitives_blob.c + the generated shim), asserting
	;; both satisfy the same geometric invariants. primitives.c is no longer in
	;; any shipped build — asset_plugin uses the Scheme generator — but stays as
	;; the reference the port is checked against.
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
