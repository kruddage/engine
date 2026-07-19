; SPDX-License-Identifier: GPL-2.0-or-later
((library "demo_game"
   (sources "demo.c")
   (public "." (root "abi") (root "core/include") (root "game"))
   (private (raw "${generated}"))
   (link "subsystem_manager" "game")))
