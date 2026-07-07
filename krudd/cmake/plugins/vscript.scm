; SPDX-License-Identifier: GPL-2.0-or-later
((library "vscript_plugin"
	(sources "vscript.c")
	(public "." (root "plugins/include") (root "modules/core/include"))
	(link "memory" "subsystem_manager"))
 (native-only
	(executable "vscript_test"
		(sources "vscript_test.c")
		(link "vscript_plugin" "asset_plugin" "log" "memory"
			"subsystem_manager"))
	(test "vscript" "vscript_test"))
 (side-module "vscript"
	(includes (current) (root "modules/core/include")
		(root "plugins/include"))
	(sources (current "vscript.c"))
	(depends (current "vscript.c"))))
