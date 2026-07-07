; SPDX-License-Identifier: GPL-2.0-or-later
;
; Content-addressed copy-on-write store (#214) — the substrate-agnostic core.
; cas.c is the addressing/manifest logic; cas_mem.c is the native/test backing
; (the IndexedDB backing is browser-only).
((library "cas"
	(sources "cas.c")
	(public "." (root "plugins/include")))
 (native-only
	(executable "cas_test"
		(sources "cas_test.c" "cas_mem.c")
		(private "." (root "plugins/include")
			(root "modules/memory/include"))
		(link "cas" "memory"))
	(test "cas" "cas_test")))
