; SPDX-License-Identifier: GPL-2.0-or-later
((library "mixer"
	(sources "mixer.c")
	(public "." (root "abi"))
	(link "m"))
 ;;! The thread seam: mixer + a lock-free SPSC command ring between bake and
 ;;! render. Pure C, no browser deps, so it links into the native audio_core_test
 ;;! as well as the WASM backend below. (On the ScriptProcessorNode backend the
 ;;! ring is drained on the same thread it is filled — harmless, and it keeps one
 ;;! core shared with the AudioWorklet variant.)
 (library "audio_core"
	(sources "audio_core.c")
	(public "." (root "abi"))
	(link "mixer" "m"))
 ;;! The ScriptProcessorNode device backend (main-thread onaudioprocess ->
 ;;! audio_core -> mixer). Browser only; a native no-op stub keeps the tree
 ;;! compiling. Listed in the core executable's wasm-modules so it links into the
 ;;! single WASM module. Needs no special link flags — the whole point of this
 ;;! variant over the AudioWorklet backend.
 (library "audio_scriptnode"
	(sources "audio_scriptnode.c")
	(public "." (root "abi"))
	(private (root "asset") (root "core/include"))
	(link "audio_core" "mixer" "sound_script" "log" "memory"
		"subsystem" "subsystem_manager"))
 (native-only
	(executable "mixer_test"
		(sources "mixer_test.c")
		(private "." (root "abi"))
		(link "mixer" "memory" "m"))
	(test "mixer" "mixer_test")

	(executable "audio_core_test"
		(sources "audio_core_test.c")
		(private "." (root "abi"))
		(link "audio_core" "mixer" "memory" "m"))
	(test "audio_core" "audio_core_test")))
