; SPDX-License-Identifier: GPL-2.0-or-later
;
; Engine heartbeat — init/tick/shutdown, subsystem manager, plugin loader, and
; the "index" executable that becomes index.html/.js/.wasm under Emscripten.
;
; Two chunks stay (verbatim ...): the configure_file() calls (no form models
; template expansion yet) and the EMSCRIPTEN-only block that bolts WASM-specific
; sources, target properties and link flags onto the already-declared "index"
; target — a one-off shape no other directory needs, so it isn't worth a form.
((verbatim "configure_file(version.h.in version.h)
configure_file(shell.html.in ${CMAKE_CURRENT_BINARY_DIR}/shell.html @ONLY)")

 (library "subsystem"
	(sources "subsystem.c")
	(public "include"))

 (library "subsystem_manager"
	(sources "subsystem_manager.c")
	(public "include"))

 (library "plugin_loader"
	(sources "plugin_loader.c")
	(public "include")
	(private (root "modules/log/include") (root "plugins/include")))

 (executable "index"
	(sources "engine.c")
	(private "include" (raw "${CMAKE_CURRENT_BINARY_DIR}")
		(root "plugins/include"))
	(link "subsystem" "subsystem_manager" "log" "memory" "plugin_loader"))

 (native-only
	(executable "subsystem_test"
		(sources "subsystem_test.c")
		(link "subsystem"))
	(test "subsystem" "subsystem_test")

	(executable "subsystem_manager_test"
		(sources "subsystem_manager_test.c")
		(link "subsystem_manager"))
	(test "subsystem_manager" "subsystem_manager_test")

	(executable "plugin_loader_test"
		(sources "plugin_loader_test.c")
		(link "plugin_loader"))
	(test "plugin_loader" "plugin_loader_test"))

 (verbatim "if(EMSCRIPTEN)
	target_sources(index PRIVATE plugin_abi.c)
	set_target_properties(index PROPERTIES
		SUFFIX \".html\"
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
		LINKER_LANGUAGE CXX
	)
	target_link_options(index PRIVATE
		-sENVIRONMENT=web
		-sALLOW_MEMORY_GROWTH=1
		# Emscripten 6.0.2 flipped GROWABLE_ARRAYBUFFERS to default =1, which
		# backs the heap with a real resizable ArrayBuffer. Web APIs reject
		# views over a resizable buffer — Firefox's crypto.getRandomValues
		# (reached at startup via WASI random_get) throws \"Argument 1 can't be
		# a resizable ArrayBuffer or ArrayBufferView\". Opting out keeps memory
		# growth (copy into a fresh plain ArrayBuffer on grow), which the Web
		# Crypto API accepts.
		-sGROWABLE_ARRAYBUFFERS=0
		# mimalloc is the one allocator: libc, FETCH, the dynamic linker, every
		# side module, and modules/memory (which allocates through libc) all
		# share this single heap. Previously libc used emmalloc while the engine
		# used a separately-linked mimalloc — two allocators over one growable
		# heap, which corrupted on growth.
		-sMALLOC=mimalloc
		-sMAIN_MODULE=1
		-sFETCH=1
		--extern-pre-js ${CMAKE_CURRENT_SOURCE_DIR}/error_overlay.js
		--shell-file ${CMAKE_CURRENT_BINARY_DIR}/shell.html
		-sMAX_WEBGL_VERSION=2
		-sEXPORTED_FUNCTIONS=_main
	)
endif()"))
