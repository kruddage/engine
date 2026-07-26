// SPDX-License-Identifier: GPL-2.0-or-later

//! [`Call`]: one entry in a [`Recorder`](crate::Recorder)'s log.
//!
//! One variant per [`Commands`](krudd_render::Commands) method, carrying only
//! the arguments a test would assert on — not every argument the method
//! took. [`Commands::create_pipeline`](krudd_render::Commands::create_pipeline)
//! is handed a WGSL source string, for instance, but nothing here stores it:
//! a test asserts that a pipeline was built with two colour targets and a
//! fragment stage, not that shader text round-trips through a log. Keeping
//! the log to assertable shape rather than a full argument dump is what
//! keeps a failing assertion's [`core::fmt::Display`] output readable.

use core::fmt;

use krudd_gpu::{BufferId, BufferUsage, IndexFormat, PipelineId, TextureFormat, TextureId};
use krudd_render::{Color, LoadOp};

/// One recorded call to a [`Commands`](krudd_render::Commands) method.
#[derive(Clone, Debug, PartialEq)]
pub enum Call {
    /// [`Commands::create_texture`](krudd_render::Commands::create_texture).
    CreateTexture {
        /// The handle returned.
        texture: TextureId,
        /// The format requested.
        format: TextureFormat,
        /// The width requested.
        width: u32,
        /// The height requested.
        height: u32,
        /// The mip-level count requested.
        mip_levels: u32,
        /// Whether initial data was supplied.
        has_data: bool,
    },
    /// [`Commands::destroy_texture`](krudd_render::Commands::destroy_texture).
    DestroyTexture {
        /// The handle destroyed.
        texture: TextureId,
    },
    /// [`Commands::create_buffer`](krudd_render::Commands::create_buffer).
    CreateBuffer {
        /// The handle returned.
        buffer: BufferId,
        /// What the buffer is bound as.
        usage: BufferUsage,
        /// The size requested, in bytes.
        size: u64,
        /// Whether initial data was supplied.
        has_data: bool,
    },
    /// [`Commands::destroy_buffer`](krudd_render::Commands::destroy_buffer).
    DestroyBuffer {
        /// The handle destroyed.
        buffer: BufferId,
    },
    /// [`Commands::create_pipeline`](krudd_render::Commands::create_pipeline).
    CreatePipeline {
        /// The handle returned.
        pipeline: PipelineId,
        /// How many colour targets the pipeline was built against.
        color_format_count: usize,
        /// Whether the pipeline named a vertex entry point.
        vertex_has_source: bool,
        /// Whether the pipeline named a fragment entry point.
        fragment_has_source: bool,
    },
    /// [`Commands::barrier`](krudd_render::Commands::barrier).
    Barrier {
        /// How many transitions this call made.
        count: usize,
    },
    /// [`Commands::begin_render_pass`](krudd_render::Commands::begin_render_pass).
    BeginRenderPass {
        /// How many colour attachments the pass has.
        color_count: usize,
        /// Whether each colour attachment, in order, carries a resolve
        /// target.
        resolve_targets: Vec<bool>,
        /// Each colour attachment's load op, in order.
        color_loads: Vec<LoadOp<Color>>,
        /// The depth attachment's load op, if the pass has one.
        depth_load: Option<LoadOp<f32>>,
    },
    /// [`Commands::end_render_pass`](krudd_render::Commands::end_render_pass).
    EndRenderPass,
    /// [`Commands::set_pipeline`](krudd_render::Commands::set_pipeline).
    SetPipeline {
        /// The pipeline bound.
        pipeline: PipelineId,
    },
    /// [`Commands::bind_vertex_buffer`](krudd_render::Commands::bind_vertex_buffer).
    BindVertexBuffer {
        /// The slot bound.
        slot: u32,
        /// The buffer bound.
        buffer: BufferId,
        /// The byte offset into the buffer.
        offset: u64,
        /// The bound range's size, in bytes.
        size: u64,
    },
    /// [`Commands::bind_index_buffer`](krudd_render::Commands::bind_index_buffer).
    BindIndexBuffer {
        /// The buffer bound.
        buffer: BufferId,
        /// The byte offset into the buffer.
        offset: u64,
        /// The bound range's size, in bytes.
        size: u64,
        /// The index width.
        format: IndexFormat,
    },
    /// [`Commands::bind_uniform_buffer`](krudd_render::Commands::bind_uniform_buffer).
    BindUniformBuffer {
        /// The slot bound.
        slot: u32,
        /// The buffer bound.
        buffer: BufferId,
        /// The byte offset into the buffer.
        offset: u64,
        /// The bound range's size, in bytes.
        size: u64,
    },
    /// [`Commands::draw`](krudd_render::Commands::draw).
    Draw {
        /// The first vertex drawn.
        first_vertex: u32,
        /// How many vertices were drawn.
        vertex_count: u32,
        /// How many instances were drawn.
        instance_count: u32,
    },
    /// [`Commands::draw_indexed`](krudd_render::Commands::draw_indexed).
    DrawIndexed {
        /// How many indices were drawn.
        index_count: u32,
        /// How many instances were drawn.
        instance_count: u32,
    },
    /// [`Commands::dispatch`](krudd_render::Commands::dispatch).
    Dispatch {
        /// Workgroups on X.
        x: u32,
        /// Workgroups on Y.
        y: u32,
        /// Workgroups on Z.
        z: u32,
    },
}

impl fmt::Display for Call {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // `{:?}` rather than a hand-written line per variant: the derived
        // `Debug` already names every field, and this exists so a failing
        // assertion prints a call sequence one entry at a time (see
        // `Recorder`'s `Display`) rather than `Vec<Call>`'s own `Debug`,
        // which wraps the whole log into one line.
        write!(f, "{self:?}")
    }
}
