; SPDX-License-Identifier: GPL-2.0-or-later
;;! The shader DSL and its GLSL/WGSL transpiler, which run *inside* the engine's
;;! s7 image rather than lowering to C — so this directory builds no targets and
;;! its whole build fact is the embed below, which is what carries shader.scm
;;! into that image for core/script.c to evaluate at boot.
((embed "shader.scm" "shader_scm.h" "SHADER_SCM"))
