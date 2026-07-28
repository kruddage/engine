#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# kruddmake test harness.
#
# Stage 0+1 (always): run-scheme-tests.sh — kruddmake's own suite, and
# @kruddage/kruddmake's `test` script. The codegen/embed helpers
# (introspect_test.scm) and the resolver + emitter (resolve_test.scm), on the
# fetched s7 CLI alone, no compiler. It also renders the build.ninja stage 2
# builds, which is why KRUDD_NINJA_OUT is exported before it runs.
#
# Stage 2 (when ninja + cc are present): render the real manifest to a
# build.ninja and build the `native` target with ninja(1). Each test links and
# runs as a run_test stamp, so a green `ninja native` means the whole native
# suite passed through the generated build — the proof the Ninja path is
# equivalent to the CMake one for native builds.
set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
export KRUDD_ROOT="$root"

# Fetches + verifies the pinned s7 artifacts and exports S7_CLI — the standalone
# s7 interpreter (krudds7) shipped by the kruddage/s7 release. `krudds7 FILE`
# loads and runs a Scheme file, which is exactly what the bootstrap/oracle
# stages below need, so we use it directly instead of building our own.
. "$root/krudd/third_party/sync.sh"

# KRUDD_BUILD_DIR lets a caller (e.g. the sanitizer / coverage CI jobs) point
# the generated build.ninja and its objects at a variant-specific directory so
# an instrumented build never reuses the plain build's object files. Defaults
# to build-ninja for a normal local run.
work="${KRUDD_BUILD_DIR:-$root/build-ninja}"
mkdir -p "$work"

s7bin="$S7_CLI"

# Stage 0+1: kruddmake's own suite. S7_CLI is already exported above, so the
# fetch is not repeated; KRUDD_NINJA_OUT names the build.ninja stage 2 wants.
export KRUDD_NINJA_OUT="$work/build.ninja"
sh "$root/krudd/kruddmake/run-scheme-tests.sh"

# Stage 1b: the monolang reference oracle — evaluate math.scm's (define-c-fn)
# bodies in s7 and check the numbers, the same math the generated C is checked
# for by engine/base/math/math_test.c in the native stage below.
"$s7bin" "$root/krudd/engine/base/math/math_test.scm"

# Stage 1c: the shader DSL oracle — run shader.scm's transpiler in s7 and check
# the GLSL it lowers the built-in shaders to, the same transpiler embedded into
# the runtime image and exercised natively by engine/core/shader_transpile_test.c.
"$s7bin" "$root/krudd/engine/render/shader/shader_test.scm"

# Stage 1d: the shader DSL's WGSL oracle — the same transpiler lowered to WGSL
# for the WebGPU backend. When naga(1) (the wgpu WGSL front end) is installed,
# also dump the emitted WGSL and validate it for real; otherwise run the
# structural oracle alone. naga is a dev convenience, not a CI dependency.
if command -v naga >/dev/null 2>&1; then
	wgsldir="$work/wgsl"
	rm -rf "$wgsldir"
	mkdir -p "$wgsldir"
	KRUDD_WGSL_DUMP="$wgsldir" \
		"$s7bin" "$root/krudd/engine/render/shader/shader_wgsl_test.scm"
	echo "kruddmake: validating emitted WGSL with naga"
	for f in "$wgsldir"/*.wgsl; do
		naga "$f" >/dev/null
		echo "  valid $(basename "$f")"
	done
	echo "kruddmake: WGSL naga validation OK"
else
	"$s7bin" "$root/krudd/engine/render/shader/shader_wgsl_test.scm"
	echo "kruddmake: naga(1) not found — WGSL structural oracle only"
fi

# Stage 2: build + run the native suite through the generated build.ninja.
if command -v ninja >/dev/null 2>&1; then
	echo "kruddmake: building native suite via ninja"
	ninja -C "$work" -f build.ninja native
	echo "kruddmake: ninja native build + tests OK"
else
	echo "kruddmake: ninja(1) not found — skipping native build stage"
fi

# Stage 3: build the WASM output (the single main module, every plugin compiled
# into it) through the same build.ninja, when the Emscripten toolchain is
# available. No emcmake — the whole WASM path goes through explicit emcc/em++
# calls.
if command -v ninja >/dev/null 2>&1 && command -v emcc >/dev/null 2>&1; then
	echo "kruddmake: building WASM output via ninja + emcc"
	ninja -C "$work" -f build.ninja wasm
	echo "kruddmake: ninja WASM build OK"
else
	echo "kruddmake: emcc not found — skipping WASM build stage"
fi
