<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# krudd in a Qt editor shell (#675)

A proof of life for the Qt editor shell #675 asks for: the engine's WebGPU
backend, presenting into a `QWindow` embedded in real Qt chrome — a menu bar,
a toolbar, Scene/Inspector docks — instead of a bare top-level window. It is
the Qt sibling of `krudd_window` (see docs/steamos-window.md): same native
Dawn backend, same `webgpu_platform.h` seam, same animated-clear proof of
life. What changes is who owns the window and who hands Dawn its native
surface.

```sh
KRUDD_DAWN_PREFIX=/path/to/dawn-native/install \
KRUDD_QT_CFLAGS="$(pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core)" \
KRUDD_QT_LIBS="$(pkg-config --libs Qt6Widgets Qt6Gui Qt6Core)" \
./krudd.sh editor-qt
```

opens a `QMainWindow` — menu bar, toolbar, Scene/Inspector docks — with the
same animated clear as `krudd_window` running inside the embedded viewport.

## What this is (and isn't)

`krudd_qt` drives the same `gpu_api` vtable `krudd_window.c` does, directly,
with no scene graph — see docs/steamos-window.md's "What this is (and isn't)"
for why (it applies here unchanged). The only new surface area is Qt: a
`QMainWindow` chrome around a `QWindow` embedded via
`QWidget::createWindowContainer`, and a `webgpu_platform_host` that hands
Dawn that `QWindow`'s native handle instead of an SDL one.

**Deliberately moc-free.** Nothing in `krudd_qt.cpp` declares `Q_OBJECT`, a
signal, or a slot — every connection is to a Qt-supplied `QObject` (`qApp`,
a `QTimer`, a `QShortcut`) through a lambda, which Qt6 allows without a
generated moc file. That keeps the `(qt)` kruddmake clause's job to "add
`-I`/`-l` flags and compile a `.cpp` with a C++ compiler" — the same shape
as every other native target, with no moc rule anywhere in `ninja.scm`.

## Prerequisites

Same as docs/steamos-window.md (native Dawn built with a surface, a Vulkan
loader) plus Qt6:

```sh
sudo pacman -S --needed base-devel git cmake ninja python vulkan-icd-loader qt6-base
```

Qt has no default include path worth guessing at the way SDL's does, so
`KRUDD_QT_CFLAGS` (and usually `KRUDD_QT_LIBS`) must be set explicitly —
`krudd editor-qt` will tell you so and exit if they aren't. The recipe
above (`pkg-config --cflags/--libs Qt6Widgets Qt6Gui Qt6Core`) is normally
enough; override `KRUDD_QT_LIBS` only if your Qt install needs more than
`-lQt6Widgets -lQt6Gui -lQt6Core` to link.

## Build and run

```sh
KRUDD_DAWN_PREFIX=$HOME/dawn-native/install \
KRUDD_QT_CFLAGS="$(pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core)" \
./krudd.sh editor-qt
```

`editor-qt` sets `KRUDD_QT=1` for the build (so the `(qt)` target joins the
native graph), compiles through kruddmake, and runs `build/bin/krudd_qt`.
Close the window, or press **Esc**, to quit.

## Wayland: the hard case, and what is actually true about it today

#675 calls out Wayland/the Deck as the hard case, and asks for that
behaviour to be documented rather than assumed. Here is what is actually
verified, not guessed, as of writing this:

**X11 and Windows are straightforward.** `QWindow::winId()` returns the X11
window XID (or, on Windows, the HWND) directly — the exact same surface
handle Dawn's `WGPUSurfaceSourceXlibWindow`/`WGPUSurfaceSourceWindowsHWND`
want. `QNativeInterface::QX11Application::display()` (public since Qt 6.0)
supplies the X11 `Display*`. Both of these were exercised against a real
(Xvfb) X server while writing `krudd_qt.cpp`, not just read about.

**Wayland needs Qt >= 6.5, verified against a real install.** Getting a
`QWindow`'s `wl_surface`/`wl_display` through *stable public* API is
`QNativeInterface::QWaylandWindow` / `QNativeInterface::QWaylandApplication`,
added in Qt 6.5. Checked directly against Ubuntu 24.04's system Qt
(6.4.2, via `qt6-wayland-dev`): that API is not there. The only route on
6.4 is `QGuiApplication::platformNativeInterface()` reaching into
`QPlatformNativeInterface::nativeResourceForWindow()` — and that class ships
*only* under `qt6-base-private-dev`'s versioned, explicitly-unstable include
path (`.../qt6/QtGui/6.4.2/QtGui/qpa/qplatformnativeinterface.h`), not a
regular public header. `krudd_qt.cpp`'s Wayland path requires Qt >= 6.5 and
fails the build with an actionable `#error` on anything older, rather than
link the shipped harness against a private, ABI-unstable header by default.

SteamOS's Arch-based toolchain (docs/steamos-window.md's distrobox recipe)
tracks current Qt — Plasma 6 on the Deck runs Qt 6.6+ — so this is not
expected to bite on the actual target hardware. It is, however, a real
constraint worth keeping in mind if this is ever built against an older
distro's system Qt.

**Embedding is the part still genuinely open.** `QWidget::createWindowContainer`
— what puts the viewport *inside* the `QMainWindow`'s layout instead of as
its own top-level window — is built around X11-style window reparenting.
Whether it behaves acceptably on Wayland (where there is no equivalent
client-side reparenting mechanism) has not been exercised on real Wayland
hardware from here; this repo's sandbox has no GPU and no running
compositor to test against. If it does not, the fallback is to let the
viewport be its own top-level `QWindow` positioned under the chrome window
rather than a child of it — a real design decision, not a bug fix, and the
next thing to resolve on real Deck hardware.

## If a Dawn or Qt roll breaks something

- Dawn surface type renames: same as docs/steamos-window.md — the only code
  to touch is `window_create_surface` in `krudd/engine/core/krudd_qt.cpp`
  (its X11/Windows branches) or `krudd_window.c` (Wayland/X11 there).
- Qt native-interface renames: `window_create_surface`'s Wayland branch in
  `krudd_qt.cpp`, guarded by `QT_VERSION_CHECK(6, 5, 0)`.
