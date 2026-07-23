<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# krudd's native editor — a Qt editor shell on Vulkan (#675, #676, #705)

The native editor: the engine's **native Vulkan** backend presenting into a
`QWindow` embedded in real Qt chrome — a menu bar, a toolbar, docks — on the
desktop (SteamOS / the Steam Deck / Windows), no browser, no Emscripten, nothing
web in the path. The web build keeps its WebGL and WebGPU backends untouched;
Vulkan is the native desktop GPU path.

```sh
KRUDD_QT_CFLAGS="$(pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core)" \
./krudd.sh editor
```

opens a `QMainWindow` — the File/Edit/View/Help menu bar, a toolbar, and the
Scene / Inspector / Assets / Console docks — with the Vulkan viewport rendering
inside the embedded window.

## What renders today (and what's next) — #705

This stage stands up a **modern, validated Vulkan base** and presents into the
window, but it does not yet translate the engine's draw stream:

- **Real:** a Vulkan 1.3 instance with the **Khronos validation layer** and a
  `VK_EXT_debug_utils` messenger wired straight into the engine log, a
  physical/logical device and queue, a `VkSurfaceKHR` + swapchain from the
  window, and a genuine per-frame **acquire → clear → present** through command
  buffers, semaphores and fences (dynamic rendering — no render-pass objects).
  Everything a validation layer has an opinion about actually runs, so when you
  launch it on real hardware the layer's diagnostics are live and point at real
  calls. That is the whole point of landing it in this shape — get the engine
  onto a current Vulkan device with the layers on, so the real renderer is built
  and debugged against a loader that already talks back.
- **Stubbed:** the `gpu_api` draw path — pipeline creation, the `cmd_*`
  recording verbs, buffer/texture creation — are honest no-ops. The editor still
  boots the engine's render cluster (`editor_boot_cluster()`: asset → entity →
  frame_graph → scene_renderer), so scene_renderer seeds and records its
  built-in demo scene, but the backend does not consume those calls yet. The
  viewport therefore shows an **animated clear**, not the scene.

Turning that clear into a real forward pass — GLSL/krudd-DSL shaders lowered to
SPIR-V (glslang is installed for exactly this), pipelines, vertex/index/uniform
buffers, textures and draws — is the follow-up #705 scopes out of this pass.

## The authoring surface (#676)

The chrome around the viewport is the authoring surface #676 asks for, **laid
out but not yet wired**:

- A **File / Edit / View / Help** menu bar like a normal desktop app. File
  carries New / Open / Save / Save As / Quit; Edit carries the usual
  Undo/Redo/Cut/Copy/Paste; Help has About. Everything that isn't wired yet
  reports "— coming soon" in the status bar rather than doing nothing.
- Four docks around the viewport — **Scene** (scene tree), **Inspector**
  (entity properties), **Assets** (asset browser) and **Console** (the live
  Scheme REPL). **Scene** and **Inspector** are wired to the running image,
  both read-only: Scene is a live tree of the entity hierarchy that shares the
  viewport's selection (click a node to select it; a viewport pick highlights
  it back), and Inspector shows the selected entity's name, id, parent,
  components and local transform (updating live as a scripted entity animates).
  **Assets** and **Console** are still "coming soon" placeholders. Which live
  panel a dock hosts is authored in the shared layout spec as data —
  `(panel-kind scene-tree)` / `(panel-kind inspector)` in
  `core/editor_layout.scm` — so the host builds the matching widget from the
  declared kind rather than hard-coding a dock id, and the **same kind crosses
  into the web chrome's JSON** (via `script_layout_json`) untouched: the web
  editor already receives the panel identity and only needs to render its own
  body later (#706 part C). The remaining #676 work is write-back — Inspector
  edits pushed through `set_transform`/`set_name` — plus the asset browser, the
  live REPL and project save.
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
build needs — installs the toolchain (compiler, ninja, Qt6, and the Vulkan
loader + headers + validation layers + glslang), and writes `.krudd-env` so
`./krudd.sh editor` finds Qt with no manual exports. It is re-runnable: each step
is skipped when its result is already there.

Unlike the previous native-Dawn setup, there is **no multi-gigabyte out-of-tree
GPU library to build** any more: the Vulkan loader and validation layers are
ordinary system packages, so the whole bootstrap is a single package install.

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
./setup.sh          # toolchain + Vulkan validation layers + Qt6 + .krudd-env
./krudd.sh editor   # builds and runs the Qt editor shell
```

`setup.sh` detects the package manager (pacman on the Deck, apt/dnf elsewhere),
so the same two commands also work on a normal Linux dev box. If you run it on
the bare SteamOS host by mistake it stops and prints the `distrobox` lines above
rather than failing on the immutable filesystem.

The rest of this doc is the same steps, explained — reach for them when you want
to understand or tweak what `setup.sh` automates.

## What this is (and isn't)

The same C engine that ships to the browser as WebAssembly gains a fourth
`gpu_api` backend beside WebGL, WebGPU and the recording null renderer:
`krudd/engine/render/vulkan/renderer_vulkan.c`, registered as the `"renderer"`
subsystem exactly like the others. `krudd_qt` is the Qt host that drives it into
a live window.

All the windowing lives behind a platform seam (`vulkan_platform.h`), the direct
analogue of the WebGPU backend's `webgpu_platform.h`. The harness registers a
host that injects the two things the backend cannot know without learning the
windowing system:

1. **Which instance extensions to enable** (`VK_KHR_xcb_surface` and friends),
   read while the instance is being created. Without them the surface call in
   step 2 fails `VK_ERROR_EXTENSION_NOT_PRESENT`.
2. **The `VkSurfaceKHR` itself**, built from the `QWindow`'s native handle at
   device bring-up.

The backend's swapchain path does the rest. With no host registered (a headless
build) it still stands up an instance and device — so validation bring-up is
exercised — it just enables no surface extension and has no swapchain to present
into.

Because the extension list is read *during instance creation*, the host must be
registered before the backend boots. The instance log line reports how many
extensions came from the host, so a host that registered too late is visible
immediately:

```
renderer_vulkan: instance up (Vulkan 1.3 requested, 4 extensions [2 from window host], validation off)
```

`0 from window host` with a window open means presentation is already lost.

The only new surface area is Qt: a `QMainWindow` chrome around a `QWindow`
embedded via `QWidget::createWindowContainer`, and a `vulkan_platform_host` that
hands the backend that `QWindow`'s native Wayland/X11/Win32 handle.

**Verified without a GPU.** The whole boot-and-record path up to the backend is
exercised in ordinary CI by `editor_boot_test` (native-only, no GPU): it runs
the exact same `editor_boot_cluster()` against the recording **null** backend
and asserts the seeded demo scene emits one draw per live mesh+material entity.
So the scene assembly, the built-in catalog, the s7 mesh generation and the
forward pass are all covered by a test; and the editor-linux CI job compiles and
links `krudd_qt` itself (see [CI](#ci) below). Only the Vulkan present glue needs
real hardware to *see*.

## Prerequisites

You need three things on the machine: the **Vulkan loader + headers + validation
layers**, **glslang**, and **Qt6**. SteamOS's root filesystem is immutable, so
the path of least resistance is a [distrobox](https://distrobox.it/) /
[toolbox](https://containertoolbx.org/) Arch container for the build tools — build
inside it, and run from inside it too (it shares the Deck's Wayland socket and
GPU).

In Desktop Mode, in an Arch-based container (or any Arch/Linux dev box):

```sh
sudo pacman -S --needed base-devel git ninja qt6-base \
  vulkan-icd-loader vulkan-headers vulkan-validation-layers glslang
```

You also need a **Vulkan driver** for your GPU (the Deck already has one:
`vulkan-radeon`). Game Mode (gamescope) and Desktop Mode (KDE Plasma) are both
Wayland, so Wayland is the primary target; X11/XWayland is kept as a fallback.

Qt has no default include path worth guessing at the way a system library on the
default search path does, so `KRUDD_QT_CFLAGS` (and usually `KRUDD_QT_LIBS`) must
be set explicitly — `krudd editor` will tell you so and exit if they aren't. The
recipe `pkg-config --cflags/--libs Qt6Widgets Qt6Gui Qt6Core` is normally
enough; override `KRUDD_QT_LIBS` only if your Qt install needs more than
`-lQt6Widgets -lQt6Gui -lQt6Core` to link.

The Vulkan loader (`-lvulkan`) and its headers are on the default library and
include paths, so nothing has to be exported for them — the build just links
`-lvulkan`. If your loader lives in a non-standard prefix (a hand-built Vulkan
SDK), set `KRUDD_VULKAN_CFLAGS` / `KRUDD_VULKAN_LIBS` to point at it.

## Build and run

```sh
KRUDD_QT_CFLAGS="$(pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core)" \
./krudd.sh editor
```

`editor` sets `KRUDD_VULKAN=1` and `KRUDD_QT=1` for the build (so the `(vulkan)`
backend and `(qt)` window targets join the native graph), compiles through
kruddmake, and runs `build/bin/krudd_qt`. Close the window, or press **Esc**, to
quit.

(`editor-qt` is kept as a back-compat alias for `editor`; both build and run the
Qt shell.)

### What you should see

```
krudd_qt: presenting on 'wayland' — close the window or press Esc to quit
```

and an **animated clear** — a slow blue↔teal pulse — filling the embedded
viewport (see [What renders today](#what-renders-today-and-whats-next--705)).
The toolbar badge reads `Vulkan · native · wayland`. If the platform line says
`xcb` you are on XWayland — that path works too, via the X11/xcb fallback
surface.

If the Vulkan **validation layers** are installed, their diagnostics stream into
the engine log with a `vulkan(validation):` prefix — that is the whole reason
this stage exists, so anything the layer flags shows up plainly. If the layer
package is missing the backend logs a one-line warning and runs without it.

## Wayland: the hard case, and what is actually true about it today

#675 calls out Wayland/the Deck as the hard case, and asks for that behaviour to
be documented rather than assumed. The surface handles are the same Qt native
interfaces the previous native backend used; only the Vulkan call they feed is
new:

**X11 and Windows are straightforward.** `QWindow::winId()` returns the X11
window XID (or, on Windows, the HWND) directly, and
`QNativeInterface::QX11Application::connection()` (public since Qt 6.0) supplies
the `xcb_connection_t*` — exactly what `vkCreateXcbSurfaceKHR` /
`vkCreateWin32SurfaceKHR` want.

**Wayland needs one private header.** The `wl_display` half is public and easy:
`QNativeInterface::QWaylandApplication` (Qt 6.5+) hands it over. The per-window
`wl_surface` half has **no public API at all** — `QNativeInterface` has no
`QWaylandWindow` in any Qt 6 release, contrary to what an earlier draft of this
document claimed. Verified against Qt 6.8.2 (Debian 13) by grepping the full
install, `qt6-wayland-dev` included: the symbol does not exist.

The only route is the QPA native-resource lookup —
`QGuiApplication::platformNativeInterface()->nativeResourceForWindow("surface",
window)` — which lives in `QtGui/qpa/qplatformnativeinterface.h`, a *private*,
ABI-unstable header. `krudd_qt.cpp` uses it deliberately and keeps the blast
radius to one call whose result is null-checked like any other handle. Because
it is ABI-unstable, the Qt the editor links against must be the same Qt it was
built against — which is already true of every build path here (setup.sh uses
the system Qt, the flatpak builds against `org.kde.Sdk`'s own Qt, CI pins 6.8).

The practical cost is a build dependency: the private headers ship separately on
Debian (`qt6-base-private-dev`) and Fedora (`qt6-qtbase-private-devel`), and
inside `qt6-base` on Arch. `setup.sh` installs them and warns if they are
missing; CI and the flatpak add the `-I` explicitly, since `pkg-config` reports
only the public include dirs.

**No heavy native headers.** The per-platform Vulkan structs take pointers to
incomplete types (`wl_display`, `wl_surface`, `xcb_connection_t`) plus a single
`xcb_window_t` (a `uint32_t`) alias, so `krudd_qt.cpp` selects
`VK_USE_PLATFORM_WAYLAND_KHR` / `VK_USE_PLATFORM_XCB_KHR` without pulling
`<X11/Xlib.h>` or `<wayland-client.h>` into a translation unit that also includes
Qt — no macro collisions to fight.

**Embedding is the part still genuinely open.** `QWidget::createWindowContainer`
— what puts the viewport *inside* the `QMainWindow`'s layout — is built around
X11-style window reparenting; whether it behaves acceptably on Wayland has not
been exercised on real Deck hardware from here (this repo's sandbox has no GPU
and no compositor). If it does not, the fallback is to let the viewport be its
own top-level `QWindow` positioned under the chrome window — a real design
decision to resolve on real hardware, not a bug fix.

## CI

The `editor-linux` job in `.github/workflows/ci.yml` builds `krudd_qt`
(Vulkan + Qt) on every PR — **compile + link only**, never run, since it opens a
window and needs a real GPU. It installs the Vulkan loader/headers/validation
layers and glslang from apt, and Qt 6.8 via `install-qt-action` (the runner's
system Qt is 6.4, older than the 6.5 `QWaylandApplication` needs), then builds
just the `bin/krudd_qt` target. The web build (WebGL + WebGPU) is covered by the separate
`build` job and is untouched — Vulkan is native only.

## How it stays out of everyone else's way

Two independent opt-in gates, mirroring how `(dawn)`/`(qt)` already worked:

- **No `KRUDD_VULKAN`** (every default checkout, every default CI run): the whole
  `(vulkan)` graph — the backend library and this editor target — is left out.
  `./krudd.sh build` is byte-for-byte unchanged, and no Vulkan headers are
  needed to build the engine or the web target.
- **`KRUDD_VULKAN` but no `KRUDD_QT`**: the `(qt)` editor target is skipped, so
  no Qt install is required to build the backend library on its own.

Only `./krudd.sh editor` (or setting `KRUDD_VULKAN`/`KRUDD_QT` yourself) pulls
the editor into the build.

## If a Qt roll breaks something

- Qt native-interface renames: `window_create_surface` in
  `krudd/engine/core/krudd_qt.cpp` is the one place the Qt→Vulkan surface glue
  lives (its Wayland branch is guarded by `QT_VERSION_CHECK(6, 5, 0)`).
- A new target platform: add its `VK_USE_PLATFORM_*` selection at the top of
  `krudd_qt.cpp`, its `VK_KHR_*_surface` entry in `k_instance_extensions`, and a
  matching branch in `window_create_surface` — those three are the only places
  that know about a specific windowing system, and all three are in that one
  file. Miss the extension entry and the surface branch cannot succeed.
