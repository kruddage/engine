; SPDX-License-Identifier: GPL-2.0-or-later
((library "tictactoe_game"
	(sources "tictactoe.c")
	(public "." (root "abi") (root "core/include"))
	(private (raw "${generated}"))
	(link "subsystem_manager" "script"))
 (native-only
	(executable "tictactoe_test"
		(sources "tictactoe_test.c"
			(root "entity/scene_script.c")
			(root "entity/entity.c"))
		(private "." (root "abi")
			(root "entity")
			(root "core/include")
			(root "memory/include")
			(raw "../third_party")
			(raw "${generated}"))
		(link "script" "memory"))
	(test "tictactoe" "tictactoe_test")))
