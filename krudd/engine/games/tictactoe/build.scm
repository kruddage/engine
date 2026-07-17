; SPDX-License-Identifier: GPL-2.0-or-later
((library "tictactoe_game"
	(sources "tictactoe.c")
	(public "." (root "abi") (root "core/include"))
	(private (raw "${generated}"))
	(link "subsystem_manager")))
