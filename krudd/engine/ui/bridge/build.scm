; SPDX-License-Identifier: GPL-2.0-or-later
;;! The editor boundary (#945): one crossing per frame between the TypeScript
;;! editor and the C document, a binary tape in and a JSON reply out. See
;;! include/bridge.h for the shape and the reasoning.
;;!
;;! Split the way viewport/ is: the wasm library carries the emscripten exports
;;! and the subsystem registration, and the pure half (bridge.c) is compiled
;;! straight into the native test. That is what lets the whole boundary —
;;! decoder, dispatcher, generations and JSON writer — be tested with a
;;! compiler and nothing else, on a box with no emsdk and no browser.
;;!
;;! It reads world/entity's headers rather than linking entity.c: every scene
;;! mutation goes through the `scene` service vtable it is handed, so the only
;;! thing it needs from that module is the shape of the columns it reports.
((wasm-only
  (library "bridge"
    (sources "bridge.c" "bridge_plugin.c" "bridge_params.c")
    (private "include" (root "abi") (root "world/entity/include")
             (root "base/math/include") (root "core/include"))
    (link "subsystem" "subsystem_manager" "script" "m")))

 (native-only
  ;;! The boundary end to end, browser-free: a hand-built world and two fake
  ;;! service vtables in, the reply document's exact bytes asserted on the way
  ;;! out. No subsystem manager, no plugins, no wasm.
  ;;! bridge_params.c comes with it, and brings s7: the inspector's derivation
  ;;! reads a (params ...) clause through core/script.c, which is the one place
  ;;! that vocabulary is parsed (#951). A test that faked that parse would be
  ;;! asserting against a second reader of the format, which is the thing the
  ;;! derivation exists to avoid.
  (executable "bridge_test"
              (sources "bridge_test.c" "bridge.c" "bridge_params.c"
                       (root "world/entity/entity.c"))
              (private "include" (root "abi") (root "world/entity/include")
                       (root "base/math/include") (root "core/include")
                       (root "base/memory/include")
                       (raw "../third_party"))
              (link "script" "memory" "m"))
  (test "bridge" "bridge_test")))
