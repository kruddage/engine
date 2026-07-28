<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# What the pnpm workspace is for

**The workspace is the physical design of the JavaScript layer. It is not the
build system, and it does not manage dependencies — there are none to manage.**

**kruddmake is the second door, and it is deliberate.** `krudd/kruddmake` builds
C and WASM with a compiler and nothing else. The workspace never becomes a
prerequisite for it.

Those two sentences are the contract. Every root file should be checkable
against them without further interpretation; where one is not, that is a bug,
even when nothing fails.

## Why this file exists

The workspace was stood up by #914 as the outer build system, and #902 asserted
it would become the single entry point to everything. #902 is dropped. That left
the question underneath it — *what is pnpm here for* — answered nowhere, while
the root kept accumulating files that each assumed a different answer. #932 is
the initiative that closes it.

## Q1 — the workspace is the physical design of the JS layer

There are four JavaScript packages, plus `@kruddage/dawn-smoke` — a fifth
workspace member that is C and a shell script, not JS, and holds no surface for
`pnpm check` to read. Each of the four JS packages declares a name, a surface,
and what it may reach; `pnpm check` reads those declarations back and fails the
build when one package reaches around another's `exports` map, or reaches the
build tree out of band. That is the whole of what the workspace buys, and it is
worth buying: the barrier holds on its own rather than by everyone remembering
where the line is. `dawn-smoke` sits outside that mechanism rather than being
forced into a fifth shape of it — it is in the workspace only so
`pnpm --filter @kruddage/dawn-smoke run smoke` is a real command, and the
boundary check's rules 1 and 3 both read source files, so a package with none
is simply outside their reach rather than exempted from them.

Two other answers were available and are rejected:

- **Dependency management.** The workspace manages no dependencies.
  `pnpm-lock.yaml` is a few hundred bytes: a handful of importers, a couple of
  `link:` edges, and zero registry packages. The zero-dependency supply chain is
  a feature and it is staying, which means dependency management will never be
  what this is for. Settings that arbitrate between competing third-party
  versions are configuring a graph that does not exist, and they go.
- **The site build and nothing more.** Honest, and it under-describes what is
  already here. The boundary check is not site staging, and retiring it would
  give up the one thing the split has actually delivered.

## Q2 — pnpm is an explicit second door, not the single entry point

There are two entry points into this repository's build, and both are supported:

| Door | Command | Needs |
|---|---|---|
| kruddmake, directly | `krudd/kruddmake/kruddmake.sh build` | a C compiler, `ninja` (and emsdk for WASM) |
| the workspace | `pnpm --filter @kruddage/engine run build` | the above, plus Node |

CI has always used both — the WASM build and the site staging enter through
pnpm, the sanitizer and coverage jobs call `run-tests.sh` by path. That is the
right design: those two jobs need no Node at all, and putting Node in the path
of a C test run to satisfy a slogan would be a real cost for no gain.

What was wrong was not the practice but the claim. The repository asserted one
door and ran two. So: two doors, named in the same words in the root scripts, in
the README, and in CI — and each place that bypasses the workspace says that it
is deliberate and why.

The consequence for test commands is that `pnpm test` is the *workspace's* suite
and is named so it cannot be misread as *the* tests. The native suite is reached
by `sh krudd/kruddmake/run-tests.sh`, or through the workspace's own door at
`pnpm test:native`.

## Q3 — `version.txt` owns the version

The released version is the single line in `version.txt`, maintained by
release-please under its `simple` (plain-text) strategy. `package.json` says
`0.0.0` and means it.

The version is consumed by Scheme: `introspect.scm` stamps it into the WASM
build and the shell template, and the site's cache-busting hash derives from
what that produces. Moving the number into `package.json` would put Node in the
path of a fact the C build needs — which is exactly the dependency Q2 refuses.
The mechanism was right; the missing piece was anyone saying so.

## Q4 — the boundary check leans on rule 3

"Only `@kruddage/engine` may depend on `@kruddage/kruddmake`" was a statement
about a package that no longer exists (#934). Its ground is already covered:
rule 3 matches `krudd/` by path with the same `@kruddage/engine` exemption. So
rule 2 goes rather than being reimplemented as a regex beside the one that
already works — two mechanisms for one rule is how the second one rots.

The cost is real and worth naming: rule 3's exemption becomes load-bearing where
it used to be belt-and-braces, and the comment beside it says so.

## Q5 — `krudd.sh` is deleted

#902 gave the root shim one release cycle. Three elapsed. A file at the root
that calls itself deprecated with no date on it is the worst of the available
states, and it is the most visible thing at the root to someone who knows pnpm.
`krudd/kruddmake/kruddmake.sh` is the entry point; the README says so.

## What this does not decide

- **Anything about the C tree.** `krudd/` leaves the workspace (#934) under
  every answer above. The remaining modules do not get manifests.
- **Whether pnpm should exist here.** It should. "What is it for" is not "should
  it exist".
- **Adding a dependency to justify the tooling.** No.
