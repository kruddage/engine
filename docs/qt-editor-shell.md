<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# krudd's native editor — a Qt editor shell (#675, #676)

A proof of life for the native editor: the engine's WebGPU backend booting
against **native Dawn** (Vulkan on the Deck's RDNA2) and presenting into a
`QWindow` embedded in real Qt chrome — a menu bar, a toolbar, docks — on the
desktop (SteamOS / the Steam Deck), no browser, no Emscripten, nothing web in
the path.

```sh
KRUDD_DAWN_PREFIX=/path/to/dawn-native/install \
KRUDD_QT_CFLAGS="$(pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core)" \
./krudd.sh editor
```

opens a `QMainWindow` — the File/Edit/View/Help menu bar, a toolbar, and the
Scene / Inspector / Assets / Console docks — with an animated clear running
inside the embedded viewport. That moving picture *is* the deliverable: it means
the whole native chain — window → Wayland surface → Dawn → swapchain acquire →
render pass → present — is live on the hardware.

## The authoring surface (#676)

The chrome around the viewport is the authoring surface #676 asks for, **laid
out but not yet wired**:

- A **File / Edit / View / Help** menu bar like a normal desktop app. File
  carries New / Open / Save / Save As / Quit; Edit carries the usual
  Undo/Redo/Cut/Copy/Paste; Help has About. Everything that isn't wired yet
  reports "— coming soon" in the status bar rather than doing nothing.
- Four docks around the viewport — **Scene** (scene tree), **Inspector**
  (entity properties), **Assets** (asset browser) and **Console** (the live
  Scheme REPL). Each panel's *contents* are a "coming soon" placeholder;
  wiring them to the running image (scene graph, inspector edits, live REPL,
  project open/save) is the rest of #676.
- The docks are **fully reconfigurable**: movable, floatable, closable,
  tabbable and nestable in any dock area (`setDockNestingEnabled` +
  `AllowNestedDocks | AllowTabbedDocks | GroupedDragging`). Drag one out to
  float it, drop it onto another to tab them, split it into any edge. **View
  ▸ Reset Layout** restores the default arrangement (captured with
  `saveState`/`saveGeometry` once the window is shown), and the View menu's
  dock toggles bring a closed panel back.

Still **moc-free**: every action and dock toggle is a lambda or a Qt-supplied
`QAction`, so the `(qt)` kruddmake clause stays a plain compile-and-link with
no moc rule.

## One-shot (`setup.sh`)

There is a committed bootstrap at the repo root that does everything the editor
build needs — installs the toolchain (compiler, cmake, ninja, Qt6, a Vulkan
loader), builds pinned native Dawn once (with a Wayland/X11 surface — the ~38 MB
step, skipped on re-runs), and writes `.krudd-env` so `./krudd.sh editor` finds
Dawn and Qt with no manual exports. It is re-runnable: each step is skipped when
its result is already there.

SteamOS's root filesystem is immutable, so the toolchain lives in an Arch
[distrobox](https://distrobox.it/) (it shares the Deck's Wayland socket and GPU).
Create and enter one first — this line is separate because `enter` drops you into
a new shell:

```sh
distrobox create -i archlinux:latest krudd && distrobox enter krudd
```

Then, inside the container:

```sh
git clone https://github.com/kruddage/engine.git
cd engine
./setup.sh          # toolchain + pinned Dawn + .krudd-env  (the ~38 MB step)
./krudd.sh editor   # builds and runs the Qt editor shell
```

`setup.sh` detects the package manager (pacman on the Deck, apt/dnf elsewhere),
so the same two commands also work on a normal Linux dev box. If you run it on
the bare SteamOS host by mistake it stops and prints the `distrobox` lines above
rather than failing on the immutable filesystem.

The rest of this doc is the same steps, explained — reach for them when you want
to understand or tweak what `setup.sh` automates.

## What this is (and isn't)

The same C engine that ships to the browser as WebAssembly already has a native
seam: `renderer_webgpu.c` compiles against Dawn's own `libwebgpu_dawn.a` at the
exact revision the emsdk port ships (`tools/dawn-smoke/README.md`), and
`krudd_native` renders it **offscreen** to a PNG so CI can trust it with no GPU.

`krudd_qt` is that same backend with the offscreen target swapped for a live
window. All the windowing lives behind the existing platform seam
(`webgpu_platform.h`): the harness registers a host that hands the backend the
window's `WGPUSurface`, and the backend's normal surface path — the one the
browser canvas uses — does the rest. The offscreen path is untouched: with no
host registered (every CI build, every `krudd_native` run) the seam is byte-for-
byte the offscreen one. The only new surface area is Qt: a `QMainWindow` chrome
around a `QWindow` embedded via `QWidget::createWindowContainer`, and a
`webgpu_platform_host` that hands Dawn that `QWindow`'s native handle.

**Scope is deliberately narrow.** This does *not* run the full site. `engine.c`'s
boot is Emscripten-only and pulls in the whole plugin table (the IndexedDB-backed
asset store, the canvas UI, `fetch`). Porting that off the browser is the editor
work this is a proof of life *for*. So the viewport drives the backend directly
through the `gpu_api` vtable — an animated clear — which exercises surface
configuration, per-frame acquire, a render pass, submit and present, and nothing
that still assumes a browser. Rendering the actual scene through this shell is
the next step, not this one.

## Prerequisites

You need three things on the machine: **native Dawn built with surface support**,
**Qt6**, and a **Vulkan loader**. SteamOS's root filesystem is immutable, so the
path of least resistance is a [distrobox](https://distrobox.it/) /
[toolbox](https://containertoolbx.org/) Arch container for the build tools — build
inside it, and run from inside it too (it shares the Deck's Wayland socket and
GPU).

In Desktop Mode, in an Arch-based container (or any Arch/Linux dev box):

```sh
sudo pacman -S --needed base-devel git cmake ninja python vulkan-icd-loader qt6-base
```

Game Mode (gamescope) and Desktop Mode (KDE Plasma) are both Wayland, so Wayland
is the primary target; X11/XWayland is kept as a fallback.

Qt has no default include path worth guessing at the way a system library on the
default search path does, so `KRUDD_QT_CFLAGS` (and usually `KRUDD_QT_LIBS`) must
be set explicitly — `krudd editor` will tell you so and exit if they aren't. The
recipe `pkg-config --cflags/--libs Qt6Widgets Qt6Gui Qt6Core` is normally
enough; override `KRUDD_QT_LIBS` only if your Qt install needs more than
`-lQt6Widgets -lQt6Gui -lQt6Core` to link.

## Building Dawn *with a surface*

This differs from the `tools/dawn-smoke` recipe in one respect: that one is
offscreen-only and explicitly turns Wayland/X11 **off**. For a window you turn
them **on**. Everything else — the pin, the monolithic-static packaging — is the
same, and the pin **must** stay the one `tools/dawn-smoke/README.md` records (the
native build is only a faithful debugger of the shipped web build if the Dawn
revision matches the emsdk port's).

Build it outside the repo so it survives worktrees and is never churned by a
build:

```sh
mkdir -p ~/dawn-native && cd ~/dawn-native
git init .
git remote add origin https://github.com/google/dawn.git
# The pinned revision — keep in lockstep with tools/dawn-smoke/README.md.
git fetch --depth 1 origin 31e25af254ab572c77054edec4946d2244e184dd
git checkout FETCH_HEAD

cmake -S . -B out/Release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=$HOME/dawn-native/install \
  -DDAWN_FETCH_DEPENDENCIES=ON \
  -DDAWN_ENABLE_VULKAN=ON \
  -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_OPENGLES=OFF -DDAWN_ENABLE_NULL=OFF \
  -DDAWN_USE_WAYLAND=ON -DDAWN_USE_X11=ON \
  -DDAWN_USE_GLFW=OFF \
  -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_BUILD_BENCHMARKS=OFF \
  -DDAWN_BUILD_PROTOBUF=OFF \
  -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF -DTINT_BUILD_BENCHMARKS=OFF \
  -DDAWN_ENABLE_INSTALL=ON -DTINT_ENABLE_INSTALL=OFF \
  -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC -DBUILD_SHARED_LIBS=OFF

ninja -C out/Release webgpu_dawn
cmake --install out/Release
```

The only changes from the offscreen recipe: `-DDAWN_USE_WAYLAND=ON
-DDAWN_USE_X11=ON` (were `OFF`), and `-DDAWN_USE_GLFW=OFF` stays off — Qt owns
the window and hands Dawn its native surface, so Dawn needs no windowing library
of its own. `DAWN_BUILD_MONOLITHIC_LIBRARY=STATIC` still bundles Dawn + Tint +
SPIRV-Tools + Abseil into one `libwebgpu_dawn.a`, so there is no runtime Dawn
dependency to ship — the binary only dynamically needs Qt6 and the Vulkan loader.

## Build and run

```sh
KRUDD_DAWN_PREFIX=$HOME/dawn-native/install \
KRUDD_QT_CFLAGS="$(pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core)" \
./krudd.sh editor
```

`editor` sets `KRUDD_QT=1` for the build (so the `(qt)` target joins the native
graph), compiles through kruddmake, and runs `build/bin/krudd_qt`. Close the
window, or press **Esc**, to quit.

(`editor-qt` is kept as a back-compat alias for `editor`; both build and run the
Qt shell.)

### What you should see

```
krudd_qt: presenting on 'wayland' — close the window or press Esc to quit
```

and a window cycling through colours in the embedded viewport. If the platform
line says `xcb` you are on XWayland — that path works too, via the X11 fallback
surface.

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

SteamOS's Arch-based toolchain (the distrobox recipe above) tracks current Qt —
Plasma 6 on the Deck runs Qt 6.6+ — so this is not expected to bite on the actual
target hardware. It is, however, a real constraint worth keeping in mind if this
is ever built against an older distro's system Qt.

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

## How it stays out of everyone else's way

Two independent opt-in gates, mirroring how `(dawn)` already worked:

- **No `KRUDD_DAWN_PREFIX`** (every CI run, every default checkout): the whole
  `(dawn)` graph — including this editor target — is left out. `./krudd.sh build`
  is byte-for-byte unchanged.
- **`KRUDD_DAWN_PREFIX` but no `KRUDD_QT`** (someone who only wants the offscreen
  `krudd_native`): the `(qt)` editor target is skipped, so no Qt install is
  required and `krudd_native` builds exactly as before.

Only `./krudd.sh editor` (or setting `KRUDD_QT` yourself) pulls the editor into
the build.

## If a Dawn or Qt roll breaks something

- Dawn surface type renames: the `WGPUSurfaceSource*` structs and their
  `WGPUSType_*` tags track Dawn's `webgpu.h` at the pinned revision. If the pin
  is ever rolled and these are renamed, the only code to touch is
  `window_create_surface` in `krudd/engine/core/krudd_qt.cpp`; check the new
  names against `$KRUDD_DAWN_PREFIX/include/webgpu/webgpu.h`.
- Qt native-interface renames: `window_create_surface`'s Wayland branch in
  `krudd_qt.cpp`, guarded by `QT_VERSION_CHECK(6, 5, 0)`.
