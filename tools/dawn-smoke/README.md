<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# dawn-smoke

The smallest thing that proves a **native Dawn** build works offscreen on this
box: create instance → adapter → device, clear a texture to a known colour, read
the pixels back, write a PNG.

It touches no engine code, and it is not part of `./krudd.sh build`. That is the
point — it has to be buildable *before* the engine build graph grows a Dawn edge,
so the build seam can be proven independently of the code that will use it.

Chunk 1 of `spec-dawn-native-build`. Chunk 2 (the platform split in
`renderer_webgpu.c`) is separate work.

## Why it lives in `tools/`

Same reasoning as `tools/render-diff`: a repo-level instrument, not engine
source, and the spec's bar is "the recipe reproduces from clean" — which means
the recipe has to be committed rather than living in someone's shell history.

## Pinned Dawn revision

```
31e25af254ab572c77054edec4946d2244e184dd
```

This is not an arbitrary pin. It is the **exact revision that the emsdk
`--use-port=emdawnwebgpu` port ships**, which is what `ninja.scm` builds the web
target with. Ground truth for that claim:

```
/data/gage/emsdk/upstream/emscripten/cache/ports/emdawnwebgpu/emdawnwebgpu_pkg/VERSION.txt
  -> Dawn release v20260423.175430 at revision 31e25af254ab572c77054edec4946d2244e184dd
```

Keeping the two in lockstep is the whole argument for the native build being a
debugger on the code we actually ship (see the spec's "Version skew" risk). If
the emsdk port is ever rolled, re-read that `VERSION.txt` and re-pin here.

## Building Dawn

Outside the repo, so it persists across worktrees and is never churned by a
build:

```sh
mkdir -p /data/gage/dawn-native && cd /data/gage/dawn-native
git init .
git remote add origin https://github.com/google/dawn.git
git fetch --depth 1 origin 31e25af254ab572c77054edec4946d2244e184dd
git checkout FETCH_HEAD

cmake -S . -B out/Release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/data/gage/dawn-native/install \
  -DDAWN_FETCH_DEPENDENCIES=ON \
  -DDAWN_ENABLE_VULKAN=ON \
  -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_OPENGLES=OFF -DDAWN_ENABLE_NULL=OFF \
  -DDAWN_USE_GLFW=OFF -DDAWN_USE_X11=OFF -DDAWN_USE_WAYLAND=OFF \
  -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_BUILD_BENCHMARKS=OFF \
  -DDAWN_BUILD_PROTOBUF=OFF \
  -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF -DTINT_BUILD_BENCHMARKS=OFF \
  -DDAWN_ENABLE_INSTALL=ON -DTINT_ENABLE_INSTALL=OFF \
  -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC -DBUILD_SHARED_LIBS=OFF

ninja -C out/Release webgpu_dawn
cmake --install out/Release
```

Notes on the flags that are not obvious:

- `DAWN_BUILD_MONOLITHIC_LIBRARY=STATIC` is what makes this tractable for a
  non-CMake consumer. It bundles Dawn + Tint + SPIRV-Tools + Abseil into a
  single `libwebgpu_dawn.a` exporting the `webgpu.h` C API, so kruddmake links
  **one** artifact instead of reproducing Dawn's dependency graph in Scheme.
- `TINT_ENABLE_INSTALL=OFF` is required, not cosmetic. With it ON, the install
  step tries to install per-target Tint archives that a `webgpu_dawn`-only build
  never produces, and `cmake --install` fails on the first missing one.
- No GLFW / X11 / Wayland, and no surface: this target renders offscreen only.
  Those dev packages are absent on this box and are deliberately not needed.
- Vulkan headers come from `DAWN_FETCH_DEPENDENCIES`, not the system. The system
  Vulkan **loader** (`libvulkan.so.1`) is what is used at runtime.

## Building and running the smoke binary

```sh
DAWN_PREFIX=/data/gage/dawn-native/install ./tools/dawn-smoke/build.sh
./tools/dawn-smoke/out/dawn-smoke out.png
```

Expected output ends in `dawn-smoke: OK`, having reported the requested colour,
the first pixel actually read back, and a mismatch count of `0/65536`. The
binary exits non-zero if any pixel disagrees, if any uncaptured validation error
fires, or if the device is lost — it is meant to be a trustworthy oracle, not a
thing that prints "done".

The clear colour is `(217, 119, 87, 255)`, chosen so every channel is exactly
`k/255` and an exact byte comparison on readback is a fair test rather than a
rounding argument.

## Building the engine's WebGPU backend natively

Chunk 2 landed the include half of the seam. With Dawn installed as above:

```sh
KRUDD_DAWN_PREFIX=/data/gage/dawn-native/install ./krudd.sh build
```

`renderer_webgpu.c` then compiles for the native target too, against the same
Dawn revision the web build uses.

**`KRUDD_DAWN_PREFIX` is opt-in on purpose.** Unset — which is how CI runs — any
`(dawn)` target is left out of the native graph entirely and `./krudd.sh build`
is byte-for-byte what it was before. Dawn is a ~38 MB out-of-tree artifact, so
making it a hard build dependency would break every checkout that has not built
it.

The WASM target ignores `(dawn)` entirely: there the headers arrive through
`--use-port=emdawnwebgpu`.

## How this maps onto a kruddmake native target (chunk 2)

kruddmake stays the build system; Dawn is only an external artifact that gets
linked. Concretely, the changes `ninja.scm` would need:

**1. Preamble variables** (`ninja-preamble`), with `dawnprefix` sourced from the
environment at generate time the way `KRUDD_ROOT` already is:

```
dawnprefix = /data/gage/dawn-native/install
dawnincludes = -I$dawnprefix/include
dawnlibs = $dawnprefix/lib/libwebgpu_dawn.a -ldl -lpthread -lm
```

**2. A C++-driver link rule**, alongside the existing `link`:

```
rule link_cxx
  command = $cxx $in $ldlibs -o $out
  description = LINK(c++) $out
```

`libwebgpu_dawn.a` is C++, so the final link needs a C++ driver (or an explicit
`-lstdc++`). This has to be a *separate* rule rather than flipping `link` over,
because the existing native `*_test` binaries are pure C and should not grow a
libstdc++ dependency for nothing.

**3. A `(dawn)` clause** in the `build.scm` vocabulary, read by ninja.scm:

- on a `library` — append `$dawnincludes` to that library's compile `includes`
  (this is what `renderer_webgpu` needs in order to see `<webgpu/webgpu.h>`
  natively);
- on an `executable` — same, plus emit the `link_cxx` rule instead of `link`
  and append `$dawnlibs` after the resolved syslibs in `ldlibs`.

The touch points are small: `ninja-emit-library` and `ninja-emit-executable`
already compute `includes` and `ldlibs` in one place each, and
`ninja-emit-form` already recurses into `native-only`, so a spec like

```scheme
(native-only
  (executable "krudd_native"
    (sources "engine_native.c")
    (dawn)
    (link "subsystem" "subsystem_manager" "log" "memory" "script"
          "renderer_webgpu" ...)))
```

needs no change to the form dispatcher at all.

**Link ordering already works out.** `ninja-emit-executable` emits `$in` as
objects-then-archives and `$ldlibs` last, so Dawn lands after the engine
archives — which is what static linking requires.

**Fetching.** The pin above should become a `krudd/third_party/dawn.artifact` +
`sync-dawn.sh` pair mirroring the existing `s7.artifact` / `sync.sh` pattern:
recorded revision, fetched on demand, never committed into the tree.

**Verified, so chunk 2 does not have to find it out the hard way:**
`<webgpu/webgpu.h>` compiles clean under krudd's exact native cflags
(`-std=gnu11 -Wall -Werror -Wpedantic`), so no per-target warning escape hatch is
needed for the header itself.
