#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# DEPRECATED — forwards to kruddmake.
#
# The build entry point is `krudd/kruddmake/kruddmake.sh`, the public surface of
# @kruddage/kruddmake (#920). This file used to be the entry point itself, and
# so was a build entry that lived outside the build graph: nothing declared a
# dependency on it, and the only thing keeping the rest of the workspace off it
# was a regex in scripts/check-barriers.mjs. It is now a dependency edge —
# @kruddage/engine declares @kruddage/kruddmake and reaches the build through
# it.
#
# What is left here is muscle memory and every doc, script and shell history
# that types `./krudd.sh build` (#905). It forwards; it holds no logic. Prefer:
#
#   krudd/kruddmake/kruddmake.sh build     directly, no node anywhere
#   pnpm --filter @kruddage/engine build   through the package
set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

exec "$root/krudd/kruddmake/kruddmake.sh" "$@"
