<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Windows packaging — NSIS installer

Closes [#694](https://github.com/kruddage/engine/issues/694) — the Windows
equivalent of the Flatpak editor distribution
([`packaging/flatpak/`](../flatpak/README.md)). Where Linux gets a signed,
self-hosted Flatpak registry, Windows gets a single downloadable
`krudd-editor-setup.exe` — a double-click installer, no package manager
involved.

## Why NSIS and not WinGet

WinGet has no static, self-hosted equivalent to the Flatpak registry served off
`gh-pages`. Its default source means submitting to `microsoft/winget-pkgs`
(review-gated, and it won't pass while the editor only presents an animated
clear — the same wall that blocks a Flathub submission), and a private WinGet
source needs a *running* REST server, not static files. So WinGet is
deliberately out of scope. NSIS gives the simplest real thing: an installer on
a GitHub Release, no hosting beyond Releases, no review gate.

## What's here

- `krudd-editor.nsi` — the NSIS script. It packages an already-staged payload
  directory (the exe + its Qt/Vulkan/mingw runtime DLLs) into
  `krudd-editor-setup.exe`, with Start Menu + Desktop shortcuts, an
  Add/Remove Programs entry, and an uninstaller. It builds nothing itself —
  version, payload directory, and icon are fed in with `makensis /D<name>=…`.
- [`.github/workflows/windows-build.yml`](../../.github/workflows/windows-build.yml)
  — builds `krudd_qt.exe` with the MSYS2 mingw-w64 toolchain, stages the
  runtime with `windeployqt`, and runs this script. It always uploads the
  installer as a workflow artifact; a push to `main` also publishes it to the
  `editor-windows-nightly` prerelease.

## Installing

Download `krudd-editor-setup.exe` — from the `editor-windows-nightly`
[GitHub Release](https://github.com/kruddage/engine/releases/tag/editor-windows-nightly),
or from the **Artifacts** of a
[Windows build workflow run](https://github.com/kruddage/engine/actions/workflows/windows-build.yml)
— run it, and launch **KRUDD Editor** from the Start Menu.

The installer is currently **unsigned**, so SmartScreen will warn on first run
("Windows protected your PC" → *More info* → *Run anyway*). Code signing is a
follow-up, opt-in the same way the Flatpak workflow's GPG signing is.

## How the payload is built (the tricky part)

kruddmake emits an ordinary GNU-flag ninja build (`gcc`, `ar`, `-o`), which
mingw's `gcc` accepts verbatim — so there is **no build-system fork for
Windows**, just two seams:

- **`KRUDD_EXE_SUFFIX=.exe`** — on Windows `gcc -o bin/krudd_qt` writes
  `bin/krudd_qt.exe`, so the ninja edge output has to carry the suffix or ninja
  never sees the file it built. The generator reads this env var (empty
  everywhere else, so Unix builds are byte-for-byte unchanged — see
  `krudd/kruddmake/ninja.scm`).
- **`KRUDD_VULKAN_LIBS=-lvulkan-1`** — the Vulkan loader's import library is
  `vulkan-1` on Windows, not `vulkan`.

`windeployqt6` gathers the Qt DLLs and the `platforms/qwindows.dll` plugin next
to the exe; the workflow then adds `vulkan-1.dll` (the loader isn't a Qt
dependency, so windeployqt skips it) and the mingw C/C++ runtime DLLs, so the
result runs on a clean Windows box with no MSYS2 installed.

## Building it locally

On a Windows box with [MSYS2](https://www.msys2.org/), from a **MINGW64** shell:

```sh
pacman -S --needed base-devel mingw-w64-x86_64-gcc mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-pkgconf mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-tools \
  mingw-w64-x86_64-vulkan-headers mingw-w64-x86_64-vulkan-loader

qt_inc=$(pkg-config --variable=includedir Qt6Gui)
qtgui_private=$(echo "$qt_inc"/QtGui/[0-9]*)

export CC=gcc KRUDD_CC=gcc KRUDD_CXX=g++ \
  KRUDD_QT=1 KRUDD_VULKAN=1 KRUDD_EXE_SUFFIX=.exe KRUDD_GENERATE_ONLY=1 \
  KRUDD_QT_CFLAGS="$(pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core) -I$qtgui_private" \
  KRUDD_QT_LIBS="$(pkg-config --libs Qt6Widgets Qt6Gui Qt6Core)" \
  KRUDD_VULKAN_LIBS="-lvulkan-1"

./krudd.sh build
ninja -C build -f build.ninja bin/krudd_qt.exe

# stage the runtime, then package it
mkdir -p stage && cp build/bin/krudd_qt.exe stage/
windeployqt6 --release --no-translations --compiler-runtime stage/krudd_qt.exe
cp /mingw64/bin/vulkan-1.dll stage/
makensis /DKRUDD_STAGE_DIR="$(cygpath -w "$PWD/stage")" packaging/nsis/krudd-editor.nsi
```

## Known caveat

`editor-qt` still renders an animated clear colour, not a real scene
([`docs/qt-editor-shell.md`](../../docs/qt-editor-shell.md)) — an installed
build launches and runs, but shows a colour-cycling window. Tracked separately
(#667/#675/#676), not blocked on anything here.
