#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# kruddmake's own test suite.
#
# The build language is Scheme, and these are the checks that read it back:
# introspect_test.scm exercises the codegen/embed helpers, resolve_test.scm the
# resolver and the ninja emitter. Both run on the pinned krudds7 interpreter
# that sync.sh fetches, so this needs no emsdk, no ninja and no C compiler —
# which is why it is worth having apart from the native suite at all.
#
# Two callers, both by this path. run-tests.sh runs it as its first two stages
# rather than repeating them: the native suite is this suite plus a toolchain.
# CI's lint job runs it directly, because that job has no toolchain and used to
# reach these checks through the workspace's recursive test task — which
# stopped reaching them when krudd/ left the workspace and this stopped being
# a package's `test` script (#934).
#
# Side effect, and the reason KRUDD_NINJA_OUT exists: resolve_test.scm renders
# the real manifest to a build.ninja on its way through, which is what
# run-tests.sh then builds. Standalone, that lands in the same build-ninja/
# directory it always has.
set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
export KRUDD_ROOT="$root"

# Fetches + verifies the pinned s7 artifacts and exports S7_CLI — the standalone
# s7 interpreter (krudds7) shipped by the kruddage/s7 release. Skipped when a
# caller (run-tests.sh) has already sourced it, so a full native run does not
# re-verify four artifacts to run two Scheme files.
if [ -z "${S7_CLI:-}" ]; then
	. "$root/krudd/third_party/sync.sh"
fi

# KRUDD_BUILD_DIR lets a caller (e.g. the sanitizer / coverage CI jobs) point
# the generated build.ninja and its objects at a variant-specific directory so
# an instrumented build never reuses the plain build's object files. Defaults
# to build-ninja for a normal local run.
work="${KRUDD_BUILD_DIR:-$root/build-ninja}"
mkdir -p "$work"

# Stage 0: s7-only bootstrap checks (codegen/embed helpers).
"$S7_CLI" "$root/krudd/kruddmake/introspect_test.scm"

# Stage 1: resolver/emitter checks; render build.ninja for whoever wants it.
# KRUDD_S7BIN lets resolve_test.scm bake a self-contained `regen` command into
# build.ninja so a later raw `ninja` run regenerates codegen when a `.scm` input
# changes.
KRUDD_NINJA_OUT="${KRUDD_NINJA_OUT:-$work/build.ninja}"
export KRUDD_NINJA_OUT
export KRUDD_S7BIN="$S7_CLI"
"$S7_CLI" "$root/krudd/kruddmake/resolve_test.scm"
