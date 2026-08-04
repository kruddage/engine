#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds the native linux-x86_64 archives — the libraries an SDK tarball will
# want (libscript.a, libasset_plugin.a, libentity_plugin.a, ...) — and copies
# them into one predictable directory, without linking or running a single
# test binary.
#
# `sh krudd/kruddmake/run-tests.sh` already leaves these same archives behind
# in its build directory, but only as a byproduct of building and running the
# whole native suite through the `native` ninja target; there was no
# supported way to ask for the archives alone. This drives ninja.scm's
# `archives` target instead — the libraries, and nothing that links or runs
# against them (#1035) — through `krudd build`, the same entry point CI
# already uses for the WASM target (see ci.yml's `build` job and
# KRUDD_TARGET in kruddmake/README.md).
#
# Deliberately plain POSIX shell, the same voice as run-tests.sh and
# kruddmake.sh and for the same reason: no node in this path, so a
# contributor (or a later CI job) with nothing but a C toolchain can still
# produce the SDK archives. Packaging them into a tarball with a manifest is
# a later PR's job; this stops at "the archives exist on disk, named".
#
#   KRUDD_BUILD_DIR     kruddmake's build directory (default: build-archives)
#   KRUDD_ARCHIVES_OUT  where the harvested .a files land
#                       (default: $KRUDD_BUILD_DIR/archives)
set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
export KRUDD_ROOT="$root"

# A build directory of its own, distinct from build-ninja's — the ordinary
# native suite's — so a run of this script never shares (and never has to
# reason about invalidating) objects a `ninja native` build left behind, and
# vice versa. Still under /build-*/, so .gitignore already covers it.
work="${KRUDD_BUILD_DIR:-$root/build-archives}"
out="${KRUDD_ARCHIVES_OUT:-$work/archives}"

export KRUDD_BUILD_DIR="$work"
export KRUDD_TARGET=archives

echo "kruddmake: building native archives (krudd build, KRUDD_TARGET=archives)"
sh "$root/krudd/kruddmake/kruddmake.sh" build

rm -rf "$out"
mkdir -p "$out"
cp "$work"/lib*.a "$out/"

echo "kruddmake: archives collected in $out"
ls -1 "$out"
