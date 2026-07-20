<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# krudd in a native window (SteamOS / Steam Deck)

A proof of life for a future native editor: the engine's WebGPU backend booting
against **native Dawn** (Vulkan on the Deck's RDNA2) and presenting into a real
**SDL3 window on Wayland** — no browser, no Emscripten, nothing web in the path.

```sh
KRUDD_DAWN_PREFIX=/path/to/dawn-native/install ./krudd.sh editor
```

opens a window that clears to an animated colour every frame. That moving picture
*is* the deliverable: it means the whole native chain — window → Wayland surface →
Dawn → swapchain acquire → render pass → present — is live on the hardware.

## One-shot (copy-paste)

SteamOS's root filesystem is immutable, so do this inside an Arch
[distrobox](https://distrobox.it/) (it shares the Deck's Wayland socket and GPU).
Create and enter one first — this line is separate because `enter` drops you into
a new shell:

```sh
distrobox create -i archlinux:latest krudd && distrobox enter krudd
```

Then paste the whole block below. It installs the toolchain, builds native Dawn
once (pinned, with a Wayland/X11 surface — the ~38 MB step, skipped on re-runs),
clones the engine at this PR's branch, and launches the window. Re-runnable.

```sh
set -e

# 1. Toolchain + SDL3 + the Vulkan loader.
sudo pacman -S --needed --noconfirm \
  base-devel git cmake ninja python sdl3 vulkan-icd-loader

# 2. Native Dawn, pinned to the emsdk port's revision, built WITH a surface.
DAWN_REV=31e25af254ab572c77054edec4946d2244e184dd
DAWN=~/dawn-native
if [ ! -f "$DAWN/install/lib/libwebgpu_dawn.a" ]; then
  mkdir -p "$DAWN" && cd "$DAWN"
  [ -d .git ] || git init -q .
  git remote add origin https://github.com/google/dawn.git 2>/dev/null || true
  git fetch --depth 1 origin "$DAWN_REV" && git checkout -q FETCH_HEAD
  cmake -S . -B out/Release -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$DAWN/install" \
    -DDAWN_FETCH_DEPENDENCIES=ON -DDAWN_ENABLE_VULKAN=ON \
    -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_OPENGLES=OFF -DDAWN_ENABLE_NULL=OFF \
    -DDAWN_USE_WAYLAND=ON -DDAWN_USE_X11=ON -DDAWN_USE_GLFW=OFF \
    -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_BUILD_BENCHMARKS=OFF \
    -DDAWN_BUILD_PROTOBUF=OFF -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF \
    -DTINT_BUILD_BENCHMARKS=OFF -DDAWN_ENABLE_INSTALL=ON -DTINT_ENABLE_INSTALL=OFF \
    -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC -DBUILD_SHARED_LIBS=OFF
  ninja -C out/Release webgpu_dawn && cmake --install out/Release
fi

# 3. The engine — clone at this PR's branch, then build + run the window.
cd ~
[ -d engine ] || git clone https://github.com/kruddage/engine.git
cd engine
git fetch -q origin claude/steamos-desktop-poc-wyh0yc
git checkout claude/steamos-desktop-poc-wyh0yc
KRUDD_DAWN_PREFIX="$DAWN/install" ./krudd.sh editor
```

Once this PR is merged, drop the two `git fetch`/`git checkout` lines and just
build `main`. The rest of this doc is the same steps, explained.

## What this is (and isn't)

The same C engine that ships to the browser as WebAssembly already has a native
seam: `renderer_webgpu.c` compiles against Dawn's own `libwebgpu_dawn.a` at the
exact revision the emsdk port ships (`tools/dawn-smoke/README.md`), and
`krudd_native` renders it **offscreen** to a PNG so CI can trust it with no GPU.

`krudd_window` is that same backend with the offscreen target swapped for a live
window. All the windowing lives behind the existing platform seam
(`webgpu_platform.h`): the harness registers a host that hands the backend the
window's `WGPUSurface`, and the backend's normal surface path — the one the
browser canvas uses — does the rest. The offscreen path is untouched: with no
host registered (every CI build, every `krudd_native` run) the seam is byte-for-
byte the offscreen one.

**Scope is deliberately narrow.** This does *not* run the full site. `engine.c`'s
boot is Emscripten-only and pulls in the whole plugin table (the IndexedDB-backed
asset store, the canvas UI, `fetch`). Porting that off the browser is the editor
work this is a proof of life *for*. So the window drives the backend directly
through the `gpu_api` vtable — an animated clear — which exercises surface
configuration, per-frame acquire, a render pass, submit and present, and nothing
that still assumes a browser. Rendering the actual scene natively is the next
step, not this one.

## Prerequisites

You need three things on the machine: **native Dawn built with surface support**,
**SDL3**, and a **Vulkan loader**. SteamOS's root filesystem is immutable, so the
path of least resistance is a [distrobox](https://distrobox.it/) /
[toolbox](https://containertoolbx.org/) Arch container for the build tools — build
inside it, and run from inside it too (it shares the Deck's Wayland socket and
GPU).

In Desktop Mode, in an Arch-based container (or any Arch/Linux dev box):

```sh
sudo pacman -S --needed base-devel git cmake ninja python sdl3 vulkan-icd-loader
```

Game Mode (gamescope) and Desktop Mode (KDE Plasma) are both Wayland, so Wayland
is the primary target; X11/XWayland is kept as a fallback.

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
-DDAWN_USE_X11=ON` (were `OFF`), and `-DDAWN_USE_GLFW=OFF` stays off — we get our
surface from SDL, not GLFW. `DAWN_BUILD_MONOLITHIC_LIBRARY=STATIC` still bundles
Dawn + Tint + SPIRV-Tools + Abseil into one `libwebgpu_dawn.a`, so there is no
runtime Dawn dependency to ship — the binary only dynamically needs `libSDL3` and
the Vulkan loader.

## Build and run

Clone the engine into your home folder on the Deck and, from inside the container:

```sh
KRUDD_DAWN_PREFIX=$HOME/dawn-native/install ./krudd.sh editor
```

`editor` sets `KRUDD_SDL=1` for the build (so the window target joins the native
graph), compiles through kruddmake, and runs `build/bin/krudd_window`. Close the
window or press **Esc** to quit.

If SDL lives off the default paths, point the build at it:

```sh
KRUDD_DAWN_PREFIX=$HOME/dawn-native/install \
KRUDD_SDL_CFLAGS="$(pkg-config --cflags sdl3)" \
KRUDD_SDL_LIBS="$(pkg-config --libs sdl3)" \
./krudd.sh editor
```

### What you should see

```
krudd_window: SDL video driver = wayland
krudd_window: device ready after N tick(s)
krudd_window: presenting — close the window or press Esc to quit
```

and a 1280×800 window (the Deck's native panel) cycling through colours. If the
driver line says `x11` you are on XWayland — that path works too, via the X11
fallback surface.

## How it stays out of everyone else's way

Two independent opt-in gates, mirroring how `(dawn)` already worked:

- **No `KRUDD_DAWN_PREFIX`** (every CI run, every default checkout): the whole
  `(dawn)` graph — including this window target — is left out. `./krudd.sh build`
  is byte-for-byte unchanged.
- **`KRUDD_DAWN_PREFIX` but no `KRUDD_SDL`** (someone who only wants the offscreen
  `krudd_native`): the `(sdl)` window target is skipped, so no SDL install is
  required and `krudd_native` builds exactly as before.

Only `./krudd.sh editor` (or setting `KRUDD_SDL` yourself) pulls the window into
the build.

## If a Dawn roll breaks the surface types

The Wayland/X11 surface descriptors — `WGPUSurfaceSourceWaylandSurface` /
`WGPUSurfaceSourceXlibWindow` and their `WGPUSType_*` tags — track Dawn's
`webgpu.h` at the pinned revision. If the pin is ever rolled and these are
renamed, the only code to touch is `window_create_surface` in
`krudd/engine/core/krudd_window.c`; check the new names against
`$KRUDD_DAWN_PREFIX/include/webgpu/webgpu.h`.
