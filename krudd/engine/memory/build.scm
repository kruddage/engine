; SPDX-License-Identifier: GPL-2.0-or-later
((library "memory"
	(sources "memory.c")
	(public "include"))
 (native-only
	(executable "memory_test"
		(sources "memory_test.c")
		(link "memory"))
	(test "memory" "memory_test")))
