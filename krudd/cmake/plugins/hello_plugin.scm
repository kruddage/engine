; SPDX-License-Identifier: GPL-2.0-or-later
;
; Minimal example plugin — a side module and nothing else.
((side-module "hello_plugin"
	(target "hello_plugin")
	(includes (root "modules/core/include"))
	(sources (current "hello_plugin.c"))
	(depends (current "hello_plugin.c"))))
