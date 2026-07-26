// SPDX-License-Identifier: GPL-2.0-or-later

//! The renderer interface: what a backend must provide, and the frame the
//! engine hands it.
//!
//! **Tier: `render`.** May reach for `base`, `world` and its own tier. It
//! holds no backend — the WebGL2 one is #818, and it lives in its own crate
//! so that this one stays the interface rather than becoming the
//! implementation's header.
//!
//! ## Scope
//!
//! This crate is the seam and the per-frame vocabulary. The pass DAG with its
//! transient lifetimes and automatic barriers — the frame graph, the one
//! render concept the audit found that wgpu does not already provide — is
//! #823 and will sit above this.
//!
//! ## `Backend` and `Commands` are two different levels
//!
//! [`Backend`] is the shell-facing seam: begin a frame, submit it, end it.
//! [`Commands`] is one level down — the individual GPU calls a pass issues,
//! the level the old `gpu_api` recorded one call at a time (#826). A `Frame`
//! is what [`Backend::submit`] is handed whole; `Commands` is what something
//! *inside* a submit — #823's frame graph, eventually — drives call by call.
//! Neither trait implies the other: `krudd-webgl`'s [`Backend`] does not go
//! through `Commands` today, and a `Commands` implementation owes nothing to
//! `Backend`'s frame boundaries.

use krudd_gpu::{BufferId, BufferUsage, IndexFormat, PipelineId, TextureFormat, TextureId};
use krudd_math::Mat4;

/// The rectangle a frame renders into, in physical pixels.
///
/// Physical, not CSS, pixels: a canvas on a 2x display is twice the size its
/// layout says, and a renderer that took the layout size would render at half
/// resolution and upscale. The shell is responsible for the conversion,
/// because it is the half that can read `devicePixelRatio`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Viewport {
    /// Width in physical pixels. Never zero — see [`Viewport::new`].
    pub width: u32,
    /// Height in physical pixels. Never zero — see [`Viewport::new`].
    pub height: u32,
}

impl Viewport {
    /// A viewport of the given size, clamped so neither dimension is zero.
    ///
    /// A minimised window or a display-`none` canvas reports 0x0, and a
    /// zero-sized framebuffer is a backend error on every API. Clamping to
    /// 1x1 keeps the frame legal and cheap rather than making every caller
    /// special-case a state the browser produces routinely.
    pub fn new(width: u32, height: u32) -> Self {
        Self {
            width: width.max(1),
            height: height.max(1),
        }
    }

    /// The width over the height.
    pub fn aspect(self) -> f32 {
        self.width as f32 / self.height as f32
    }
}

/// A linear, premultiplied RGBA colour.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct Color {
    /// Red.
    pub r: f32,
    /// Green.
    pub g: f32,
    /// Blue.
    pub b: f32,
    /// Alpha.
    pub a: f32,
}

impl Color {
    /// Opaque black.
    pub const BLACK: Self = Self::rgba(0.0, 0.0, 0.0, 1.0);

    /// A colour from its components.
    pub const fn rgba(r: f32, g: f32, b: f32, a: f32) -> Self {
        Self { r, g, b, a }
    }
}

/// One draw: a pipeline, a transform, and a range of a vertex buffer.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Draw {
    /// The pipeline to bind.
    pub pipeline: PipelineId,
    /// The model-to-world transform.
    pub transform: Mat4,
    /// The first vertex to draw.
    pub first_vertex: u32,
    /// How many vertices to draw.
    pub vertex_count: u32,
}

/// Everything the engine wants drawn this frame.
///
/// Built once per frame and handed to the backend whole. It is not a command
/// encoder: nothing here touches the GPU, so the frame can be built on a
/// worker, recorded for a test, or thrown away.
///
/// Deliberately not `Default`: a frame with no viewport is not a sensible
/// zero value, and [`Frame::new`] is the only way to get one.
#[derive(Clone, Debug)]
pub struct Frame {
    /// Where the frame renders.
    pub viewport: Viewport,
    /// What the colour target is cleared to before anything draws. `None`
    /// loads the previous contents instead.
    pub clear: Option<Color>,
    /// The world-to-clip transform every draw is composed with.
    pub view_projection: Mat4,
    /// Where the frame renders to. `None` means the swapchain's backbuffer.
    pub target: Option<TextureId>,
    draws: Vec<Draw>,
}

impl Default for Viewport {
    fn default() -> Self {
        Self::new(1, 1)
    }
}

impl Frame {
    /// An empty frame targeting the backbuffer, cleared to black.
    pub fn new(viewport: Viewport) -> Self {
        Self {
            viewport,
            clear: Some(Color::BLACK),
            view_projection: Mat4::IDENTITY,
            target: None,
            draws: Vec::new(),
        }
    }

    /// Adds a draw.
    pub fn push(&mut self, draw: Draw) {
        self.draws.push(draw);
    }

    /// The draws, in submission order.
    pub fn draws(&self) -> &[Draw] {
        &self.draws
    }

    /// Drops every draw but keeps the allocation, so the next frame refills
    /// the same buffer instead of allocating one.
    pub fn clear_draws(&mut self) {
        self.draws.clear();
    }
}

/// A rendering backend.
///
/// ## `end_frame` is not `submit`
///
/// The old engine learned this the expensive way and it is worth not
/// relearning: a frame is not one command buffer. A backend may submit
/// several times within a frame — a shadow pass, a blur chain — and the point
/// at which the frame is *finished* is a separate event from the point at
/// which work is *handed to the driver*. Collapsing the two meant per-frame
/// resources were released at the first submit, while later passes were still
/// reading them.
///
/// So: [`Backend::submit`] hands work over and may be called many times;
/// [`Backend::end_frame`] is called exactly once and is where per-frame
/// lifetimes end.
pub trait Backend {
    /// The error a backend reports.
    type Error: core::fmt::Debug;

    /// Prepares for a new frame at the given viewport, resizing the
    /// swapchain if it changed.
    fn begin_frame(&mut self, viewport: Viewport) -> Result<(), Self::Error>;

    /// Executes a frame's draws and hands the work to the driver. May be
    /// called more than once between [`Backend::begin_frame`] and
    /// [`Backend::end_frame`].
    fn submit(&mut self, frame: &Frame) -> Result<(), Self::Error>;

    /// Ends the frame. Called exactly once per `begin_frame`, and the only
    /// place per-frame resources may be released.
    fn end_frame(&mut self) -> Result<(), Self::Error>;
}

/// The shape a texture is created with.
///
/// Deliberately smaller than a full GPU texture descriptor: every field here
/// is one `krudd-record`'s recorder captures, and this is the level
/// [`Commands::create_texture`] speaks at — widening it without a call site
/// that needs the extra field is how a descriptor drifts ahead of what
/// anything asserts on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TextureDesc {
    /// Pixel layout.
    pub format: TextureFormat,
    /// Width in texels.
    pub width: u32,
    /// Height in texels.
    pub height: u32,
    /// Mip levels, including the base level. `1` means no mip chain.
    pub mip_levels: u32,
    /// MSAA sample count. `1` means no multisampling.
    pub sample_count: u32,
}

/// The shape a buffer is created with.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BufferDesc {
    /// What the buffer is bound as.
    pub usage: BufferUsage,
    /// Size in bytes.
    pub size: u64,
}

/// The shape a render pipeline is created with.
///
/// One `shader` module compiles both stages — mirroring `krudd-webgl`'s WGSL,
/// where one module holds both entry points — so `vertex_entry` and
/// `fragment_entry` name which functions in it are which stage, and either
/// may be absent: a depth-only pass has no fragment stage, and "whether each
/// stage had source" only means something because the entry point can be
/// missing.
#[derive(Clone, Copy, Debug)]
pub struct PipelineDesc<'a> {
    /// For error messages and GPU debug tools.
    pub label: &'a str,
    /// The WGSL module both stages compile from.
    pub shader: &'a str,
    /// The vertex stage's entry point, if this pipeline has one.
    pub vertex_entry: Option<&'a str>,
    /// The fragment stage's entry point, if this pipeline has one.
    pub fragment_entry: Option<&'a str>,
    /// The colour targets this pipeline is validated against — see
    /// `krudd-webgl`'s `Error::PipelineFormat`, which is what a mismatch here
    /// turns into once something is driving a real backend.
    pub color_formats: &'a [TextureFormat],
    /// The depth target this pipeline is validated against, if it writes one.
    pub depth_format: Option<TextureFormat>,
    /// MSAA sample count this pipeline is built for.
    pub sample_count: u32,
}

/// What a render pass attachment starts a frame with.
///
/// One generic enum rather than two near-identical ones: a colour attachment
/// clears to a [`Color`] and a depth attachment clears to an `f32`, and nothing
/// else differs between "keep what's there" and "clear to this" regardless of
/// what the cleared value's type is.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum LoadOp<T> {
    /// Keep the attachment's existing contents.
    Load,
    /// Clear to this value before anything draws.
    Clear(T),
}

/// One colour target a render pass draws into.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ColorAttachment {
    /// The texture drawn into.
    pub texture: TextureId,
    /// Where a multisampled result resolves to, if this attachment is
    /// multisampled.
    ///
    /// Riding on the colour attachment rather than being an attachment in its
    /// own right is load-bearing for #823: it is what lets a resolve target
    /// be an ordinary produced resource to the frame graph's cull and
    /// lifetime machinery, instead of a second kind of pass output the graph
    /// has to special-case. Do not drop this field because nothing sets it
    /// yet.
    pub resolve_target: Option<TextureId>,
    /// What the attachment starts the pass with.
    pub load: LoadOp<Color>,
}

/// The depth target a render pass draws into.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DepthAttachment {
    /// The texture drawn into.
    pub texture: TextureId,
    /// What the attachment starts the pass with.
    pub load: LoadOp<f32>,
}

/// A render pass's attachments.
#[derive(Clone, Copy, Debug)]
pub struct RenderPassDesc<'a> {
    /// For error messages and GPU debug tools.
    pub label: &'a str,
    /// The colour targets, in binding order.
    pub colors: &'a [ColorAttachment],
    /// The depth target, if this pass writes one.
    pub depth: Option<DepthAttachment>,
}

/// What a texture is being used as, on either side of a [`Barrier`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Access {
    /// Written as a colour attachment.
    RenderTarget,
    /// Written as a depth attachment.
    DepthTarget,
    /// Read through a shader binding.
    Sampled,
}

/// A transition a texture must make before the next pass can use it in a
/// different way.
///
/// #823 is what will emit these: a pass that writes a texture as a render
/// target and a later one that samples it need a barrier in between, and the
/// frame graph is the only thing that has enough of the pass order to know
/// where.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Barrier {
    /// The texture transitioning.
    pub texture: TextureId,
    /// What it was being used as.
    pub from: Access,
    /// What it is about to be used as.
    pub to: Access,
}

/// The individual GPU calls a pass issues.
///
/// This is one level below [`Backend`] — see the module docs — and its method
/// list is deliberately the old `renderer_null.h` call list (#826): that is
/// what makes it the assertion vocabulary a recording implementation and a
/// real one can both be tested against. `krudd-record`'s `Recorder`
/// implements it today; `krudd-webgl` gains an implementation only once
/// something drives it through here (#823), and does not gain one as part of
/// this trait landing.
pub trait Commands {
    /// The error this implementation reports.
    type Error: core::fmt::Debug;

    // Off-frame: persistent resources, created against the device directly
    // and never through a lent pass context — that context is #823's to
    // define, not this trait's.
    /// Creates a texture, optionally seeded with data.
    fn create_texture(
        &mut self,
        desc: &TextureDesc,
        data: Option<&[u8]>,
    ) -> Result<TextureId, Self::Error>;

    /// Destroys a texture. Every outstanding handle to it goes stale.
    fn destroy_texture(&mut self, texture: TextureId) -> Result<(), Self::Error>;

    /// Creates a buffer, optionally seeded with data.
    fn create_buffer(
        &mut self,
        desc: &BufferDesc,
        data: Option<&[u8]>,
    ) -> Result<BufferId, Self::Error>;

    /// Destroys a buffer. Every outstanding handle to it goes stale.
    fn destroy_buffer(&mut self, buffer: BufferId) -> Result<(), Self::Error>;

    /// Compiles a render pipeline.
    fn create_pipeline(&mut self, desc: &PipelineDesc<'_>) -> Result<PipelineId, Self::Error>;

    // Per-frame.
    /// Transitions one or more textures ahead of the passes that need them in
    /// their new state.
    fn barrier(&mut self, barriers: &[Barrier]) -> Result<(), Self::Error>;

    /// Begins a render pass against the given attachments.
    fn begin_render_pass(&mut self, desc: &RenderPassDesc<'_>) -> Result<(), Self::Error>;

    /// Ends the current render pass.
    fn end_render_pass(&mut self) -> Result<(), Self::Error>;

    /// Binds the pipeline subsequent draws use.
    fn set_pipeline(&mut self, pipeline: PipelineId) -> Result<(), Self::Error>;

    /// Binds a vertex buffer at the given slot.
    fn bind_vertex_buffer(
        &mut self,
        slot: u32,
        buffer: BufferId,
        offset: u64,
        size: u64,
    ) -> Result<(), Self::Error>;

    /// Binds the index buffer subsequent indexed draws read from.
    fn bind_index_buffer(
        &mut self,
        buffer: BufferId,
        offset: u64,
        size: u64,
        format: IndexFormat,
    ) -> Result<(), Self::Error>;

    /// Binds a uniform buffer at the given slot.
    fn bind_uniform_buffer(
        &mut self,
        slot: u32,
        buffer: BufferId,
        offset: u64,
        size: u64,
    ) -> Result<(), Self::Error>;

    /// Draws non-indexed vertices.
    fn draw(
        &mut self,
        first_vertex: u32,
        vertex_count: u32,
        instance_count: u32,
    ) -> Result<(), Self::Error>;

    /// Draws indexed vertices, through the bound index buffer.
    fn draw_indexed(&mut self, index_count: u32, instance_count: u32) -> Result<(), Self::Error>;

    /// Dispatches a compute workgroup grid.
    fn dispatch(&mut self, x: u32, y: u32, z: u32) -> Result<(), Self::Error>;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_zero_sized_viewport_is_clamped_not_accepted() {
        let v = Viewport::new(0, 0);
        assert_eq!(v, Viewport::new(1, 1));
        assert_eq!(v.aspect(), 1.0);
    }

    #[test]
    fn aspect_is_width_over_height() {
        assert_eq!(Viewport::new(16, 9).aspect(), 16.0 / 9.0);
    }

    #[test]
    fn a_new_frame_targets_the_backbuffer_and_clears() {
        let f = Frame::new(Viewport::new(320, 240));
        assert!(f.target.is_none());
        assert_eq!(f.clear, Some(Color::BLACK));
        assert!(f.draws().is_empty());
    }

    #[test]
    fn draws_keep_submission_order() {
        let mut f = Frame::new(Viewport::new(8, 8));
        for i in 0..3u32 {
            f.push(Draw {
                pipeline: PipelineId::new(0, 0),
                transform: Mat4::IDENTITY,
                first_vertex: i,
                vertex_count: 3,
            });
        }
        assert_eq!(
            f.draws().iter().map(|d| d.first_vertex).collect::<Vec<_>>(),
            vec![0, 1, 2]
        );
    }

    #[test]
    fn clearing_draws_keeps_the_allocation() {
        let mut f = Frame::new(Viewport::new(8, 8));
        f.push(Draw {
            pipeline: PipelineId::new(0, 0),
            transform: Mat4::IDENTITY,
            first_vertex: 0,
            vertex_count: 3,
        });
        let capacity = f.draws.capacity();
        f.clear_draws();
        assert!(f.draws().is_empty());
        assert_eq!(f.draws.capacity(), capacity);
    }
}
