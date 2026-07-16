; SPDX-License-Identifier: GPL-2.0-or-later
((library "mixer"
	(sources "mixer.c")
	(public "." (root "abi"))
	(link "m"))
 (native-only
	(executable "mixer_test"
		(sources "mixer_test.c")
		(private "." (root "abi"))
		(link "mixer" "memory" "m"))
	(test "mixer" "mixer_test")))
