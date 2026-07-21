#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# setup.sh — one-time environment bootstrap for building krudd from source.
#
# Aimed at SteamOS / the Steam Deck, where the root filesystem is immutable and
# you "can't even get clang", but works on any Linux dev box. It does the two
# things `./krudd.sh editor` can't do for itself:
#
#   1. installs the native toolchain (compiler, ninja, Qt6, and the Vulkan
#      loader + headers + validation layers + driver + glslang)
#   2. writes .krudd-env so `./krudd.sh editor` picks up the Qt cflags/libs
#      automatically — no manual exports
#
# The native editor renders on Vulkan directly (see docs/qt-editor-shell.md),
# so there is no multi-gigabyte out-of-tree GPU library to build any more: the
# Vulkan loader and validation layers are ordinary system packages, and the
# whole bootstrap is a single package install.
#
# Re-runnable: every step is skipped if its result is already there, so a second
# run is cheap and only repairs what's missing.
#
#   ./setup.sh              bootstrap, then tells you to run ./krudd.sh editor
#
# See docs/qt-editor-shell.md for the long-form explanation of each step.
set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

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
# 1. Toolchain (compiler + ninja + Qt6 + Vulkan loader/headers/validation).
# ---------------------------------------------------------------------------

# Is at least one Vulkan *driver* (ICD) installed? The loader is only a
# dispatch shim: with no ICD manifest to point it at a driver .so it enumerates
# zero devices, and the editor fails at "no adapter" having linked and started
# perfectly. The loader searches both of these directories.
have_vulkan_icd() {
	for d in /usr/share/vulkan/icd.d /etc/vulkan/icd.d; do
		for f in "$d"/*.json; do
			[ -f "$f" ] && return 0
		done
	done
	return 1
}

# Everything the editor build needs is present already? Two markers matter
# beyond the compilers: the validation-layer manifest (the Vulkan *validation*
# layers, not just the loader — that is the whole point of this backend's
# bring-up) and an ICD, without which the loader has nothing to load.
have_toolchain() {
	command -v cc >/dev/null 2>&1 &&
		command -v ninja >/dev/null 2>&1 &&
		command -v git >/dev/null 2>&1 &&
		[ -e /usr/include/vulkan/vulkan.h ] &&
		[ -f /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json ] &&
		have_vulkan_icd &&
		pkg-config --exists Qt6Widgets Qt6Gui Qt6Core 2>/dev/null
}

# The editor needs Qt's QPA *private* headers as well as the public ones (one
# header, for the Wayland surface lookup — see krudd_qt.cpp). Arch ships them
# inside qt6-base; Debian and Fedora split them into a separate -private package.
#
# Each branch installs a Vulkan *driver* alongside the loader. Debian and Fedora
# ship every Mesa driver — including the lavapipe software rasteriser, which is
# what a GPU-less CI box falls back to — in one `mesa-vulkan-drivers`. Arch
# splits them per-GPU, so the Deck's RDNA2 needs `vulkan-radeon` by name, and
# `vulkan-swrast` is the separate lavapipe package that gives the Arch branch
# the same software fallback the other two get for free.
install_toolchain() {
	if command -v pacman >/dev/null 2>&1; then
		sudo pacman -S --needed --noconfirm \
			base-devel git ninja qt6-base \
			vulkan-icd-loader vulkan-headers \
			vulkan-radeon vulkan-swrast \
			vulkan-validation-layers glslang
	elif command -v apt-get >/dev/null 2>&1; then
		sudo apt-get update
		sudo apt-get install -y \
			build-essential git ninja-build pkg-config \
			qt6-base-dev qt6-base-private-dev libvulkan-dev \
			mesa-vulkan-drivers \
			vulkan-validationlayers glslang-tools
	elif command -v dnf >/dev/null 2>&1; then
		sudo dnf install -y \
			@development-tools git ninja-build \
			pkgconf-pkg-config qt6-qtbase-devel \
			qt6-qtbase-private-devel \
			vulkan-loader-devel mesa-vulkan-drivers \
			vulkan-validation-layers glslang
	else
		say "no supported package manager (pacman/apt/dnf) found."
		say "install a C compiler, ninja, git, Qt6, and the Vulkan"
		say "loader + headers + validation layers + a driver (ICD) for"
		say "your GPU + glslang by hand, then re-run ./setup.sh."
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
# 2. .krudd-env — sourced automatically by krudd.sh.
# ---------------------------------------------------------------------------
#
# Written with `:=` conditional assignment so an explicit value in the caller's
# shell always wins over what we discovered here.
#
# There is deliberately nothing Vulkan-shaped here: the loader is on the default
# library search path and its headers on the default include path, so the build
# just links -lvulkan. KRUDD_VULKAN (which pulls the Vulkan target into the
# native graph) is set by `./krudd.sh editor` per-invocation, NOT here — writing
# it into the sourced env would pull Vulkan into every `krudd build` too.

env_file="$root/.krudd-env"
{
	echo "# Written by setup.sh — sourced automatically by krudd.sh."
	echo "# Re-run ./setup.sh to refresh, or delete this file to reset."
} > "$env_file"

# Qt6 is the editor's windowing toolkit; wire up its cflags/libs so
# `./krudd.sh editor` needs no manual exports. install_toolchain pulls it in,
# so this normally succeeds — warn (don't fail) if it somehow isn't found.
if pkg-config --exists Qt6Widgets Qt6Gui Qt6Core 2>/dev/null; then
	# pkg-config only reports the public include dirs, but krudd_qt.cpp needs
	# one QPA private header too. Qt keeps those under <includedir>/QtGui/<ver>,
	# so derive the path from pkg-config rather than hardcoding a Qt version.
	qt_private="$(pkg-config --variable=includedir Qt6Gui)/QtGui/$(pkg-config --modversion Qt6Gui)"
	if [ ! -f "$qt_private/QtGui/qpa/qplatformnativeinterface.h" ]; then
		say "warning: Qt6 QPA private headers not found under $qt_private —"
		say "install them (Debian: qt6-base-private-dev, Fedora:"
		say "qt6-qtbase-private-devel; Arch ships them in qt6-base)."
	fi
	{
		echo ": \"\${KRUDD_QT_CFLAGS:=\$(pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core) -I$qt_private}\""
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
