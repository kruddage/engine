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

use krudd_gpu::{PipelineId, TextureId};
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
