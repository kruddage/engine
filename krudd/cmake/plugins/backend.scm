; SPDX-License-Identifier: GPL-2.0-or-later
;
; Persistence seam: the Local provider (backend_record) plus the branching
; host (#213) and the content-addressed store it owns (#214/#215/#216). The
; branch/CAS sources compile directly into the backend_plugin side module and
; into the native branch tests — plugins are self-contained side modules and
; cannot link another module's symbols, so the sources are named here rather
; than linked as library targets. The CAS backing is target-specific:
; EMSCRIPTEN uses the IndexedDB binding (cas_idb.c), which makes
; branches/snapshots durable across a reload; every other target (native,
; unit tests) keeps the in-memory binding (cas_mem.c), since cas_idb.c is
; browser-only.
((library "backend_record"
	(sources "backend_record.c")
	(public "." (root "plugins/include")))
 (library "backend_plugin"
	(sources "backend_plugin.c")
	(public "." (root "modules/core/include") (root "plugins/include")
		(root "plugins/cas") (root "plugins/branch")
		(root "plugins/snapshot"))
	(link "backend_record" "log" "subsystem" "subsystem_manager"))
 (native-only
	(executable "backend_record_test" (sources "backend_record_test.c")
		(link "backend_record"))
	(test "backend_record" "backend_record_test")
	; Native gate test: links the host + store directly and drives the
	; vtable.
	(executable "backend_branch_test"
		(sources "branch_host_test.c" "branch_host.c" "branch_serialize.c"
			"branch_ingest.c" (root "plugins/cas/cas.c")
			(root "plugins/cas/cas_mem.c")
			(root "plugins/branch/branch.c")
			(root "plugins/snapshot/snapshot.c"))
		(private "." (root "plugins/include") (root "plugins/cas")
			(root "plugins/branch") (root "plugins/snapshot")
			(root "modules/core/include") (root "modules/memory/include"))
		; subsystem_manager: the serialize/ingest glue reaches the world &
		; catalog through vtables looked up on the manager (get_api), so the
		; native test links it even though those subsystems aren't
		; registered in-test.
		(link "memory" "subsystem_manager"))
	(test "backend_branch" "backend_branch_test")
	; End-to-end round-trip test: registers fake scene/asset/asset_mut
	; subsystems on a real manager and drives serialize -> fork -> switch ->
	; ingest, proving the live world+catalog swap and the manifest path
	; round-trip natively (the browser proof-of-life's data-runtime layer).
	(executable "backend_branch_roundtrip_test"
		(sources "branch_roundtrip_test.c" "branch_host.c"
			"branch_serialize.c" "branch_ingest.c"
			(root "plugins/cas/cas.c") (root "plugins/cas/cas_mem.c")
			(root "plugins/branch/branch.c")
			(root "plugins/snapshot/snapshot.c"))
		(private "." (root "plugins/include") (root "plugins/cas")
			(root "plugins/branch") (root "plugins/snapshot")
			(root "modules/core/include") (root "modules/memory/include"))
		(link "memory" "subsystem_manager"))
	(test "backend_branch_roundtrip" "backend_branch_roundtrip_test"))
 (side-module "backend_plugin"
	(includes (current) (root "modules/core/include")
		(root "plugins/include") (root "plugins/cas")
		(root "plugins/branch") (root "plugins/snapshot"))
	(sources (current "backend_plugin.c") (current "backend_record.c")
		(current "branch_host.c") (current "branch_serialize.c")
		(current "branch_ingest.c") (root "plugins/cas/cas.c")
		(root "plugins/cas/cas_idb.c") (root "plugins/branch/branch.c")
		(root "plugins/snapshot/snapshot.c"))
	(depends (current "backend_plugin.c") (current "backend_record.c")
		(current "backend_record.h") (current "branch_host.c")
		(current "branch_host.h") (current "branch_serialize.c")
		(current "branch_serialize.h") (current "branch_ingest.c")
		(current "branch_ingest.h") (current "branch_manifest.h")
		(root "plugins/cas/cas.c") (root "plugins/cas/cas_idb.c")
		(root "plugins/cas/cas_idb.h") (root "plugins/branch/branch.c")
		(root "plugins/snapshot/snapshot.c")
		(root "plugins/include/backend_api.h")
		(root "plugins/include/branch_api.h"))))
