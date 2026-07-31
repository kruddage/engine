; SPDX-License-Identifier: GPL-2.0-or-later
((library "renderer_webgl"
	(sources "renderer_webgl.c")
	(private (raw "${generated}") (root "core/include"))
	(link "log" "memory" "subsystem" "subsystem_manager" "script")))
