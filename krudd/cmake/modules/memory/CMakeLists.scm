; SPDX-License-Identifier: GPL-2.0-or-later
;
; memory.c calls the libc malloc family directly. On WASM the main module is
; linked -sMALLOC=mimalloc (see modules/core), so that libc — and this seam — is
; mimalloc; natively it is the platform libc. No separately linked allocator:
; one heap for the whole program.
((library "memory"
	(sources "memory.c")
	(public "include"))
 (native-only
	(executable "memory_test"
		(sources "memory_test.c")
		(link "memory"))
	(test "memory" "memory_test")))
