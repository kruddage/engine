; SPDX-License-Identifier: GPL-2.0-or-later
;
; Per-branch auto-snapshots + restore (#216) over the branch model (#215) and
; the CAS (#214).
((library "snapshot"
	(sources "snapshot.c")
	(public "." (root "plugins/include") (root "plugins/cas")
		(root "plugins/branch"))
	(link "branch" "cas"))
 (native-only
	(executable "snapshot_test"
		(sources "snapshot_test.c" "snapshot.c"
			(root "plugins/branch/branch.c")
			(root "plugins/cas/cas.c")
			(root "plugins/cas/cas_mem.c"))
		(private "." (root "plugins/include") (root "plugins/cas")
			(root "plugins/branch")
			(root "modules/memory/include"))
		(link "memory"))
	(test "snapshot" "snapshot_test")))
