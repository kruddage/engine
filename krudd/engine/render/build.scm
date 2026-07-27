; SPDX-License-Identifier: GPL-2.0-or-later
;;! render/ is a tier, not a module — the backends and passes under it own their
;;! own build.scm and nothing is compiled at this level. What lives here is
;;! renderer.scm, the backend interface spec every one of those modules includes
;;! the generated header of, so this spec declares that one generation step and
;;! nothing else.
;;!
;;! A directory with no targets used to be left out of manifest.scm entirely,
;;! which is precisely why renderer.scm's codegen had nowhere to be declared and
;;! sat as a literal in the generator instead (#787). A spec with only a codegen
;;! form is a legitimate spec: it declares a build fact, so it belongs with the
;;! thing it describes.
((emit-interface-header "renderer.scm" "renderer.h"))
