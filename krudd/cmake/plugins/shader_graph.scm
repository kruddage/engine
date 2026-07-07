; SPDX-License-Identifier: GPL-2.0-or-later
((library "shader_graph_plugin"
	(sources "shader_nodes.c" "glsl_backend.c" "shader_graph.c")
	(public "." (root "plugins/include") (root "modules/core/include"))
	(link "memory" "subsystem_manager"))
 (native-only
	(executable "shader_graph_test"
		(sources "shader_graph_test.c")
		(link "shader_graph_plugin" "vscript_plugin" "asset_plugin"
			"log" "memory" "subsystem_manager"))
	(test "shader_graph" "shader_graph_test"))
 (side-module "shader_graph"
	(includes (current) (root "modules/core/include")
		(root "plugins/include"))
	(sources (current "shader_nodes.c") (current "glsl_backend.c")
		(current "shader_graph.c"))
	(depends (current "shader_nodes.c") (current "glsl_backend.c")
		(current "shader_graph.c"))))
