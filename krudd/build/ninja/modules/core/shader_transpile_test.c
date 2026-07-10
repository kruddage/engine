/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * shader_transpile — the runtime shader path end to end, minus the GPU. It
 * boots the real s7 image (which loads the embedded shader.scm) and drives
 * script_shader_transpile the way the WebGL backend does at bind time: one
 * DSL source, a stage string, GLSL out. The GLSL text itself is checked more
 * exhaustively by the Scheme oracle (modules/shader_test.scm); here we prove
 * the C seam, the rotating buffers, and the missing-stage NULL contract.
 */
#include "script.h"

#include "log.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *SCENE =
	"(shader scene"
	"  (inputs (a_pos vec3 (location 0)) (a_normal vec3 (location 1))"
	"          (a_uv0 vec2 (location 2)))"
	"  (uniforms (Camera (block 0) (layout std140)"
	"              (view_proj mat4) (model mat4)))"
	"  (varyings (v_normal vec3))"
	"  (targets (frag_color vec4 (location 0)))"
	"  (vertex"
	"    (set v_normal (* (mat3 model) a_normal))"
	"    (set position (* view_proj model (vec4 a_pos 1.0))))"
	"  (fragment"
	"    (let* ((n (normalize v_normal)))"
	"      (set frag_color (vec4 n 1.0)))))";

int main(void)
{
	const char *vs;
	const char *fs;

	log_init();
	script_init();

	/* Vertex stage lowers to GLSL ES 300 from the shared IO model. */
	vs = script_shader_transpile(SCENE, "vertex");
	assert(vs != NULL);
	assert(strstr(vs, "#version 300 es") == vs);
	assert(strstr(vs, "layout(location = 1) in vec3 a_normal;") != NULL);
	assert(strstr(vs, "out vec3 v_normal;") != NULL);
	assert(strstr(vs,
		      "gl_Position = (view_proj * model * vec4(a_pos, 1.0));")
	       != NULL);

	/* Fragment stage: same varying flips to an in, precision is set. */
	fs = script_shader_transpile(SCENE, "fragment");
	assert(fs != NULL);
	assert(strstr(fs, "precision mediump float;") != NULL);
	assert(strstr(fs, "in vec3 v_normal;") != NULL);
	assert(strstr(fs, "layout(location = 0) out vec4 frag_color;") != NULL);

	/* The vertex result survives the fragment call (rotating buffers). */
	assert(strstr(vs, "a_normal") != NULL);

	/* A stage the shader never declares is NULL — the renderer's error. */
	assert(script_shader_transpile(SCENE, "compute") == NULL);

	/*
	 * The Material-block introspection seam: the same image reports a
	 * shader's editable parameters (std140 offsets + edit hints), which the
	 * editor turns into widgets and the packer turns into material bytes.
	 */
	{
		static const char *MAT =
			"(shader m (uniforms (Material (block 1) (layout std140)"
			"  (base_color vec4 (edit color))"
			"  (roughness  float (edit range 0 1))))"
			"  (fragment (set c (vec4 (* base_color roughness) 1.0))))";
		struct shader_param p[8];
		uint32_t            total = 0;
		int                 n = script_shader_material_params(MAT, p, 8,
								      &total);

		assert(n == 2);
		assert(total == 32); /* vec4(16) + float(4) -> rounded to 32 */

		assert(strcmp(p[0].name, "base_color") == 0);
		assert(strcmp(p[0].type, "vec4") == 0);
		assert(p[0].offset == 0 && p[0].size == 16 && p[0].components == 4);
		assert(strcmp(p[0].edit, "color") == 0);

		assert(strcmp(p[1].name, "roughness") == 0);
		assert(p[1].offset == 16 && p[1].components == 1);
		assert(strcmp(p[1].edit, "range") == 0);
		assert(p[1].edit_min == 0.0f && p[1].edit_max == 1.0f);

		/* A shader with no Material block: zero fields, zero size. */
		total = 123;
		n = script_shader_material_params(
			"(shader n (targets (c vec4 (location 0)))"
			"  (fragment (set c (vec4 1.0 1.0 1.0 1.0))))",
			p, 8, &total);
		assert(n == 0 && total == 0);
	}

	script_shutdown();
	printf("shader_transpile tests passed\n");
	return 0;
}
