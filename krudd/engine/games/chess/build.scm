; SPDX-License-Identifier: GPL-2.0-or-later
((library "chess_game"
	(sources "chess.c")
	(public "." (root "abi") (root "core/include") (root "game"))
	(private (raw "${generated}"))
	(link "subsystem_manager" "game"))
 (native-only
	(executable "chess_test"
		(sources "chess_test.c"
			(root "entity/scene_script.c")
			(root "entity/entity.c"))
		(private "." (root "abi")
			(root "entity")
			(root "core/include")
			(root "memory/include")
			(raw "../third_party")
			(raw "${generated}"))
		(link "script" "memory"))
	(test "chess" "chess_test")))
