// SPDX-License-Identifier: GPL-2.0-or-later
//
// The hello-triangle shader. Proof of life for #818: one triangle, three
// vertices, no vertex buffer.
//
// The positions live in the shader rather than in a buffer on purpose. A
// vertex buffer is a whole path — upload, layout, attribute locations, a
// stride to get wrong — and none of it is what this issue is proving. The
// geometry is a generator, which is where krudd wants assets to end up
// anyway (#825); this is the smallest possible one.
//
// WGSL only. The GLSL a WebGL2 driver eventually sees is naga's output, not
// anything checked in, which is the whole reason the old tree's shader DSL is
// a `drop` verdict rather than a `carry`.

/// The one uniform: model-view-projection, already composed on the CPU.
///
/// Per-*draw*, not per-frame — the buffer is bound with a dynamic offset and
/// the renderer advances the offset for each draw in the frame. That keeps a
/// frame with many draws to one buffer and one bind group.
struct Uniforms {
	mvp: mat4x4<f32>,
}

@group(0) @binding(0) var<uniform> uniforms: Uniforms;

struct VertexOut {
	@builtin(position) clip: vec4<f32>,
	@location(0) color: vec3<f32>,
}

@vertex
fn vertex_main(@builtin(vertex_index) index: u32) -> VertexOut {
	// Function-local `var` rather than a module-level `const`: a local array
	// indexed by a runtime value lowers to a local array on every naga
	// backend, where dynamic indexing of a module-level constant does not.
	var positions = array<vec2<f32>, 3>(
		vec2<f32>(0.0, 0.75),
		vec2<f32>(-0.75, -0.75),
		vec2<f32>(0.75, -0.75),
	);
	// Linear, not sRGB. The surface format is `*UnormSrgb`, so the hardware
	// gamma-encodes whatever this writes — pasting the values a colour picker
	// gives you produces a washed-out triangle, and it looks like a shader bug
	// rather than a colour-space one. These are the linear values behind a
	// vivid red, green and blue.
	var colors = array<vec3<f32>, 3>(
		vec3<f32>(0.90, 0.04, 0.09),
		vec3<f32>(0.03, 0.60, 0.20),
		vec3<f32>(0.04, 0.14, 0.80),
	);

	// `vertex_index` starts at the draw's `first_vertex`, so the modulo is
	// what keeps a triangle a triangle at any offset into the range rather
	// than reading off the end of the arrays.
	let corner = index % 3u;

	var out: VertexOut;
	out.clip = uniforms.mvp * vec4<f32>(positions[corner], 0.0, 1.0);
	out.color = colors[corner];
	return out;
}

@fragment
fn fragment_main(in: VertexOut) -> @location(0) vec4<f32> {
	return vec4<f32>(in.color, 1.0);
}
