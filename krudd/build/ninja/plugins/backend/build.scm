; SPDX-License-Identifier: GPL-2.0-or-later
;
; Persistence seam: the Local provider (backend_record) plus the plugin glue
; that registers the "backend" subsystem and bridges authored assets to
; IndexedDB.
((library "backend_record"
	(sources "backend_record.c")
	(public "." (root "plugins/include")))
 (library "backend_plugin"
	(sources "backend_plugin.c")
	(public "." (root "modules/core/include") (root "plugins/include"))
	(link "backend_record" "log" "subsystem" "subsystem_manager"))
 (native-only
	(executable "backend_record_test" (sources "backend_record_test.c")
		(link "backend_record"))
	(test "backend_record" "backend_record_test"))
 (side-module "backend_plugin"
	(includes (current) (root "modules/core/include")
		(root "plugins/include"))
	(sources (current "backend_plugin.c") (current "backend_record.c"))
	(depends (current "backend_plugin.c") (current "backend_record.c")
		(current "backend_record.h")
		(root "plugins/include/backend_api.h"))))
