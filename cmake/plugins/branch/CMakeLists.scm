; SPDX-License-Identifier: GPL-2.0-or-later
;
; Branch model + bootstrap + live-save + switch (#215) over the CAS (#214).
((library "branch"
	(sources "branch.c")
	(public "." (root "plugins/include") (root "plugins/cas"))
	(link "cas"))
 (native-only
	(executable "branch_test"
		(sources "branch_test.c" (root "plugins/cas/cas_mem.c"))
		(private "." (root "plugins/include") (root "plugins/cas")
			(root "modules/memory/include"))
		(link "branch" "cas" "memory"))
	(test "branch" "branch_test")))
