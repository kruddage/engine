; SPDX-License-Identifier: GPL-2.0-or-later
((library "renderer_webgpu"
	(sources "renderer_webgpu.c")
	(private (raw "${generated}") (root "core/include"))
	(link "log" "subsystem" "subsystem_manager")))
