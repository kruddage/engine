; SPDX-License-Identifier: GPL-2.0-or-later
;
; Structured logging with level filtering and ring-buffer history. The ring
; buffer itself lives in modules/core and compiles straight in.
((library "log"
	(sources "log.c" (root "modules/core/ring_buf.c"))
	(public "include" (root "modules/include"))
	(private (root "modules/core/include")))
 (native-only
	(executable "log_test"
		(sources "log_test.c")
		(link "log"))
	(test "log" "log_test")))
