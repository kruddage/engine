#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# setup.sh — one-time environment bootstrap for building krudd from source.
#
# Aimed at SteamOS / the Steam Deck, where the root filesystem is immutable and
# you "can't even get clang", but works on any Linux dev box. It does the three
# things `./krudd.sh editor` can't do for itself:
#
#   1. installs the native toolchain (compiler, cmake, ninja, Qt6, Vulkan loader)
#   2. builds pinned native Dawn once, out-of-tree, WITH a Wayland/X11 surface
#   3. writes .krudd-env so `./krudd.sh editor` picks up KRUDD_DAWN_PREFIX and
#      the Qt cflags/libs automatically — no manual exports
#
# Re-runnable: every step is skipped if its result is already there, so a second
# run is cheap and only repairs what's missing.
#
#   ./setup.sh              bootstrap, then tells you to run ./krudd.sh editor
#
# See docs/qt-editor-shell.md for the long-form explanation of each step.
set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# The pinned Dawn revision. This MUST match tools/dawn-smoke/README.md and the
# emsdk emdawnwebgpu port the web build uses — the native build is only a
# faithful debugger of the shipped web build when the two are in lockstep. If
# the port is ever rolled, re-pin here AND in tools/dawn-smoke/README.md.
DAWN_REV=31e25af254ab572c77054edec4946d2244e184dd

# Out-of-tree, so the build survives worktrees and is never churned by a repo
# build. It is not small: ~1.6 GB of source and objects for a ~38 MB installed
# libwebgpu_dawn.a. Honour an existing KRUDD_DAWN_PREFIX so a shared Dawn is
# reused.
dawn_src="$HOME/.krudd/dawn-native"
dawn_prefix="${KRUDD_DAWN_PREFIX:-$dawn_src/install}"

say() { printf 'setup.sh: %s\n' "$1"; }

# Are we inside a distrobox / toolbox / docker container (mutable FS) rather than
# on the bare host?
in_container() {
	[ -f /run/.containerenv ] || [ -f /run/.toolboxenv ] ||
		[ -f /run/.dockerenv ] || [ -n "${container:-}" ]
}

# ID field of /etc/os-release, read in a subshell so it leaks no variables.
os_id=$( . /etc/os-release 2>/dev/null && printf '%s' "$ID" )

# ---------------------------------------------------------------------------
# 1. Toolchain.
# ---------------------------------------------------------------------------

# Everything the editor build needs is present already?
have_toolchain() {
	command -v cc >/dev/null 2>&1 &&
		command -v cmake >/dev/null 2>&1 &&
		command -v ninja >/dev/null 2>&1 &&
		command -v git >/dev/null 2>&1 &&
		command -v python3 >/dev/null 2>&1 &&
		pkg-config --exists Qt6Widgets Qt6Gui Qt6Core 2>/dev/null
}

install_toolchain() {
	if command -v pacman >/dev/null 2>&1; then
		sudo pacman -S --needed --noconfirm \
			base-devel git cmake ninja python qt6-base vulkan-icd-loader
	elif command -v apt-get >/dev/null 2>&1; then
		sudo apt-get update
		# The libx11-xcb/xcb/x11/wayland/xkb dev packages are what the native
		# Dawn build's X11 + Wayland surface (DAWN_USE_X11/WAYLAND=ON below)
		# needs. qt6-base-dev happens to pull most in transitively, but name
		# them so the Dawn build doesn't depend on that incidentally.
		sudo apt-get install -y \
			build-essential git cmake ninja-build python3 \
			pkg-config qt6-base-dev libvulkan-dev \
			libx11-xcb-dev libxcb1-dev libx11-dev libwayland-dev libxkbcommon-dev
	elif command -v dnf >/dev/null 2>&1; then
		sudo dnf install -y \
			@development-tools git cmake ninja-build python3 \
			pkgconf-pkg-config qt6-qtbase-devel vulkan-loader-devel
	else
		say "no supported package manager (pacman/apt/dnf) found."
		say "install a C compiler, cmake, ninja, git, python3, Qt6 and a"
		say "Vulkan loader by hand, then re-run ./setup.sh."
		return 1
	fi
}

if have_toolchain; then
	say "toolchain already present — skipping package install."
elif [ "$os_id" = steamos ] && ! in_container; then
	# The SteamOS root filesystem is immutable, so pacman can't touch it.
	# Build inside an Arch distrobox, which shares the Deck's Wayland socket
	# and GPU. This is a subshell (`enter` drops you into a new shell), so it
	# has to be a separate step from the one that runs setup.sh inside it.
	cat >&2 <<-EOF
	setup.sh: this looks like the SteamOS host, whose root filesystem is
	setup.sh: immutable — packages can't be installed here directly.
	setup.sh:
	setup.sh: Build inside an Arch distrobox instead (it shares the Deck's
	setup.sh: Wayland socket and GPU). Run these two lines:
	setup.sh:
	setup.sh:     distrobox create -i archlinux:latest krudd && distrobox enter krudd
	setup.sh:     cd $root && ./setup.sh
	setup.sh:
	setup.sh: See docs/qt-editor-shell.md for the full walkthrough.
	EOF
	exit 1
else
	say "installing the native toolchain (needs sudo)…"
	install_toolchain
fi

# ---------------------------------------------------------------------------
# 2. Native Dawn (pinned, WITH a surface — the editor recipe, not the
#    offscreen dawn-smoke one).
# ---------------------------------------------------------------------------

if [ -f "$dawn_prefix/lib/libwebgpu_dawn.a" ]; then
	say "native Dawn already built at $dawn_prefix — skipping."
else
	say "building native Dawn ($DAWN_REV) — one-time, needs ~1.6 GB and a while."
	mkdir -p "$dawn_src"
	cd "$dawn_src"
	[ -d .git ] || git init -q .
	git remote add origin https://github.com/google/dawn.git 2>/dev/null || true
	git fetch --depth 1 origin "$DAWN_REV"
	git checkout -q FETCH_HEAD
	cmake -S . -B out/Release -G Ninja \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$dawn_prefix" \
		-DDAWN_FETCH_DEPENDENCIES=ON \
		-DDAWN_ENABLE_VULKAN=ON \
		-DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_OPENGLES=OFF \
		-DDAWN_ENABLE_NULL=OFF \
		-DDAWN_USE_WAYLAND=ON -DDAWN_USE_X11=ON -DDAWN_USE_GLFW=OFF \
		-DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF \
		-DDAWN_BUILD_BENCHMARKS=OFF -DDAWN_BUILD_PROTOBUF=OFF \
		-DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF \
		-DTINT_BUILD_BENCHMARKS=OFF \
		-DDAWN_ENABLE_INSTALL=ON -DTINT_ENABLE_INSTALL=OFF \
		-DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC -DBUILD_SHARED_LIBS=OFF
	ninja -C out/Release webgpu_dawn
	cmake --install out/Release
	cd "$root"
fi

# ---------------------------------------------------------------------------
# 3. .krudd-env — sourced automatically by krudd.sh.
# ---------------------------------------------------------------------------
#
# Written with `:=` conditional assignment so an explicit value in the caller's
# shell always wins over what we discovered here.

env_file="$root/.krudd-env"
{
	echo "# Written by setup.sh — sourced automatically by krudd.sh."
	echo "# Re-run ./setup.sh to refresh, or delete this file to reset."
	echo ": \"\${KRUDD_DAWN_PREFIX:=$dawn_prefix}\""
	echo "export KRUDD_DAWN_PREFIX"
} > "$env_file"

# Qt6 is the editor's windowing toolkit; wire up its cflags/libs so
# `./krudd.sh editor` needs no manual exports. install_toolchain pulls it in,
# so this normally succeeds — warn (don't fail) if it somehow isn't found, so
# the Dawn build above is still recorded.
if pkg-config --exists Qt6Widgets Qt6Gui Qt6Core 2>/dev/null; then
	{
		echo ": \"\${KRUDD_QT_CFLAGS:=\$(pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core)}\""
		echo "export KRUDD_QT_CFLAGS"
		echo ": \"\${KRUDD_QT_LIBS:=\$(pkg-config --libs Qt6Widgets Qt6Gui Qt6Core)}\""
		echo "export KRUDD_QT_LIBS"
	} >> "$env_file"
	say "Qt6 found — the editor is wired up."
else
	say "warning: Qt6 not found via pkg-config — install it (see"
	say "docs/qt-editor-shell.md) and set KRUDD_QT_CFLAGS before ./krudd.sh editor."
fi

say "done. Wrote $env_file."
say "Now run:  ./krudd.sh editor"
