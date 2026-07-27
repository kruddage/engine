; SPDX-License-Identifier: GPL-2.0-or-later
;;! The plugin vtables every tier includes. Twelve hand-written headers, no C,
;;! no codegen and nothing to compile — which is exactly why this directory had
;;! no spec until now, and why manifest.scm documented the absence as
;;! deliberate.
;;!
;;! That reasoning was sound for a build system and wrong for a dependency
;;! graph (#919). A node with no outgoing edges and the highest fan-in in the
;;! tree is the most important node there is, and until it was listed it was the
;;! one thing `resolve-check-tiers` could not have an opinion about.
;;!
;;! `interface-library` is the form for exactly this: a target that is a public
;;! surface and nothing else. ninja.scm emits no edge for it, so this spec adds
;;! no compilation — it puts abi in the target table, at the top of the tier
;;! order, so that a module reaching for it is a link edge the generator can
;;! read rather than an include path it cannot.
;;!
;;! The surface is `.` — the whole module root. This is the one directory where
;;! that is the examined answer rather than the unexamined one (contrast #922):
;;! every header here exists to be included by another tier, so there is no
;;! private half to separate out and an `include/` would only add a level.
((interface-library "abi" (interface ".")))
