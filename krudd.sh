#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# krudd bootstrap
#   ./krudd.sh              resolve projects here (setup / run / pick)
#   ./krudd.sh build        configure + build, no prompts (what CI runs)
#   ./krudd.sh new-project  scaffold a new project
set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Are we inside a distrobox / toolbox / docker container (mutable FS)?
in_container() {
	[ -f /run/.containerenv ] || [ -f /run/.toolboxenv ] ||
		[ -f /run/.dockerenv ] || [ -n "${container:-}" ]
}

os_id=$( . /etc/os-release 2>/dev/null && printf '%s' "$ID" )

# Pick up the environment setup.sh wrote (KRUDD_DAWN_PREFIX, and the Qt vars if
# Qt6 was found), so `./krudd.sh editor` works after a one-time `./setup.sh`
# with no manual exports. It uses `:=` assignment, so anything already set in
# the caller's shell still wins.
if [ -f "$root/.krudd-env" ]; then
	. "$root/.krudd-env"
fi

# System default compiler — CC if set, else the first of cc/gcc/clang found.
cc=${CC:-}
if [ -z "$cc" ]; then
	for c in clang gcc cc; do
		if command -v "$c" >/dev/null 2>&1; then
			cc=$c
			break
		fi
	done
fi
if [ -z "$cc" ]; then
	if [ "$os_id" = steamos ] && ! in_container; then
		cat >&2 <<-EOF
		krudd.sh: this looks like the SteamOS host, whose root filesystem is
		krudd.sh: immutable — no compiler is available here.
		krudd.sh:
		krudd.sh: Build inside an Arch distrobox instead (it shares the Deck's
		krudd.sh: Wayland socket and GPU). Run these two lines:
		krudd.sh:
		krudd.sh:     distrobox create -i archlinux:latest krudd && distrobox enter krudd
		krudd.sh:     cd $root && ./setup.sh && ./krudd.sh editor
		krudd.sh:
		krudd.sh: See docs/qt-editor-shell.md for the full walkthrough.
		EOF
	else
		echo "krudd.sh: no C compiler found (set CC, or install cc/gcc/clang)" >&2
	fi
	exit 1
fi

echo "krudd.sh: found C compiler $cc"

# We always build against the latest kruddage/s7 release (see
# krudd/third_party/s7.artifact); sync.sh fetches + checksum-verifies the
# prebuilt header/library before we link the tool that embeds them, and
# exports S7_HEADER / S7_NATIVE_LIB.
. "$root/krudd/third_party/sync.sh"

bin="$root/krudd/krudd"
src="$root/krudd/krudd.c"
# The WITH_* defines keep krudd.c's view of s7.h identical to how the linked
# library was built (no dlopen C-loader, s7 as a library rather than a REPL).
if [ ! -x "$bin" ] || [ "$src" -nt "$bin" ] || [ "$S7_NATIVE_LIB" -nt "$bin" ]; then
	"$cc" -O2 -w -DWITH_C_LOADER=0 -DWITH_MAIN=0 \
		-I"$root/krudd/third_party" \
		-o "$bin" "$src" "$S7_NATIVE_LIB" -lm
fi

export KRUDD_ROOT="$root"
exec "$bin" "$@"
