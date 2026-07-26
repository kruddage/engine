// SPDX-License-Identifier: GPL-2.0-or-later

//! [`Recorder`]: a [`Commands`] implementation that logs every call instead
//! of touching a GPU.

use core::convert::Infallible;
use core::fmt;

use krudd_gpu::{Buffer, BufferId, IndexFormat, Pipeline, PipelineId, Texture, TextureId};
use krudd_render::{Barrier, BufferDesc, Commands, PipelineDesc, RenderPassDesc, TextureDesc};

use crate::call::Call;
use crate::slots::Slots;

/// Records every [`Commands`] call instead of executing it.
///
/// `type Error = Infallible`: a recorder cannot fail, and a test asserting on
/// a call log should never be unwrapping a result it already knows is `Ok`.
#[derive(Default)]
pub struct Recorder {
    calls: Vec<Call>,
    textures: Slots<Texture>,
    buffers: Slots<Buffer>,
    pipelines: Slots<Pipeline>,
}

impl Recorder {
    /// A recorder with an empty log and no live resources.
    pub fn new() -> Self {
        Self::default()
    }

    /// The calls made since the last [`Recorder::reset`], in order.
    pub fn calls(&self) -> &[Call] {
        &self.calls
    }

    /// Drains the call log.
    ///
    /// Handle state — which textures, buffers and pipelines are live — is
    /// untouched: only the log a test asserts on is reset, which is what lets
    /// a test `reset()` between the phases of a scenario (build the
    /// resources, then assert only on what one frame did with them) without
    /// losing track of what it already created.
    pub fn reset(&mut self) {
        self.calls.clear();
    }

    /// How many textures are currently live: created and not yet destroyed.
    ///
    /// The highest-value accessor here, not because it is complex but
    /// because of the bug it turns into a one-line assertion: a frame graph
    /// that leaks a transient texture across frames — "a write counts as a
    /// use", #823's stated hazard — shows up as this number climbing frame
    /// over frame, rather than as a slow memory grower nobody notices until
    /// it falls over.
    pub fn live_textures(&self) -> usize {
        self.textures.live_count()
    }
}

impl fmt::Display for Recorder {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // One call per line, numbered, so a failing assertion's output reads
        // as a sequence to scan — the reason this exists at all rather than
        // callers using `{:?}` on `calls()` directly.
        for (index, call) in self.calls.iter().enumerate() {
            writeln!(f, "{index}: {call}")?;
        }
        Ok(())
    }
}

impl Commands for Recorder {
    type Error = Infallible;

    fn create_texture(
        &mut self,
        desc: &TextureDesc,
        data: Option<&[u8]>,
    ) -> Result<TextureId, Self::Error> {
        let texture = self.textures.insert();
        self.calls.push(Call::CreateTexture {
            texture,
            format: desc.format,
            width: desc.width,
            height: desc.height,
            mip_levels: desc.mip_levels,
            has_data: data.is_some(),
        });
        Ok(texture)
    }

    fn destroy_texture(&mut self, texture: TextureId) -> Result<(), Self::Error> {
        self.textures.remove(texture);
        self.calls.push(Call::DestroyTexture { texture });
        Ok(())
    }

    fn create_buffer(
        &mut self,
        desc: &BufferDesc,
        data: Option<&[u8]>,
    ) -> Result<BufferId, Self::Error> {
        let buffer = self.buffers.insert();
        self.calls.push(Call::CreateBuffer {
            buffer,
            usage: desc.usage,
            size: desc.size,
            has_data: data.is_some(),
        });
        Ok(buffer)
    }

    fn destroy_buffer(&mut self, buffer: BufferId) -> Result<(), Self::Error> {
        self.buffers.remove(buffer);
        self.calls.push(Call::DestroyBuffer { buffer });
        Ok(())
    }

    fn create_pipeline(&mut self, desc: &PipelineDesc<'_>) -> Result<PipelineId, Self::Error> {
        let pipeline = self.pipelines.insert();
        self.calls.push(Call::CreatePipeline {
            pipeline,
            color_format_count: desc.color_formats.len(),
            vertex_has_source: desc.vertex_entry.is_some(),
            fragment_has_source: desc.fragment_entry.is_some(),
        });
        Ok(pipeline)
    }

    fn barrier(&mut self, barriers: &[Barrier]) -> Result<(), Self::Error> {
        self.calls.push(Call::Barrier {
            count: barriers.len(),
        });
        Ok(())
    }

    fn begin_render_pass(&mut self, desc: &RenderPassDesc<'_>) -> Result<(), Self::Error> {
        self.calls.push(Call::BeginRenderPass {
            color_count: desc.colors.len(),
            resolve_targets: desc
                .colors
                .iter()
                .map(|color| color.resolve_target.is_some())
                .collect(),
            color_loads: desc.colors.iter().map(|color| color.load).collect(),
            depth_load: desc.depth.map(|depth| depth.load),
        });
        Ok(())
    }

    fn end_render_pass(&mut self) -> Result<(), Self::Error> {
        self.calls.push(Call::EndRenderPass);
        Ok(())
    }

    fn set_pipeline(&mut self, pipeline: PipelineId) -> Result<(), Self::Error> {
        self.calls.push(Call::SetPipeline { pipeline });
        Ok(())
    }

    fn bind_vertex_buffer(
        &mut self,
        slot: u32,
        buffer: BufferId,
        offset: u64,
        size: u64,
    ) -> Result<(), Self::Error> {
        self.calls.push(Call::BindVertexBuffer {
            slot,
            buffer,
            offset,
            size,
        });
        Ok(())
    }

    fn bind_index_buffer(
        &mut self,
        buffer: BufferId,
        offset: u64,
        size: u64,
        format: IndexFormat,
    ) -> Result<(), Self::Error> {
        self.calls.push(Call::BindIndexBuffer {
            buffer,
            offset,
            size,
            format,
        });
        Ok(())
    }

    fn bind_uniform_buffer(
        &mut self,
        slot: u32,
        buffer: BufferId,
        offset: u64,
        size: u64,
    ) -> Result<(), Self::Error> {
        self.calls.push(Call::BindUniformBuffer {
            slot,
            buffer,
            offset,
            size,
        });
        Ok(())
    }

    fn draw(
        &mut self,
        first_vertex: u32,
        vertex_count: u32,
        instance_count: u32,
    ) -> Result<(), Self::Error> {
        self.calls.push(Call::Draw {
            first_vertex,
            vertex_count,
            instance_count,
        });
        Ok(())
    }

    fn draw_indexed(&mut self, index_count: u32, instance_count: u32) -> Result<(), Self::Error> {
        self.calls.push(Call::DrawIndexed {
            index_count,
            instance_count,
        });
        Ok(())
    }

    fn dispatch(&mut self, x: u32, y: u32, z: u32) -> Result<(), Self::Error> {
        self.calls.push(Call::Dispatch { x, y, z });
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use krudd_gpu::{BufferUsage, TextureFormat};
    use krudd_render::{Access, Color, ColorAttachment, DepthAttachment, LoadOp};

    fn texture_desc() -> TextureDesc {
        TextureDesc {
            format: TextureFormat::Rgba8Unorm,
            width: 4,
            height: 4,
            mip_levels: 1,
            sample_count: 1,
        }
    }

    #[test]
    fn calls_are_logged_in_the_order_they_were_made() {
        let mut recorder = Recorder::new();
        recorder.create_texture(&texture_desc(), None).unwrap();
        recorder.draw(0, 3, 1).unwrap();
        recorder.end_render_pass().unwrap();

        assert!(matches!(recorder.calls()[0], Call::CreateTexture { .. }));
        assert!(matches!(recorder.calls()[1], Call::Draw { .. }));
        assert!(matches!(recorder.calls()[2], Call::EndRenderPass));
    }

    #[test]
    fn reset_drains_the_log_but_not_live_resource_state() {
        let mut recorder = Recorder::new();
        recorder.create_texture(&texture_desc(), None).unwrap();
        recorder.reset();

        assert!(recorder.calls().is_empty());
        assert_eq!(
            recorder.live_textures(),
            1,
            "reset clears the log, not what was created through it"
        );
    }

    #[test]
    fn create_texture_records_format_size_mips_and_data_presence() {
        let mut recorder = Recorder::new();
        recorder
            .create_texture(
                &TextureDesc {
                    format: TextureFormat::Depth32Float,
                    width: 16,
                    height: 32,
                    mip_levels: 4,
                    sample_count: 1,
                },
                Some(&[0u8; 4]),
            )
            .unwrap();

        match recorder.calls()[0] {
            Call::CreateTexture {
                format,
                width,
                height,
                mip_levels,
                has_data,
                ..
            } => {
                assert_eq!(format, TextureFormat::Depth32Float);
                assert_eq!(width, 16);
                assert_eq!(height, 32);
                assert_eq!(mip_levels, 4);
                assert!(has_data);
            }
            ref other => panic!("expected CreateTexture, got {other:?}"),
        }
    }

    #[test]
    fn create_buffer_without_data_records_no_data_supplied() {
        let mut recorder = Recorder::new();
        recorder
            .create_buffer(
                &BufferDesc {
                    usage: BufferUsage::Vertex,
                    size: 64,
                },
                None,
            )
            .unwrap();

        match recorder.calls()[0] {
            Call::CreateBuffer { has_data, size, .. } => {
                assert!(!has_data);
                assert_eq!(size, 64);
            }
            ref other => panic!("expected CreateBuffer, got {other:?}"),
        }
    }

    #[test]
    fn create_pipeline_records_color_format_count_and_stage_presence() {
        let mut recorder = Recorder::new();
        let formats = [TextureFormat::Rgba8UnormSrgb, TextureFormat::Rgba8Unorm];
        recorder
            .create_pipeline(&PipelineDesc {
                label: "test",
                shader: "// wgsl",
                vertex_entry: Some("vs_main"),
                fragment_entry: None,
                color_formats: &formats,
                depth_format: None,
                sample_count: 1,
            })
            .unwrap();

        match recorder.calls()[0] {
            Call::CreatePipeline {
                color_format_count,
                vertex_has_source,
                fragment_has_source,
                ..
            } => {
                assert_eq!(color_format_count, 2);
                assert!(vertex_has_source);
                assert!(!fragment_has_source, "no fragment_entry was given");
            }
            ref other => panic!("expected CreatePipeline, got {other:?}"),
        }
    }

    #[test]
    fn begin_render_pass_records_resolve_presence_per_attachment_in_order() {
        let mut recorder = Recorder::new();
        let plain = ColorAttachment {
            texture: TextureId::new(0, 0),
            resolve_target: None,
            load: LoadOp::Clear(Color::BLACK),
        };
        let resolved = ColorAttachment {
            texture: TextureId::new(1, 0),
            resolve_target: Some(TextureId::new(2, 0)),
            load: LoadOp::Load,
        };
        recorder
            .begin_render_pass(&RenderPassDesc {
                label: "test pass",
                colors: &[plain, resolved],
                depth: None,
            })
            .unwrap();

        match &recorder.calls()[0] {
            Call::BeginRenderPass {
                color_count,
                resolve_targets,
                depth_load,
                ..
            } => {
                assert_eq!(*color_count, 2);
                assert_eq!(resolve_targets, &[false, true]);
                assert_eq!(*depth_load, None);
            }
            other => panic!("expected BeginRenderPass, got {other:?}"),
        }
    }

    #[test]
    fn begin_render_pass_records_the_depth_load_op_when_depth_is_present() {
        let mut recorder = Recorder::new();
        recorder
            .begin_render_pass(&RenderPassDesc {
                label: "shadow pass",
                colors: &[],
                depth: Some(DepthAttachment {
                    texture: TextureId::new(0, 0),
                    load: LoadOp::Clear(1.0),
                }),
            })
            .unwrap();

        match recorder.calls()[0] {
            Call::BeginRenderPass {
                color_count,
                depth_load,
                ..
            } => {
                assert_eq!(color_count, 0);
                assert_eq!(depth_load, Some(LoadOp::Clear(1.0)));
            }
            ref other => panic!("expected BeginRenderPass, got {other:?}"),
        }
    }

    #[test]
    fn barrier_records_only_the_count_not_the_transitions() {
        let mut recorder = Recorder::new();
        let barriers = [
            Barrier {
                texture: TextureId::new(0, 0),
                from: Access::RenderTarget,
                to: Access::Sampled,
            },
            Barrier {
                texture: TextureId::new(1, 0),
                from: Access::DepthTarget,
                to: Access::Sampled,
            },
        ];
        recorder.barrier(&barriers).unwrap();

        assert_eq!(recorder.calls()[0], Call::Barrier { count: 2 });
    }

    #[test]
    fn a_destroyed_then_recreated_texture_slot_does_not_answer_to_the_old_handle() {
        let mut recorder = Recorder::new();
        let first = recorder.create_texture(&texture_desc(), None).unwrap();
        recorder.destroy_texture(first).unwrap();
        let second = recorder.create_texture(&texture_desc(), None).unwrap();

        assert_eq!(first.slot(), second.slot(), "the slot should be reused");
        assert_ne!(first.generation(), second.generation());
    }

    #[test]
    fn live_textures_tracks_creates_and_destroys() {
        let mut recorder = Recorder::new();
        assert_eq!(recorder.live_textures(), 0);

        let a = recorder.create_texture(&texture_desc(), None).unwrap();
        let _b = recorder.create_texture(&texture_desc(), None).unwrap();
        assert_eq!(recorder.live_textures(), 2);

        recorder.destroy_texture(a).unwrap();
        assert_eq!(recorder.live_textures(), 1);
    }

    #[test]
    fn display_prints_one_numbered_line_per_call() {
        let mut recorder = Recorder::new();
        recorder.end_render_pass().unwrap();
        recorder.dispatch(1, 1, 1).unwrap();

        let printed = recorder.to_string();
        let lines: Vec<&str> = printed.lines().collect();
        assert_eq!(lines.len(), 2);
        assert!(lines[0].starts_with("0:"));
        assert!(lines[1].starts_with("1:"));
    }
}
