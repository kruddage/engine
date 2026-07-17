; SPDX-License-Identifier: GPL-2.0-or-later
((library "game"
	(sources "game.c")
	(public "."))
 (native-only
	(executable "game_test"
		(sources "game_test.c" "game.c")
		(private "."))
	(test "game" "game_test")))
