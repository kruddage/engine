<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# What the workspace is for

**The workspace is the physical design of the JavaScript layer. It is not the
build system, and it does not manage dependencies — there are none to manage,
and since #1010 there is no package manager here to manage them.**

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

#932 answered "what is it for" and explicitly declined to ask "should it exist",
recording the answer as a bare assertion. #1010 asked anyway, and the assertion
did not survive: Q6 below is the reasoning that replaced it, and pnpm is gone.
The workspace is not — it is the packages and the boundary between them, which
never depended on which program ran the scripts.

## Q1 — the workspace is the physical design of the JS layer

There are four JavaScript packages, plus `@kruddage/dawn-smoke` — a fifth
workspace member that is C and a shell script, not JS, and holds no surface for
the boundary check to read. Each of the four JS packages declares a name, a
surface, and what it may reach; `workspace.sh check` reads those declarations
back and fails the build when one package reaches around another's `exports`
map, or reaches the build tree out of band. That is the whole of what the
workspace buys, and it is worth buying: the barrier holds on its own rather than
by everyone remembering where the line is. `dawn-smoke` sits outside that
mechanism rather than being forced into a fifth shape of it — it is in the
workspace only so `workspace.sh smoke` is a real command, and the boundary
check's rules 1 and 3 both read source files, so a package with none is simply
outside their reach rather than exempted from them.

Membership is the filesystem: any directory under `packages/` or `tools/` that
declares a `package.json`. It used to also be a list, in `pnpm-workspace.yaml`,
which `tools/barriers`' `packageDirs()` mirrored "entry for entry" — two
statements of one fact, and the check's own comment had to explain that they
must be read as one thing. #1010 deleted the list rather than the mirror, so
there is now one place membership comes from and nothing left to drift.

Two other answers were available and are rejected:

- **Dependency management.** The workspace manages no dependencies. When there
  was a lockfile it was 333 bytes: six importers, one `link:` edge, and zero
  registry packages. The zero-dependency supply chain is a feature and it is
  staying, which means dependency management will never be what this is for.
  Settings that arbitrate between competing third-party versions are
  configuring a graph that does not exist, and they go — and in the end so did
  the tool whose job that was (Q6).
- **The site build and nothing more.** Honest, and it under-describes what is
  already here. The boundary check is not site staging, and retiring it would
  give up the one thing the split has actually delivered.

## Q2 — the workspace is an explicit second door, not the single entry point

There are two entry points into this repository's build, and both are supported:

| Door | Command | Needs |
|---|---|---|
| kruddmake, directly | `sh krudd/kruddmake/kruddmake.sh build` | a C compiler, `ninja` (and emsdk for WASM) |
| the workspace | `sh tools/workspace/workspace.sh build` | the above, plus Node |

CI has always used both — the WASM build and the site staging enter through the
workspace, the sanitizer and coverage jobs call `run-tests.sh` by path. That is
the right design: those two jobs need no Node at all, and putting Node in the
path of a C test run to satisfy a slogan would be a real cost for no gain.

What was wrong was not the practice but the claim. The repository asserted one
door and ran two. So: two doors, named in the same words in this file, in the
README, and in CI — and each place that bypasses the workspace says that it is
deliberate and why.

Since #1010 the two doors are also the same *shape*: a POSIX shell script taking
a task name, at a path. That was a consequence of removing pnpm rather than a
reason to, but it is the part of the change most worth keeping. The old pairing
asked a reader to hold two unrelated conventions at once — a shell entry point
on one side, a package manager's filter grammar on the other — and the symmetry
Q2 claimed was only in the words. Now it is in the commands.

The consequence for test commands is unchanged: `workspace.sh test` is the
*workspace's* suite and is named so it cannot be misread as *the* tests. The
native suite is reached by `sh krudd/kruddmake/run-tests.sh`, or through the
workspace's own door at `workspace.sh test:native`.

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
states, and it is the most visible thing at the root to a newcomer.
`krudd/kruddmake/kruddmake.sh` is the entry point; the README says so.

## Q6 — there is no package manager

This file used to say, under "What this does not decide": *"Whether pnpm should
exist here. It should. 'What is it for' is not 'should it exist'."* That was an
assertion with no reasoning under it, and #1010 reopened it on exactly that
ground. The answer changed. Here is the argument, which is what should have been
here the first time.

**pnpm resolved nothing.** The lockfile was 333 bytes and named zero registry
packages, because the zero-dependency supply chain is a stated feature (Q1) and
is staying. `pnpm install` reported `Already up to date` and wrote no package.
This was never going to grow into a real dependency graph.

**Its entire material output was one symlink.** After an install, everything
under `node_modules/` was three bookkeeping files and
`packages/site/node_modules/@kruddage/engine -> ../../../engine`. That symlink is
genuinely load-bearing — `packages/site` names `@kruddage/engine` by bare
specifier in three files, and Node resolves bare specifiers through
`node_modules` — but it is a symlink, and `ln -s` makes symlinks.

**The other three uses were names, not capabilities.** Recursive task running is
a loop over the packages. Filtered addressing (`--filter X run build`) is a
longer way to write `node packages/engine/scripts/build.mjs`. And `pnpm check`
was a *name* for `node tools/barriers/check-barriers.mjs` — 214 lines that read
`package.json` files off the filesystem and run identically with no package
manager present. Q1 calls the boundary check the one thing the split has
delivered; it never needed pnpm to deliver it.

So the tool was carrying one symlink and three names. `tools/workspace/workspace.sh`
carries them instead, in POSIX shell, at the same shape as the other door (Q2).

Two arguments against, both real, and why they lost:

- **"The second door costs nothing and buys optionality."** It cost the accurate
  version of this file — for three releases the root said pnpm was here for
  something, and the honest answer was one symlink. Optionality against a policy
  that says *never add a dependency* is optionality on a branch this repo has
  committed to not taking.
- **"`pnpm check` and `pnpm test` are memorable; a bespoke script is not."**
  This is the one that nearly held, and it is why nothing here is named
  `run.sh`. A task runner's real product is its names, and replacing a
  convention every JS developer knows with one only this repo knows is a real
  loss — paid once, by readers of this repo, who already have to learn
  `kruddmake.sh` to build anything at all. The names are now consistent with the
  door beside them rather than with npm, and that is the trade: legibility to a
  passing JS developer, for legibility to someone reading this repository.

What is explicitly *not* claimed: that this is faster (pnpm took ~800ms), or
that a package manager is wrong in general. It is that a package manager which
resolves nothing is machinery for a graph that does not exist, and Q1 already
rejected that reasoning once — it just had not been pointed at the tool itself.

What did not change: barrier rule 1 still forbids relative imports across
packages, so `packages/site` still says `@kruddage/engine`. Removing pnpm by
switching to `../../engine/src/index.mjs` would have deleted the workspace's
only real deliverable to save a symlink, and was rejected outright.

## What this does not decide

- **Anything about the C tree.** `krudd/` leaves the workspace (#934) under
  every answer above. The remaining modules do not get manifests.
- **Whether the *packages* should exist.** They should, and Q1 says why. Q6 is
  about the package *manager*; the packages, their manifests and their
  boundaries all outlived it.
- **Adding a dependency to justify the tooling.** No. This is now harder rather
  than easier, and that is a cost of Q6 worth naming: a first third-party
  dependency would mean bringing a package manager back, not editing a script.
