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
;;! The surface was `.` — the whole module root — on the reasoning that every
;;! header here exists to be included by another tier, so there is no private
;;! half to separate out and an `include/` would only add a level. True as far
;;! as it went, and it stopped one step short: a surface at the module root is
;;! the one surface that cannot be namespaced, because the directory a consumer
;;! puts on its include path has to *contain* a directory named for the module.
;;! So abi was the one module whose headers arrived unprefixed — `log_api.h`,
;;! not `<abi/log_api.h>` — across 46 include sites, and the README's "no module
;;! exports its own root" was false of exactly one module.
;;!
;;! It now exports `include/` holding `abi/`, which is the shape every module
;;! has. The extra level is what buys the prefix, and the prefix is what makes
;;! the highest-fan-in node in the tree legible at every site that reaches it.
((interface-library "abi" (interface "include")))
