// SPDX-License-Identifier: GPL-2.0-or-later

//! The WebGL2 rendering backend, on wgpu.
//!
//! **Tier: `render`.** May reach for `base`, `world` and its own tier. It is
//! the implementation behind [`krudd_render::Backend`]; the interface itself
//! stays in `krudd-render` so that nothing has to depend on wgpu to talk
//! about a frame.
//!
//! ## WebGL2, and no WebGPU anywhere
//!
//! wgpu is compiled with its `webgl` feature and *without* `webgpu`, so there
//! is no second code path to keep working and no runtime backend choice to get
//! wrong — [`wgpu::Backends::GL`] is the only backend the instance is even
//! built with. That is #812's call, and it is not only about scope: Tauri uses
//! WebKitGTK on Linux, WebKitGTK ships no WebGPU, and WebGL2 runs in every
//! browser *and* in WebKitGTK. Deferring WebGPU removes the risk rather than
//! maintaining a fallback for it.
//!
//! wgpu rather than raw WebGL through `glow` is the other deliberate trade:
//! shaders are WGSL and naga lowers them, so adding WebGPU later is a feature
//! flag rather than a renderer rewrite.
//!
//! ## Two lessons from the old engine, made structural
//!
//! **A frame is not one command buffer.** [`Backend::submit`] hands work to
//! the driver and may be called several times in a frame; the surface texture
//! belongs to the *frame*, and is released only in [`Backend::end_frame`].
//! Collapsing the two is how the old WebGPU backend ended up presenting a
//! blank canvas: the second subsystem to draw acquired a fresh texture, and
//! the first one's work sat in a texture nobody presented.
//!
//! Acquisition is *lazy*, for the same reason the old `gpu_api` had no
//! `frame_begin`: a frame that submits nothing should not touch the surface at
//! all.
//!
//! **A pipeline is validated against the pass it runs in.** So a pipeline
//! cannot be shared across target formats, and this backend records the format
//! each pipeline was built against and refuses a mismatch
//! ([`Error::PipelineFormat`]) rather than letting the driver report it three
//! layers away. One triangle does not need a pipeline cache keyed by (shader,
//! target state), but it does need the mismatch to be an error rather than a
//! surprise — the cache itself arrives with the second target.
//!
//! ## Scope
//!
//! One triangle, per #818. There is no vertex buffer, no depth attachment, no
//! offscreen target and no MSAA. The pass DAG with its transient lifetimes and
//! barriers is #823, and it sits *above* this — a triangle does not need one.

mod slots;

use std::fmt;
use std::num::NonZeroU64;
use std::sync::{Arc, Mutex};

use krudd_gpu::PipelineId;
use krudd_math::Mat4;
use krudd_render::{Backend, Color, Frame, Viewport};

use crate::slots::Slots;

/// The hello-triangle shader, in WGSL.
///
/// Public because it is the source [`Renderer::create_pipeline`] is normally
/// handed, and a caller that wants to know what it is drawing should be able
/// to read it without reaching into this crate's source directory.
pub const TRIANGLE_SHADER: &str = include_str!("triangle.wgsl");

/// Bytes one `mat4x4<f32>` occupies — the whole of this backend's per-draw
/// uniform block.
const UNIFORM_BYTES: u64 = 64;

/// What can go wrong, from booting the backend to presenting a frame.
///
/// Every variant carries enough to act on: a boot failure says which stage
/// failed, and a per-frame failure says whether it is worth retrying. The
/// shell turns these into text on the page — a renderer that fails silently is
/// indistinguishable from one that drew nothing, and #812 rules that out
/// explicitly.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    /// The surface could not be created — on WebGL2 this is what "the browser
    /// has no WebGL2" looks like.
    Surface(String),
    /// No adapter could present to the surface.
    NoAdapter(String),
    /// An adapter was found but would not give up a device.
    NoDevice(String),
    /// The surface exposes no format/present-mode pair this backend can use.
    NoSurfaceFormat,
    /// A draw named a pipeline this backend does not have — a stale handle, or
    /// one from a different renderer.
    UnknownPipeline(PipelineId),
    /// A draw's pipeline was built for a different colour format than the
    /// target it is being drawn into.
    PipelineFormat {
        /// What the pipeline was built against.
        pipeline: String,
        /// What the frame is rendering into.
        target: String,
    },
    /// The frame's viewport disagrees with the configured surface size.
    /// [`Backend::begin_frame`] is what reconciles them, and it was not called
    /// for this size.
    ViewportMismatch {
        /// What the frame asked for.
        frame: Viewport,
        /// What the surface is configured at.
        surface: Viewport,
    },
    /// [`Backend::begin_frame`] was called twice without an intervening
    /// [`Backend::end_frame`].
    FrameStillOpen,
    /// A frame was submitted outside a `begin_frame` / `end_frame` pair.
    NoFrameOpen,
    /// The surface could not hand over a texture, and the frame should be
    /// skipped. Transient: the next frame is likely to work.
    SurfaceUnavailable(&'static str),
    /// The surface configuration was stale and has been reconfigured; retry the
    /// frame.
    SurfaceOutdated,
    /// A frame targeted an offscreen texture. Not yet supported — the frame
    /// graph is what will own render targets (#823).
    OffscreenTarget,
    /// The device reported an error out of band. This is a validation failure
    /// or a lost device, and it is fatal to the frame.
    Device(String),
}

impl Error {
    /// Whether this is a frame worth skipping rather than a failure worth
    /// reporting.
    ///
    /// A browser produces both of the transient cases routinely — a resize
    /// between frames, a tab going to the background — and a page that put
    /// either on screen as an error would cry wolf often enough that nobody
    /// would read the ones that matter. Everything else is real: a caller that
    /// treats an unknown pipeline or a device error as a skipped frame is back
    /// to a blank canvas with no explanation, which is the failure mode #812
    /// forbids.
    pub fn is_transient(&self) -> bool {
        matches!(self, Self::SurfaceOutdated | Self::SurfaceUnavailable(_))
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Surface(why) => write!(f, "no WebGL2 surface: {why}"),
            Self::NoAdapter(why) => write!(f, "no GPU adapter could present to the canvas: {why}"),
            Self::NoDevice(why) => write!(f, "the adapter would not provide a device: {why}"),
            Self::NoSurfaceFormat => {
                write!(f, "the surface reports no usable format or present mode")
            }
            Self::UnknownPipeline(id) => write!(f, "no such pipeline: {id:?}"),
            Self::PipelineFormat { pipeline, target } => write!(
                f,
                "pipeline was built for {pipeline} but the target is {target} — \
                 a pipeline is validated against the pass it runs in"
            ),
            Self::ViewportMismatch { frame, surface } => write!(
                f,
                "the frame is {}x{} but the surface is configured at {}x{}",
                frame.width, frame.height, surface.width, surface.height
            ),
            Self::FrameStillOpen => write!(f, "begin_frame called with a frame already open"),
            Self::NoFrameOpen => write!(f, "submit called outside begin_frame / end_frame"),
            Self::SurfaceUnavailable(why) => write!(f, "the surface is unavailable: {why}"),
            Self::SurfaceOutdated => write!(f, "the surface was reconfigured — retry the frame"),
            Self::OffscreenTarget => write!(f, "offscreen render targets are not implemented yet"),
            Self::Device(why) => write!(f, "the GPU device reported an error: {why}"),
        }
    }
}

impl std::error::Error for Error {}

/// Where the device's out-of-band errors land.
///
/// wgpu reports a validation failure through a callback rather than through
/// the call that caused it, and on the web the message frequently never reaches
/// the page's console either. So it is captured here and returned from the next
/// frame boundary instead: a blank canvas plus a message in a log nobody is
/// reading is exactly the "instrument that cannot fail loudly" #812 forbids.
///
/// The *first* error is kept rather than the last — later ones are usually
/// consequences of it.
#[derive(Clone, Default)]
struct ErrorSink(Arc<Mutex<Option<String>>>);

impl ErrorSink {
    /// Records an error if none is recorded yet.
    fn record(&self, message: String) {
        let mut slot = self.lock();
        if slot.is_none() {
            *slot = Some(message);
        }
    }

    /// Takes the recorded error, if any.
    fn take(&self) -> Option<String> {
        self.lock().take()
    }

    /// The guard, with a poisoned lock recovered rather than propagated: what
    /// is behind it is one `Option<String>`, and a panic cannot leave that
    /// inconsistent.
    fn lock(&self) -> std::sync::MutexGuard<'_, Option<String>> {
        self.0
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }
}

/// One render pipeline, and the colour format it was built against.
struct Pipeline {
    handle: wgpu::RenderPipeline,
    /// Kept because wgpu validates a pipeline against the pass it runs in, so
    /// this is part of the pipeline's identity rather than a detail of how it
    /// happened to be made.
    format: wgpu::TextureFormat,
}

/// The surface texture a frame renders into, held for the whole frame.
struct Acquired {
    texture: wgpu::SurfaceTexture,
    view: wgpu::TextureView,
    /// Whether anything has drawn into it yet. The first submit of a frame may
    /// clear; a later one must load, or it erases the earlier one's work.
    written: bool,
}

/// The per-draw uniform buffer, its bind group, and the stride between draws.
struct Uniforms {
    buffer: wgpu::Buffer,
    bind_group: wgpu::BindGroup,
    layout: wgpu::BindGroupLayout,
    /// Bytes between one draw's block and the next. At least
    /// [`UNIFORM_BYTES`], rounded up to the device's dynamic-offset alignment
    /// — 256 on WebGL2.
    stride: u64,
    /// How many draws the buffer currently has room for.
    capacity: usize,
}

/// A WebGL2 renderer bound to one canvas.
pub struct Renderer {
    surface: wgpu::Surface<'static>,
    device: wgpu::Device,
    queue: wgpu::Queue,
    config: wgpu::SurfaceConfiguration,
    adapter: wgpu::Adapter,
    uniforms: Uniforms,
    pipelines: Slots<krudd_gpu::Pipeline, Pipeline>,
    /// The frame's surface texture. `Some` from the first submit of a frame
    /// until `end_frame`, and taken by nothing else — see the module docs.
    acquired: Option<Acquired>,
    /// Whether a `begin_frame` is outstanding.
    open: bool,
    errors: ErrorSink,
    /// Reused between submits so a frame's uniform upload does not allocate.
    staging: Vec<u8>,
}

impl Renderer {
    /// Boots a renderer against a surface target.
    ///
    /// A [`wgpu::SurfaceTarget`] rather than a canvas: this crate stays free of
    /// `web-sys` and compiles for the host, so `cargo test` remains a real test
    /// run. The shell picks `SurfaceTarget::Canvas`, because the shell is the
    /// half that is allowed to know the browser exists.
    ///
    /// Async because requesting an adapter and a device are both async on the
    /// web, and there is no blocking to be had on the main thread.
    pub async fn new(
        target: wgpu::SurfaceTarget<'static>,
        viewport: Viewport,
    ) -> Result<Self, Error> {
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
            // GL only. The `webgpu` feature is off at compile time, so this is
            // belt and braces rather than a choice made at runtime.
            backends: wgpu::Backends::GL,
            ..wgpu::InstanceDescriptor::new_without_display_handle()
        });

        // The surface comes first, and not by preference: on WebGL2 an adapter
        // cannot be created without one, because the adapter *is* the canvas's
        // GL context.
        let surface = instance
            .create_surface(target)
            .map_err(|e| Error::Surface(e.to_string()))?;

        let adapter = instance
            .request_adapter(&wgpu::RequestAdapterOptions {
                power_preference: wgpu::PowerPreference::default(),
                force_fallback_adapter: false,
                compatible_surface: Some(&surface),
                ..Default::default()
            })
            .await
            .map_err(|e| Error::NoAdapter(e.to_string()))?;

        let (device, queue) = adapter
            .request_device(&wgpu::DeviceDescriptor {
                label: Some("krudd device"),
                required_features: wgpu::Features::empty(),
                // The WebGL2 floor, so a device request cannot succeed here and
                // fail on a weaker machine. Asking for more than the engine
                // uses is how a renderer becomes untestable on the hardware it
                // ships to.
                required_limits: wgpu::Limits::downlevel_webgl2_defaults(),
                ..Default::default()
            })
            .await
            .map_err(|e| Error::NoDevice(e.to_string()))?;

        let errors = ErrorSink::default();
        let sink = errors.clone();
        device.on_uncaptured_error(Arc::new(move |error: wgpu::Error| {
            sink.record(error.to_string());
        }));
        let sink = errors.clone();
        device.set_device_lost_callback(move |reason, message| {
            sink.record(format!("device lost ({reason:?}): {message}"));
        });

        let mut config = surface
            .get_default_config(&adapter, viewport.width, viewport.height)
            .ok_or(Error::NoSurfaceFormat)?;
        config.width = viewport.width;
        config.height = viewport.height;
        surface.configure(&device, &config);

        let uniforms = Uniforms::new(&device, 1);

        Ok(Self {
            surface,
            device,
            queue,
            config,
            adapter,
            uniforms,
            pipelines: Slots::new(),
            acquired: None,
            open: false,
            errors,
            staging: Vec::new(),
        })
    }

    /// Compiles a WGSL source into a pipeline targeting the surface's format,
    /// and returns the handle a [`krudd_render::Draw`] names it by.
    ///
    /// A pipeline outlives the frame, so it is created here — against the
    /// device directly — rather than from inside one. Keeping that separation
    /// is what makes "the frame owns the frame" true rather than aspirational,
    /// and it is the seam #823's lent pass context will sit against.
    pub fn create_pipeline(&mut self, label: &str, source: &str) -> PipelineId {
        let module = self
            .device
            .create_shader_module(wgpu::ShaderModuleDescriptor {
                label: Some(label),
                source: wgpu::ShaderSource::Wgsl(source.into()),
            });
        let layout = self
            .device
            .create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some(label),
                bind_group_layouts: &[Some(&self.uniforms.layout)],
                immediate_size: 0,
            });
        let format = self.config.format;
        let handle = self
            .device
            .create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label),
                layout: Some(&layout),
                vertex: wgpu::VertexState {
                    module: &module,
                    entry_point: Some("vertex_main"),
                    compilation_options: wgpu::PipelineCompilationOptions::default(),
                    buffers: &[],
                },
                primitive: wgpu::PrimitiveState::default(),
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                fragment: Some(wgpu::FragmentState {
                    module: &module,
                    entry_point: Some("fragment_main"),
                    compilation_options: wgpu::PipelineCompilationOptions::default(),
                    targets: &[Some(format.into())],
                }),
                multiview_mask: None,
                cache: None,
            });
        self.pipelines.insert(Pipeline { handle, format })
    }

    /// Compiles [`TRIANGLE_SHADER`]. The whole of #818's proof of life.
    pub fn create_triangle_pipeline(&mut self) -> PipelineId {
        self.create_pipeline("krudd triangle", TRIANGLE_SHADER)
    }

    /// Drops a pipeline. Every outstanding handle to it goes stale rather than
    /// resolving to whatever next occupies the slot.
    pub fn drop_pipeline(&mut self, id: PipelineId) -> bool {
        self.pipelines.remove(id)
    }

    /// The surface size the renderer is currently configured at.
    pub fn viewport(&self) -> Viewport {
        Viewport::new(self.config.width, self.config.height)
    }

    /// A one-line description of what actually got picked — backend, adapter
    /// and surface format.
    ///
    /// For the page's debug readout. "It says Gl" is the cheapest possible
    /// check that the thing under the canvas is what this crate claims.
    pub fn description(&self) -> String {
        let info = self.adapter.get_info();
        format!(
            "{:?} · {} · {:?}",
            info.backend, info.name, self.config.format
        )
    }

    /// The device error recorded since the last check, if any.
    pub fn take_device_error(&self) -> Option<String> {
        self.errors.take()
    }

    /// Reconfigures the surface to a new size.
    fn reconfigure(&mut self, viewport: Viewport) {
        self.config.width = viewport.width;
        self.config.height = viewport.height;
        self.surface.configure(&self.device, &self.config);
    }

    /// Acquires the frame's surface texture, if it has not been acquired yet.
    fn acquire(&mut self) -> Result<(), Error> {
        if self.acquired.is_some() {
            return Ok(());
        }
        let texture = match self.surface.get_current_texture() {
            wgpu::CurrentSurfaceTexture::Success(texture) => texture,
            // Suboptimal still draws correctly; reconfiguring is a performance
            // fix and belongs at the next frame boundary, not halfway through
            // this one.
            wgpu::CurrentSurfaceTexture::Suboptimal(texture) => texture,
            wgpu::CurrentSurfaceTexture::Outdated | wgpu::CurrentSurfaceTexture::Lost => {
                let viewport = self.viewport();
                self.reconfigure(viewport);
                return Err(Error::SurfaceOutdated);
            }
            wgpu::CurrentSurfaceTexture::Timeout => {
                return Err(Error::SurfaceUnavailable("timed out"));
            }
            wgpu::CurrentSurfaceTexture::Occluded => {
                return Err(Error::SurfaceUnavailable("the canvas is not visible"));
            }
            wgpu::CurrentSurfaceTexture::Validation => {
                return Err(Error::SurfaceUnavailable("a validation error was raised"));
            }
        };
        let view = texture
            .texture
            .create_view(&wgpu::TextureViewDescriptor::default());
        self.acquired = Some(Acquired {
            texture,
            view,
            written: false,
        });
        Ok(())
    }

    /// Uploads one uniform block per draw and returns the stride between them.
    fn upload_uniforms(&mut self, frame: &Frame) -> u64 {
        if frame.draws().len() > self.uniforms.capacity {
            self.uniforms = Uniforms::new(&self.device, frame.draws().len());
        }
        let stride = self.uniforms.stride;
        let padding = (stride - UNIFORM_BYTES) as usize;

        self.staging.clear();
        for draw in frame.draws() {
            let mvp = frame.view_projection.mul(&draw.transform);
            self.staging.extend(uniform_bytes(&mvp));
            self.staging.extend(std::iter::repeat_n(0u8, padding));
        }
        if !self.staging.is_empty() {
            self.queue
                .write_buffer(&self.uniforms.buffer, 0, &self.staging);
        }
        stride
    }

    /// Checks a frame before any of it is recorded.
    ///
    /// Up front rather than as it goes, so a frame with a bad draw in the
    /// middle of it is rejected whole instead of being half-drawn.
    fn validate(&self, frame: &Frame) -> Result<(), Error> {
        if frame.target.is_some() {
            return Err(Error::OffscreenTarget);
        }
        let surface = self.viewport();
        if frame.viewport != surface {
            return Err(Error::ViewportMismatch {
                frame: frame.viewport,
                surface,
            });
        }
        for draw in frame.draws() {
            let pipeline = self
                .pipelines
                .get(draw.pipeline)
                .ok_or(Error::UnknownPipeline(draw.pipeline))?;
            if pipeline.format != self.config.format {
                return Err(Error::PipelineFormat {
                    pipeline: format!("{:?}", pipeline.format),
                    target: format!("{:?}", self.config.format),
                });
            }
        }
        Ok(())
    }

    /// Records and submits one command buffer for a frame's draws.
    fn record(&self, frame: &Frame, acquired: &Acquired, stride: u64) -> Result<(), Error> {
        let mut encoder = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("krudd frame"),
            });
        {
            // Clear only if the frame asks for it *and* nothing has drawn into
            // this texture yet. A second submit in the same frame loads, or it
            // erases the first one's work — the same bug as releasing the
            // surface texture at submit, seen from the other side.
            let load = match frame.clear {
                Some(color) if !acquired.written => wgpu::LoadOp::Clear(to_wgpu_color(color)),
                _ => wgpu::LoadOp::Load,
            };
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("krudd forward"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &acquired.view,
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load,
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            for (index, draw) in frame.draws().iter().enumerate() {
                let pipeline = self
                    .pipelines
                    .get(draw.pipeline)
                    .ok_or(Error::UnknownPipeline(draw.pipeline))?;
                pass.set_pipeline(&pipeline.handle);
                let offset = stride * index as u64;
                pass.set_bind_group(0, &self.uniforms.bind_group, &[offset as u32]);
                pass.draw(
                    draw.first_vertex..draw.first_vertex + draw.vertex_count,
                    0..1,
                );
            }
        }
        self.queue.submit([encoder.finish()]);
        Ok(())
    }
}

impl Backend for Renderer {
    type Error = Error;

    fn begin_frame(&mut self, viewport: Viewport) -> Result<(), Error> {
        if self.open {
            return Err(Error::FrameStillOpen);
        }
        if let Some(why) = self.errors.take() {
            return Err(Error::Device(why));
        }
        if viewport != self.viewport() {
            self.reconfigure(viewport);
        }
        self.open = true;
        Ok(())
    }

    fn submit(&mut self, frame: &Frame) -> Result<(), Error> {
        if !self.open {
            return Err(Error::NoFrameOpen);
        }
        self.validate(frame)?;
        self.acquire()?;

        let stride = self.upload_uniforms(frame);
        // Taken out so the mutable borrow does not overlap the `&self` reads in
        // `record`; put back before returning, either way.
        let mut acquired = self.acquired.take().ok_or(Error::NoFrameOpen)?;
        let result = self.record(frame, &acquired, stride);
        acquired.written = true;
        self.acquired = Some(acquired);
        result
    }

    fn end_frame(&mut self) -> Result<(), Error> {
        // Cleared first, so a failure here cannot wedge the renderer into
        // rejecting every subsequent frame with `FrameStillOpen`.
        self.open = false;
        if let Some(acquired) = self.acquired.take() {
            self.queue.present(acquired.texture);
        }
        match self.errors.take() {
            Some(why) => Err(Error::Device(why)),
            None => Ok(()),
        }
    }
}

impl Uniforms {
    /// A uniform buffer with room for at least `draws` per-draw blocks.
    fn new(device: &wgpu::Device, draws: usize) -> Self {
        let capacity = draws.max(1).next_power_of_two();
        let stride = uniform_stride(device.limits().min_uniform_buffer_offset_alignment);
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("krudd uniforms"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::VERTEX,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    // One buffer, one bind group, one offset per draw — rather
                    // than a bind group per draw, which is the shape that makes
                    // a renderer allocate per object.
                    has_dynamic_offset: true,
                    min_binding_size: NonZeroU64::new(UNIFORM_BYTES),
                },
                count: None,
            }],
        });
        let buffer = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("krudd uniforms"),
            size: stride * capacity as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let bind_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("krudd uniforms"),
            layout: &layout,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                    buffer: &buffer,
                    offset: 0,
                    size: NonZeroU64::new(UNIFORM_BYTES),
                }),
            }],
        });
        Self {
            buffer,
            bind_group,
            layout,
            stride,
            capacity,
        }
    }
}

/// The stride between per-draw uniform blocks, given the device's
/// dynamic-offset alignment.
///
/// A dynamic offset must be a multiple of that alignment — 256 bytes on
/// WebGL2 — so consecutive 64-byte blocks are padded apart rather than packed.
/// Its own function because it is arithmetic worth a test, and testing it does
/// not need a GPU.
fn uniform_stride(alignment: u32) -> u64 {
    let alignment = u64::from(alignment).max(1);
    UNIFORM_BYTES.div_ceil(alignment) * alignment
}

/// One matrix as the bytes the shader reads.
///
/// Little-endian explicitly: that is what the GPU expects, whatever the host
/// happens to be.
fn uniform_bytes(mvp: &Mat4) -> impl Iterator<Item = u8> + use<'_> {
    mvp.as_slice().iter().flat_map(|v| v.to_le_bytes())
}

/// A [`krudd_render::Color`] as wgpu's.
///
/// Both are linear RGBA, so this is a widening and nothing else — no gamma
/// conversion belongs here.
fn to_wgpu_color(color: Color) -> wgpu::Color {
    wgpu::Color {
        r: f64::from(color.r),
        g: f64::from(color.g),
        b: f64::from(color.b),
        a: f64::from(color.a),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_uniform_stride_is_the_alignment_when_a_block_fits_in_one() {
        // WebGL2's alignment. One 64-byte block per 256-byte slot: three
        // quarters wasted, and unavoidable — a dynamic offset has to land on
        // the alignment.
        assert_eq!(uniform_stride(256), 256);
    }

    #[test]
    fn the_uniform_stride_rounds_up_rather_than_down() {
        // A device with a tighter alignment packs blocks closer; one whose
        // alignment does not divide 64 still has to clear the block.
        assert_eq!(uniform_stride(64), 64);
        assert_eq!(uniform_stride(32), 64);
        assert_eq!(uniform_stride(48), 96);
        assert!(uniform_stride(48) >= UNIFORM_BYTES);
    }

    #[test]
    fn a_zero_alignment_does_not_divide_by_zero() {
        // wgpu will not report zero, but a stride of zero would put every
        // draw's uniform at offset 0, and the bug would look like "every
        // entity draws in the same place".
        assert_eq!(uniform_stride(0), UNIFORM_BYTES);
    }

    #[test]
    fn a_uniform_block_is_sixteen_little_endian_floats() {
        let bytes: Vec<u8> = uniform_bytes(&Mat4::IDENTITY).collect();
        assert_eq!(bytes.len() as u64, UNIFORM_BYTES);
        assert_eq!(&bytes[0..4], &1.0f32.to_le_bytes());
        assert_eq!(&bytes[4..8], &0.0f32.to_le_bytes());
    }

    #[test]
    fn a_uniform_block_is_column_major() {
        // A transposed upload is the classic silent renderer bug: geometry
        // still draws, in the wrong place. The translation column has to land
        // in the last four floats, not strided through every fourth one.
        let m = Mat4::from_translation(krudd_math::Vec3::new(1.0, 2.0, 3.0));
        let bytes: Vec<u8> = uniform_bytes(&m).collect();
        assert_eq!(&bytes[48..52], &1.0f32.to_le_bytes());
        assert_eq!(&bytes[52..56], &2.0f32.to_le_bytes());
        assert_eq!(&bytes[56..60], &3.0f32.to_le_bytes());
    }

    #[test]
    fn colour_widens_without_converting() {
        let c = to_wgpu_color(Color::rgba(0.25, 0.5, 0.75, 1.0));
        assert_eq!((c.r, c.g, c.b, c.a), (0.25, 0.5, 0.75, 1.0));
    }

    #[test]
    fn errors_say_what_to_do_about_them() {
        // The shell puts these on the page verbatim, so they are user-facing
        // text and worth holding to it.
        assert_eq!(
            Error::SurfaceOutdated.to_string(),
            "the surface was reconfigured — retry the frame"
        );
        assert!(
            Error::ViewportMismatch {
                frame: Viewport::new(800, 600),
                surface: Viewport::new(400, 300),
            }
            .to_string()
            .contains("800x600")
        );
    }

    #[test]
    fn only_the_surface_errors_are_worth_skipping_a_frame_over() {
        assert!(Error::SurfaceOutdated.is_transient());
        assert!(Error::SurfaceUnavailable("timed out").is_transient());

        // These have to reach the page. A silent retry loop over a validation
        // error is a blank canvas that reports nothing wrong.
        assert!(!Error::Device("validation".to_string()).is_transient());
        assert!(!Error::UnknownPipeline(PipelineId::new(0, 0)).is_transient());
        assert!(!Error::OffscreenTarget.is_transient());
        assert!(!Error::FrameStillOpen.is_transient());
    }

    #[test]
    fn the_error_sink_keeps_the_first_error_not_the_last() {
        let sink = ErrorSink::default();
        sink.record("the cause".to_string());
        sink.record("the consequence".to_string());
        assert_eq!(sink.take().as_deref(), Some("the cause"));
        assert_eq!(sink.take(), None, "taking should drain it");
    }

    #[test]
    fn the_triangle_shader_declares_the_entry_points_the_pipeline_asks_for() {
        // create_pipeline names these explicitly rather than relying on there
        // being exactly one of each stage, so a rename in the WGSL that missed
        // the Rust would otherwise only fail in a browser.
        assert!(TRIANGLE_SHADER.contains("fn vertex_main"));
        assert!(TRIANGLE_SHADER.contains("fn fragment_main"));
        assert!(
            TRIANGLE_SHADER.contains("@group(0) @binding(0)"),
            "the shader must bind the uniform block the pipeline layout declares"
        );
    }
}
