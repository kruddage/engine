; SPDX-License-Identifier: GPL-2.0-or-later
;;! The whole viewer is modelgen.scm. Like every project it is Scheme and no C,
;;! so this directory declares three things: that the build ships the viewer, how
;;! the source reaches the test, and the test. The contract those declarations
;;! are drawn from is projects/README.md.
;;!
;;! Shipped, not staged: `default` holds the staged slot, and a tool is not what
;;! a bare URL should open on. The (embed ...) is for the test alone —
;;! MODELGEN_SCM is included by modelgen_test.c and by nothing else, so the test
;;! drives the real source with no filesystem under it and the shipped WASM
;;! module pays nothing for it.
((project-source "modelgen.scm")
 (embed "modelgen.scm" "modelgen_scm.h" "MODELGEN_SCM")

 (native-only
  ;;! The harness is game/project_test's, so the whole of what a project test
  ;;! used to assemble by hand is one link name (#1035). The FAT half of it —
  ;;! project_test_init_with_catalog — because this project defines assets and
  ;;! binds them: six meshes, a material and a turntable script reach the
  ;;! catalog through mesh-define! / material-define! / script-define!, and
  ;;! whether the dropdown's pick still lands on real geometry is exactly what
  ;;! the catalog answers. The script driver comes with that half too, which is
  ;;! what lets the test step the turntable.
  ;;!
  ;;! The two include roots are this directory's own: ${generated} for the
  ;;! embedded source above, and ../third_party for the s7.h the test needs to
  ;;! stand in for kruddgui's panel seam — a wasm-only module the native image
  ;;! does not carry, so the test defines the one primitive itself and checks
  ;;! that loading installs a panel through it.
  (executable "modelgen_test"
              (sources "modelgen_test.c")
              (private "." (raw "../third_party") (raw "${generated}"))
              (link "krudd_project_test"))
  (test "modelgen" "modelgen_test")))
