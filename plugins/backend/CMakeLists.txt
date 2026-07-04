add_library(backend_record backend_record.c)
target_include_directories(backend_record PUBLIC .
	${CMAKE_SOURCE_DIR}/plugins/include)

# The branching host (#213) and the content-addressed store it owns (#214/#215/
# #216).  These sources compile INTO the backend side module (plugins are
# self-contained side modules — they cannot link another module's symbols), and
# into the native branch test.  Track A swaps cas_mem.c -> cas_idb.c under
# EMSCRIPTEN once the IndexedDB backing lands; until then the store is in-memory
# on every target.
set(BRANCH_HOST_SRCS
	${CMAKE_CURRENT_SOURCE_DIR}/branch_host.c
	${CMAKE_CURRENT_SOURCE_DIR}/branch_serialize.c
	${CMAKE_CURRENT_SOURCE_DIR}/branch_ingest.c
	${CMAKE_SOURCE_DIR}/plugins/cas/cas.c
	${CMAKE_SOURCE_DIR}/plugins/cas/cas_mem.c
	${CMAKE_SOURCE_DIR}/plugins/branch/branch.c
	${CMAKE_SOURCE_DIR}/plugins/snapshot/snapshot.c)
set(BRANCH_HOST_INCS
	${CMAKE_SOURCE_DIR}/plugins/cas
	${CMAKE_SOURCE_DIR}/plugins/branch
	${CMAKE_SOURCE_DIR}/plugins/snapshot)

add_library(backend_plugin backend_plugin.c)
target_include_directories(backend_plugin PUBLIC .
	${CMAKE_SOURCE_DIR}/modules/core/include
	${CMAKE_SOURCE_DIR}/plugins/include
	${BRANCH_HOST_INCS})
target_link_libraries(backend_plugin PRIVATE backend_record log subsystem subsystem_manager)

if(NOT EMSCRIPTEN)
	add_executable(backend_record_test backend_record_test.c)
	target_link_libraries(backend_record_test PRIVATE backend_record)
	add_test(NAME backend_record COMMAND backend_record_test)

	# Native gate test: links the host + store directly and drives the vtable.
	add_executable(backend_branch_test branch_host_test.c ${BRANCH_HOST_SRCS})
	target_include_directories(backend_branch_test PRIVATE .
		${CMAKE_SOURCE_DIR}/plugins/include
		${BRANCH_HOST_INCS}
		${CMAKE_SOURCE_DIR}/modules/core/include
		${CMAKE_SOURCE_DIR}/modules/memory/include)
	# subsystem_manager: the serialize/ingest glue reaches the world & catalog
	# through vtables looked up on the manager (get_api), so the native test
	# links it even though those subsystems aren't registered in-test.
	target_link_libraries(backend_branch_test PRIVATE memory subsystem_manager)
	add_test(NAME backend_branch COMMAND backend_branch_test)
endif()

if(EMSCRIPTEN)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/backend_plugin.wasm
		COMMAND ${CMAKE_C_COMPILER}
			-sSIDE_MODULE=1
			-O2
			-I${CMAKE_CURRENT_SOURCE_DIR}
			-I${CMAKE_SOURCE_DIR}/modules/core/include
			-I${CMAKE_SOURCE_DIR}/plugins/include
			-I${CMAKE_SOURCE_DIR}/plugins/cas
			-I${CMAKE_SOURCE_DIR}/plugins/branch
			-I${CMAKE_SOURCE_DIR}/plugins/snapshot
			-o ${CMAKE_BINARY_DIR}/backend_plugin.wasm
			${CMAKE_CURRENT_SOURCE_DIR}/backend_plugin.c
			${CMAKE_CURRENT_SOURCE_DIR}/backend_record.c
			${BRANCH_HOST_SRCS}
		DEPENDS
			${CMAKE_CURRENT_SOURCE_DIR}/backend_plugin.c
			${CMAKE_CURRENT_SOURCE_DIR}/backend_record.c
			${CMAKE_CURRENT_SOURCE_DIR}/backend_record.h
			${CMAKE_CURRENT_SOURCE_DIR}/branch_host.c
			${CMAKE_CURRENT_SOURCE_DIR}/branch_host.h
			${CMAKE_CURRENT_SOURCE_DIR}/branch_serialize.c
			${CMAKE_CURRENT_SOURCE_DIR}/branch_serialize.h
			${CMAKE_CURRENT_SOURCE_DIR}/branch_ingest.c
			${CMAKE_CURRENT_SOURCE_DIR}/branch_ingest.h
			${CMAKE_CURRENT_SOURCE_DIR}/branch_manifest.h
			${CMAKE_SOURCE_DIR}/plugins/cas/cas.c
			${CMAKE_SOURCE_DIR}/plugins/cas/cas_mem.c
			${CMAKE_SOURCE_DIR}/plugins/branch/branch.c
			${CMAKE_SOURCE_DIR}/plugins/snapshot/snapshot.c
			${CMAKE_SOURCE_DIR}/plugins/include/backend_api.h
			${CMAKE_SOURCE_DIR}/plugins/include/branch_api.h
		COMMENT "Building backend_plugin SIDE_MODULE"
	)
	add_custom_target(backend_plugin_wasm ALL
		DEPENDS ${CMAKE_BINARY_DIR}/backend_plugin.wasm
	)
	add_dependencies(index backend_plugin_wasm)
endif()
