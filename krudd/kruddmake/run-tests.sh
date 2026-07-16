#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# kruddmake test harness.
#
# Stage 0 (always): the s7-only bootstrap checks — introspect_test.scm exercises
# the codegen/embed helpers with no WASM/Ninja toolchain needed.
#
# Stage 1 (always): build a native s7 interpreter and run resolve_test.scm —
# the resolver + emitter checks. This needs only a C compiler, no WASM/Ninja.
#
# Stage 2 (when ninja + cc are present): render the real manifest to a
# build.ninja and build the `native` target with ninja(1). Each test links and
# runs as a run_test stamp, so a green `ninja native` means the whole native
# suite passed through the generated build — the proof the Ninja path is
# equivalent to the CMake one for native builds.
set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
export KRUDD_ROOT="$root"

cc=${CC:-}
if [ -z "$cc" ]; then
	for c in cc gcc clang; do
		if command -v "$c" >/dev/null 2>&1; then
			cc=$c
			break
		fi
	done
fi
[ -n "$cc" ] || { echo "kruddmake: no C compiler (set CC)" >&2; exit 1; }

. "$root/krudd/third_party/sync.sh"

work="$root/build-ninja"
mkdir -p "$work"

s7bin="$work/s7"
s7src="$root/krudd/third_party/s7.c"
if [ ! -x "$s7bin" ] || [ "$s7src" -nt "$s7bin" ]; then
	echo "kruddmake: building s7 test interpreter"
	"$cc" -O2 -w -DWITH_MAIN=1 -DWITH_C_LOADER=0 \
		-I"$root/krudd/third_party" \
		-o "$s7bin" "$s7src" -lm
fi

# Stage 0: s7-only bootstrap checks (codegen/embed helpers).
"$s7bin" "$root/krudd/kruddmake/introspect_test.scm"

# Stage 1: resolver/emitter checks; render build.ninja for stage 2. KRUDD_S7BIN
# lets resolve_test.scm bake a self-contained `regen` command into build.ninja
# so a later raw `ninja` run regenerates codegen when a `.scm` input changes.
export KRUDD_NINJA_OUT="$work/build.ninja"
export KRUDD_S7BIN="$s7bin"
"$s7bin" "$root/krudd/kruddmake/resolve_test.scm"

# Stage 1b: the monolang reference oracle — evaluate math.scm's (define-c-fn)
# bodies in s7 and check the numbers, the same math the generated C is checked
# for by engine/math/math_test.c in the native stage below.
"$s7bin" "$root/krudd/engine/math/math_test.scm"

# Stage 1c: the shader DSL oracle — run shader.scm's transpiler in s7 and check
# the GLSL it lowers the built-in shaders to, the same transpiler embedded into
# the runtime image and exercised natively by engine/core/shader_transpile_test.c.
"$s7bin" "$root/krudd/engine/shader/shader_test.scm"

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
