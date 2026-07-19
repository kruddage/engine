; SPDX-License-Identifier: GPL-2.0-or-later
;;! The gizmo/undo-redo board UI (kruddboard.cpp, the wasm-only "kruddboard"
;;! library) has been removed. What remains is md_parse: a small markdown
;;! parser with its own native library/tests and a Scheme-embedded twin, kept
;;! here as-is — it is generic kruddmake codegen test fixture and native-test
;;! infrastructure (see kruddmake/ninja.scm and kruddmake/introspect_test.scm),
;;! unrelated to the removed editor UI.
((native-only
  (library "md_parse"
    (sources "md_parse.c")
    (public (current) (raw "${generated}")))
  (executable "md_parse_test"
              (sources "md_parse_test.c")
              (link "md_parse"))
  (test "md_parse" "md_parse_test")

  (library "md_parse_scheme"
    (sources (raw "${generated}/md_parse.scm.c"))
    (public (raw "${generated}"))
    (private (root "core/include")
             (raw "../third_party"))
    (link "script"))
  (executable "md_parse_scheme_test"
              (sources "md_parse_test.c")
              (link "md_parse_scheme"))
  (test "md_parse_scheme" "md_parse_scheme_test")))
