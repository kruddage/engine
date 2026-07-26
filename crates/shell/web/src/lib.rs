// SPDX-License-Identifier: GPL-2.0-or-later

//! The wasm module the browser loads.
//!
//! **Tier: `shell`.** Last in the order on purpose: a shell may reach for
//! anything, and nothing may reach for a shell. This is the only crate that
//! knows wasm-bindgen exists — every crate below it compiles for the host
//! unchanged, which is what keeps `cargo test` a real test run rather than a
//! browser harness.
//!
//! ## What the boundary looks like
//!
//! The exported surface here is deliberately tiny, and the path the engine
//! takes through it is never per-object. [`Engine::tick`] advances the whole
//! world in one call, and TypeScript reads the result as a `Float32Array`
//! mapped straight over wasm linear memory via [`Engine::positions_ptr`] — no
//! serialisation, no copy, no call per entity. Violating that would look like
//! "wasm is slow" and would not be.
//!
//! The full contract — who allocates, who frees, when a view goes stale, and
//! what the per-call path costs when measured rather than asserted — is
//! `docs/boundary.md`. [`Engine::position_of`] and [`Engine::set_position`]
//! are the per-call path, kept so the benchmark there has something to
//! measure the batched path against.
//!
//! ## Booting is async, and that is the renderer's fault
//!
//! Requesting a GPU adapter and a device are both async on the web, so a
//! `#[wasm_bindgen(constructor)]` cannot do it — a constructor cannot return a
//! promise. [`start`] is the real entry point: it takes the canvas, awaits the
//! device, and hands back an [`Engine`] that is already drawing. [`Engine::new`]
//! survives as the renderer-less form, which is what the host tests below
//! construct.
//!
//! ## Scope
//!
//! This is the loadable artifact #815 asks for and the thing #816's TypeScript
//! build links against, not the engine. It draws the triangle #818 asks for and
//! nothing beyond it: no scene, no camera controls, no assets. Generating both
//! sides of this boundary from one spec, rather than hand-writing the pair, is
//! #824.

use krudd_gpu::PipelineId;
use krudd_math::{Mat4, Vec3};
use krudd_render::{Backend, Draw, Frame, Viewport};
use krudd_webgl::Renderer;
use krudd_world::{Handle, Store};
use wasm_bindgen::JsValue;
use wasm_bindgen::prelude::wasm_bindgen;

/// Half the height of the visible world, in world units.
///
/// The camera is a fixed orthographic box this tall, widened by the canvas
/// aspect so a triangle is a triangle rather than whatever shape the window
/// happens to be.
const VIEW_EXTENT: f32 = 2.0;

/// How large one entity's triangle draws, in world units.
const TRIANGLE_SCALE: f32 = 0.35;

/// Boots an engine that draws into `canvas`.
///
/// The canvas is taken by value and kept by the surface for the lifetime of the
/// engine, which is what lets the surface be `'static` — a borrowed canvas
/// would put a lifetime on [`Engine`], and a `#[wasm_bindgen]` type cannot
/// carry one.
///
/// The engine's viewport comes from `canvas.width`/`height`, the canvas's
/// *drawing buffer* size in physical pixels — not its CSS size. The page is
/// responsible for setting them from `devicePixelRatio`, because the page is
/// the half that can read it, and for putting them through
/// [`fit_drawing_buffer`] first, because a drawing buffer past the WebGL2
/// limit is a validation error rather than a clamp.
#[cfg(target_arch = "wasm32")]
#[wasm_bindgen]
pub async fn start(canvas: web_sys::HtmlCanvasElement) -> Result<Engine, JsValue> {
    let mut engine = Engine::new(canvas.width(), canvas.height());
    engine
        .attach(wgpu::SurfaceTarget::Canvas(canvas))
        .await
        .map_err(|why| JsValue::from_str(&why.to_string()))?;
    Ok(engine)
}

/// The largest drawing buffer the renderer can present, given a requested one:
/// `[width, height]` in physical pixels.
///
/// A canvas at `devicePixelRatio` on a portrait phone is routinely taller than
/// the 2048 the WebGL2 backend configures a surface within
/// ([`krudd_webgl::MAX_SURFACE_EXTENT`]), and asking for more is a validation
/// error that takes the page down rather than a clamp that costs resolution.
/// So the page scales the buffer down to fit, keeping the aspect — an
/// independently clamped side would be stretched back over the canvas's CSS
/// box and read as a squashed image.
///
/// Exported because the page has to size the drawing buffer before [`start`]
/// has a device to ask, and because the arithmetic must be *the same*
/// arithmetic on both sides: a page that fitted to a slightly different size
/// than the renderer would set the buffer, have wgpu set it back, and do it
/// again on every resize event. `fitCanvas` in `@krudd/boundary` is the one
/// caller.
#[wasm_bindgen]
pub fn fit_drawing_buffer(width: u32, height: u32) -> Box<[u32]> {
    let fitted = Viewport::new(width, height).fit_within(krudd_webgl::MAX_SURFACE_EXTENT);
    Box::new([fitted.width, fitted.height])
}

/// The engine version, from `version.txt` by way of the build.
///
/// Stamped in by `cargo xtask` so the page can report exactly what it is
/// running; falls back to `dev` for a plain `cargo build`.
pub const VERSION: &str = match option_env!("KRUDD_VERSION") {
    Some(v) => v,
    None => "dev",
};

/// The engine version string.
#[wasm_bindgen]
pub fn version() -> String {
    VERSION.to_string()
}

/// One running engine.
///
/// Owns the world and the column that TypeScript reads. Held by the shell for
/// the lifetime of the page.
#[wasm_bindgen]
pub struct Engine {
    store: Store,
    /// The position column, one [`Vec3`] per slot, indexed by
    /// [`Handle::index`]. Sized to [`Store::capacity`], so a tombstoned slot
    /// still occupies its three floats.
    positions: Vec<Vec3>,
    /// The per-slot velocity that [`Engine::tick`] integrates. Private: it is
    /// engine state, not something the page reads.
    velocities: Vec<Vec3>,
    viewport: Viewport,
    frame_count: u32,
    elapsed: f32,
    /// The backend, once a canvas has been attached. `None` in the host tests,
    /// which have no canvas and want none — every method below works without
    /// it except [`Engine::render`].
    renderer: Option<Renderer>,
    /// The one pipeline, compiled when the renderer is attached.
    triangle: Option<PipelineId>,
}

impl Default for Engine {
    fn default() -> Self {
        Self::new(1, 1)
    }
}

#[wasm_bindgen]
impl Engine {
    /// An engine with no renderer, at a viewport of the given size in physical
    /// pixels, fitted within what the renderer will be able to present.
    ///
    /// It ticks, spawns and hands out its columns; it cannot draw. [`start`] is
    /// what attaches a canvas, and the page always goes through that — this is
    /// the form the host tests and the Node harnesses build, neither of which
    /// has a canvas to give it.
    #[wasm_bindgen(constructor)]
    pub fn new(width: u32, height: u32) -> Self {
        Self {
            store: Store::new(),
            positions: Vec::new(),
            velocities: Vec::new(),
            viewport: Viewport::new(width, height).fit_within(krudd_webgl::MAX_SURFACE_EXTENT),
            frame_count: 0,
            elapsed: 0.0,
            renderer: None,
            triangle: None,
        }
    }

    /// Draws the world.
    ///
    /// Separate from [`Engine::tick`] on purpose: simulating and drawing are
    /// different rates, and a page that wants to tick twice and draw once
    /// should be able to. The page calls both once per animation frame today.
    ///
    /// A transient surface failure — a resize landing between frames, a
    /// backgrounded tab — is a skipped frame and reports success. Anything else
    /// is thrown, because the page's job is to put it on screen: a canvas that
    /// silently stops drawing looks exactly like a canvas that was drawing
    /// nothing all along.
    pub fn render(&mut self) -> Result<(), JsValue> {
        self.render_frame()
            .map_err(|why| JsValue::from_str(&why.to_string()))
    }

    /// What the renderer picked — backend, adapter and surface format — or
    /// `undefined` if no canvas is attached.
    ///
    /// The page shows this. "It says Gl" is the cheapest available check that
    /// the thing under the canvas is WebGL2 and not something else.
    pub fn renderer_description(&self) -> Option<String> {
        self.renderer.as_ref().map(Renderer::description)
    }

    /// How many draws the last [`Engine::render`] would submit.
    ///
    /// For the page's debug readout, and for a test that wants to know the
    /// world reached the frame without needing a GPU to ask.
    pub fn draw_count(&self) -> u32 {
        self.build_frame().draws().len() as u32
    }

    /// Spawns an entity and returns its slot index.
    ///
    /// The slot index, not the generational handle: a `u32` is what a
    /// TypeScript caller can hold, and it is also the index into the position
    /// view, which is the only thing the page does with it. Handing out the
    /// generation as well is #824's business, once the boundary is generated
    /// rather than hand-written.
    pub fn spawn(&mut self, x: f32, y: f32, z: f32) -> u32 {
        let handle = self.store.alloc();
        self.grow_columns_to_fit();
        let slot = handle.index() as usize;
        self.positions[slot] = Vec3::new(x, y, z);
        self.velocities[slot] = Vec3::ZERO;
        handle.index()
    }

    /// Sets an entity's velocity, in units per second.
    ///
    /// Returns whether the slot was live.
    pub fn set_velocity(&mut self, slot: u32, x: f32, y: f32, z: f32) -> bool {
        match self.live_handle(slot) {
            Some(handle) => {
                self.velocities[handle.index() as usize] = Vec3::new(x, y, z);
                true
            }
            None => false,
        }
    }

    /// Advances the world by `dt` seconds.
    ///
    /// One call for the whole world, not one per entity — see the module
    /// docs.
    pub fn tick(&mut self, dt: f32) {
        for handle in self.store.iter() {
            let slot = handle.index() as usize;
            let v = self.velocities[slot];
            let p = &mut self.positions[slot];
            p.x += v.x * dt;
            p.y += v.y * dt;
            p.z += v.z * dt;
        }
        self.elapsed += dt;
        self.frame_count += 1;
    }

    /// The address of the position column in wasm linear memory.
    ///
    /// Paired with [`Engine::positions_len`] to build a `Float32Array` view.
    /// The view is invalidated by anything that can grow the column
    /// ([`Engine::spawn`]) and by wasm memory growth, so the caller rebuilds
    /// it whenever [`Engine::positions_len`] changes — `@krudd/boundary` does
    /// that for it.
    ///
    /// The column stays owned by Rust: the pointer is a loan for the lifetime
    /// of the next call in, not a transfer. See `docs/boundary.md`.
    pub fn positions_ptr(&self) -> usize {
        self.positions.as_ptr() as usize
    }

    /// The length of the position column, in `f32`s — three per slot.
    pub fn positions_len(&self) -> usize {
        self.positions.len() * 3
    }

    /// How many slots the position column covers, live and tombstoned alike.
    pub fn slot_count(&self) -> u32 {
        self.store.capacity() as u32
    }

    /// How many entities are live.
    pub fn entity_count(&self) -> u32 {
        self.store.len() as u32
    }

    /// How many times [`Engine::tick`] has been called.
    pub fn frame_count(&self) -> u32 {
        self.frame_count
    }

    /// Seconds of simulated time since boot.
    pub fn elapsed(&self) -> f32 {
        self.elapsed
    }

    /// Tells the engine the canvas changed size, in physical pixels.
    ///
    /// Fitted within what the renderer can present, for the same reason
    /// [`fit_drawing_buffer`] exists — and it has to happen here as well as on
    /// the page, because this viewport is what the frame is submitted at and a
    /// frame that disagrees with the surface is refused outright.
    /// [`Engine::width`] and [`Engine::height`] report what it settled on.
    pub fn resize(&mut self, width: u32, height: u32) {
        self.viewport = Viewport::new(width, height).fit_within(self.max_extent());
    }

    /// The current viewport width in physical pixels.
    pub fn width(&self) -> u32 {
        self.viewport.width
    }

    /// The current viewport height in physical pixels.
    pub fn height(&self) -> u32 {
        self.viewport.height
    }

    /// Despawns an entity. Returns whether the slot was live.
    pub fn despawn(&mut self, slot: u32) -> bool {
        match self.live_handle(slot) {
            Some(handle) => self.store.free(handle),
            None => false,
        }
    }

    /// One entity's position as a fresh `Float32Array`, or `undefined` if the
    /// slot is not live.
    ///
    /// **This is the per-call path, and the contract forbids the engine from
    /// taking it.** It is exported because a claim nobody measured is a claim
    /// nobody can defend: `cargo xtask bench` reads a whole world through
    /// here and through the batched view, and reports the difference. Every
    /// call is a boundary crossing plus an allocation the collector then has
    /// to take back — which is the cost the column exists to avoid.
    pub fn position_of(&self, slot: u32) -> Option<Box<[f32]>> {
        let handle = self.live_handle(slot)?;
        let p = self.positions[handle.index() as usize];
        Some(Box::new([p.x, p.y, p.z]))
    }

    /// Moves one entity. Returns whether the slot was live.
    ///
    /// The per-call write path, and the counterpart to [`Engine::position_of`]
    /// — same reason for existing, same rule against using it. The batched
    /// form is to write into the `Float32Array` view directly, which reaches
    /// the same bytes with no crossing at all.
    pub fn set_position(&mut self, slot: u32, x: f32, y: f32, z: f32) -> bool {
        match self.live_handle(slot) {
            Some(handle) => {
                self.positions[handle.index() as usize] = Vec3::new(x, y, z);
                true
            }
            None => false,
        }
    }
}

impl Engine {
    /// Boots a backend against a surface target and compiles the pipeline.
    ///
    /// Takes a [`wgpu::SurfaceTarget`] rather than a canvas so that the async
    /// plumbing is one function on every target, and only [`start`] — which is
    /// wasm-only, because `SurfaceTarget::Canvas` is — has to know what kind of
    /// surface this is.
    ///
    /// Not exported to JavaScript: an exported async method would have to hold
    /// `&mut self` across an await, which wasm-bindgen cannot express. [`start`]
    /// is the exported form, and it owns the engine while it awaits.
    pub async fn attach(
        &mut self,
        target: wgpu::SurfaceTarget<'static>,
    ) -> Result<(), krudd_webgl::Error> {
        let mut renderer = Renderer::new(target, self.viewport).await?;
        // The surface is the authority on its own size: it fits the viewport
        // within what the device can present, and on the web wgpu sizes the
        // canvas's drawing buffer from the configuration it settled on. Adopting
        // it here rather than asserting the two agree is what keeps the frame,
        // the camera's aspect and the canvas describing one size.
        self.viewport = renderer.viewport();
        // Compiled once, off-frame, against the device — pipelines outlive the
        // frame and must never be created through a lent frame context. See
        // krudd-webgl's module docs.
        self.triangle = Some(renderer.create_triangle_pipeline());
        self.renderer = Some(renderer);
        Ok(())
    }

    /// The largest either dimension of the viewport may be.
    ///
    /// The attached renderer's device limit, or — before there is one — the
    /// limit the device request will pin it to. The two agree; asking the
    /// renderer once it exists means a device that somehow reported less could
    /// not leave the engine submitting frames at a size its surface refused.
    fn max_extent(&self) -> u32 {
        self.renderer
            .as_ref()
            .map_or(krudd_webgl::MAX_SURFACE_EXTENT, Renderer::max_extent)
    }

    /// [`Engine::render`] without the `JsValue`.
    ///
    /// The exported method is a one-line wrapper around this so that the
    /// decision of what is worth reporting is testable: `JsValue::from_str`
    /// panics off wasm, so anything that touches it can only be exercised in a
    /// browser.
    fn render_frame(&mut self) -> Result<(), RenderError> {
        let frame = self.build_frame();
        let renderer = self.renderer.as_mut().ok_or(RenderError::Detached)?;
        match draw(renderer, &frame) {
            Ok(()) => Ok(()),
            Err(why) if why.is_transient() => Ok(()),
            Err(why) => Err(RenderError::Backend(why)),
        }
    }

    /// Builds the frame the backend draws: one triangle per live entity.
    ///
    /// Not exported: a `Frame` is Rust's own value and there is nothing useful
    /// the page could do with one. It is `pub` so [`Engine::draw_count`] and the
    /// tests can read what the world would submit without a GPU in the room.
    pub fn build_frame(&self) -> Frame {
        let mut frame = Frame::new(self.viewport);
        frame.view_projection = self.view_projection();
        let Some(pipeline) = self.triangle else {
            // No canvas attached, so no pipeline to name. An empty frame is
            // correct rather than an error: the world still ticks.
            return frame;
        };
        let scale = Mat4::from_scale(Vec3::new(TRIANGLE_SCALE, TRIANGLE_SCALE, 1.0));
        for handle in self.store.iter() {
            let position = self.positions[handle.index() as usize];
            frame.push(Draw {
                pipeline,
                transform: Mat4::from_translation(position).mul(&scale),
                first_vertex: 0,
                vertex_count: 3,
            });
        }
        frame
    }

    /// The camera: a fixed orthographic box [`VIEW_EXTENT`] tall, widened by
    /// the canvas aspect.
    ///
    /// Widened rather than squashed, so resizing the window shows more world
    /// instead of stretching what is already on screen — and a triangle stays
    /// the shape the shader describes at any canvas size.
    fn view_projection(&self) -> Mat4 {
        let half_width = VIEW_EXTENT * self.viewport.aspect();
        Mat4::orthographic(
            -half_width,
            half_width,
            -VIEW_EXTENT,
            VIEW_EXTENT,
            -1.0,
            1.0,
        )
    }

    /// Resolves a slot index to a live handle, or `None` if the slot is
    /// tombstoned or out of range.
    fn live_handle(&self, slot: u32) -> Option<Handle> {
        self.store.handle(slot)
    }

    /// Extends the columns to cover every slot the store can hand out.
    ///
    /// Called after every alloc rather than sized up front: the store reuses
    /// tombstones, so the columns only grow when it has genuinely run out of
    /// slots to recycle.
    fn grow_columns_to_fit(&mut self) {
        self.positions.resize(self.store.capacity(), Vec3::ZERO);
        self.velocities.resize(self.store.capacity(), Vec3::ZERO);
    }
}

/// Why a frame did not draw.
///
/// Private, and deliberately not a `JsValue`: the page receives these as
/// strings, and keeping the decision in Rust types is what lets the host tests
/// assert on it.
#[derive(Debug)]
enum RenderError {
    /// No canvas has been attached, so there is nothing to draw into.
    Detached,
    /// The backend refused the frame.
    Backend(krudd_webgl::Error),
}

impl core::fmt::Display for RenderError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::Detached => write!(
                f,
                "the engine has no renderer — it was built without a canvas"
            ),
            Self::Backend(why) => write!(f, "{why}"),
        }
    }
}

/// Draws one frame, keeping `begin_frame` / `end_frame` paired whatever
/// happens in between.
///
/// `end_frame` runs even when `submit` failed, and that is the whole reason
/// this is a function rather than three lines inlined into [`Engine::render`]:
/// it is the only place that releases the frame's surface texture, so an early
/// return past it leaves the surface holding a texture nobody presented and the
/// next frame with nothing to acquire. `submit`'s error is the one reported —
/// `end_frame`'s is usually a consequence of it.
fn draw(renderer: &mut Renderer, frame: &Frame) -> Result<(), krudd_webgl::Error> {
    renderer.begin_frame(frame.viewport)?;
    let submitted = renderer.submit(frame);
    let ended = renderer.end_frame();
    submitted.and(ended)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A pipeline handle for the tests, which have no device to make a real one
    /// with. The backend would reject it; `build_frame` only needs something to
    /// name.
    fn fake_pipeline() -> PipelineId {
        PipelineId::new(0, 0)
    }

    #[test]
    fn a_fresh_engine_has_no_entities_and_no_frames() {
        let e = Engine::new(800, 600);
        assert_eq!(e.entity_count(), 0);
        assert_eq!(e.frame_count(), 0);
        assert_eq!(e.positions_len(), 0);
        assert_eq!(e.width(), 800);
        assert_eq!(e.height(), 600);
    }

    #[test]
    fn spawning_grows_the_column_by_three_floats() {
        let mut e = Engine::default();
        e.spawn(1.0, 2.0, 3.0);
        assert_eq!(e.entity_count(), 1);
        assert_eq!(e.positions_len(), 3);
        assert_eq!(e.positions[0], Vec3::new(1.0, 2.0, 3.0));
    }

    #[test]
    fn tick_integrates_velocity_for_every_live_entity() {
        let mut e = Engine::default();
        let a = e.spawn(0.0, 0.0, 0.0);
        let b = e.spawn(10.0, 0.0, 0.0);
        assert!(e.set_velocity(a, 1.0, 0.0, 0.0));
        assert!(e.set_velocity(b, -1.0, 0.0, 0.0));

        e.tick(0.5);

        assert_eq!(e.positions[a as usize].x, 0.5);
        assert_eq!(e.positions[b as usize].x, 9.5);
        assert_eq!(e.frame_count(), 1);
        assert_eq!(e.elapsed(), 0.5);
    }

    #[test]
    fn a_despawned_entity_stops_moving_but_keeps_its_slot() {
        let mut e = Engine::default();
        let a = e.spawn(0.0, 0.0, 0.0);
        e.set_velocity(a, 1.0, 0.0, 0.0);
        assert!(e.despawn(a));

        e.tick(1.0);

        assert_eq!(e.entity_count(), 0);
        // The column keeps its width: the slot is tombstoned, not removed, so
        // every other entity's index — and the view TypeScript holds — stays
        // valid.
        assert_eq!(e.positions_len(), 3);
        assert_eq!(e.positions[a as usize].x, 0.0);
    }

    #[test]
    fn despawning_a_slot_twice_is_rejected() {
        let mut e = Engine::default();
        let a = e.spawn(0.0, 0.0, 0.0);
        assert!(e.despawn(a));
        assert!(!e.despawn(a));
        assert!(!e.set_velocity(a, 1.0, 0.0, 0.0));
    }

    #[test]
    fn a_recycled_slot_is_reset_not_inherited() {
        let mut e = Engine::default();
        let a = e.spawn(5.0, 5.0, 5.0);
        e.set_velocity(a, 9.0, 9.0, 9.0);
        e.despawn(a);

        let b = e.spawn(0.0, 0.0, 0.0);
        assert_eq!(a, b, "the tombstone should have been reused");
        e.tick(1.0);
        assert_eq!(
            e.positions[b as usize],
            Vec3::ZERO,
            "the recycled slot kept the old entity's velocity"
        );
        assert_eq!(e.slot_count(), 1);
    }

    #[test]
    fn the_per_call_path_reads_what_the_column_holds() {
        let mut e = Engine::default();
        let a = e.spawn(1.0, 2.0, 3.0);
        assert_eq!(
            e.position_of(a).as_deref(),
            Some([1.0, 2.0, 3.0].as_slice()),
            "the benchmark's two paths have to agree, or it measures nothing"
        );
    }

    #[test]
    fn the_per_call_path_refuses_a_dead_slot() {
        let mut e = Engine::default();
        let a = e.spawn(1.0, 2.0, 3.0);
        assert!(e.despawn(a));
        assert!(e.position_of(a).is_none());
        assert!(!e.set_position(a, 9.0, 9.0, 9.0));
        assert!(e.position_of(a + 1).is_none(), "out of range is not live");
    }

    #[test]
    fn a_per_call_write_lands_where_the_view_reads() {
        let mut e = Engine::default();
        let a = e.spawn(0.0, 0.0, 0.0);
        assert!(e.set_position(a, 4.0, 5.0, 6.0));
        assert_eq!(e.positions[a as usize], Vec3::new(4.0, 5.0, 6.0));
    }

    #[test]
    fn resize_clamps_a_zero_sized_canvas() {
        let mut e = Engine::new(800, 600);
        e.resize(0, 0);
        assert_eq!((e.width(), e.height()), (1, 1));
    }

    #[test]
    fn a_viewport_past_the_webgl2_limit_is_fitted_rather_than_configured() {
        // A 1080x2256 portrait phone at its device pixel ratio. Configuring a
        // surface that tall is a validation error — "must be within the maximum
        // supported texture size" — which took the page down on every Android
        // browser. The engine renders a little softer instead.
        let mut e = Engine::new(1080, 2256);
        assert_eq!(e.height(), krudd_webgl::MAX_SURFACE_EXTENT);
        assert!(e.width() < 1080, "both sides scale, or the image squashes");

        e.resize(1080, 2256);
        assert_eq!(e.height(), krudd_webgl::MAX_SURFACE_EXTENT);

        // And the camera follows the viewport, so the fitted frame still shows
        // the same box of world the unfitted one would have.
        let fitted = e.build_frame().viewport;
        assert_eq!(fitted, Viewport::new(e.width(), e.height()));
        assert!((fitted.aspect() - Viewport::new(1080, 2256).aspect()).abs() < 0.001);
    }

    #[test]
    fn a_viewport_within_the_limit_is_left_exactly_alone() {
        // The overwhelmingly common case, and the one a rounding bug in the
        // fitting would show up in first.
        let mut e = Engine::new(1920, 1080);
        assert_eq!((e.width(), e.height()), (1920, 1080));
        e.resize(2048, 2048);
        assert_eq!((e.width(), e.height()), (2048, 2048));
    }

    #[test]
    fn the_frame_carries_the_current_viewport() {
        let mut e = Engine::new(320, 240);
        e.resize(640, 480);
        assert_eq!(e.build_frame().viewport, Viewport::new(640, 480));
    }

    #[test]
    fn a_frame_without_a_pipeline_draws_nothing_rather_than_failing() {
        // The host tests, and any page whose canvas has not been attached yet.
        // The world still ticks; there is simply nothing to submit.
        let mut e = Engine::default();
        e.spawn(0.0, 0.0, 0.0);
        assert_eq!(e.draw_count(), 0);
        assert!(e.build_frame().draws().is_empty());
    }

    #[test]
    fn every_live_entity_becomes_one_triangle() {
        let mut e = Engine::new(100, 100);
        e.triangle = Some(fake_pipeline());
        e.spawn(0.0, 0.0, 0.0);
        let b = e.spawn(1.0, 0.0, 0.0);
        e.spawn(2.0, 0.0, 0.0);
        assert_eq!(e.draw_count(), 3);

        assert!(e.despawn(b));
        assert_eq!(e.draw_count(), 2, "a tombstoned slot is not drawn");

        let frame = e.build_frame();
        for draw in frame.draws() {
            assert_eq!(draw.vertex_count, 3, "a triangle is three vertices");
            assert_eq!(draw.first_vertex, 0);
        }
    }

    #[test]
    fn a_draws_transform_carries_the_entitys_position() {
        let mut e = Engine::new(100, 100);
        e.triangle = Some(fake_pipeline());
        e.spawn(3.0, -4.0, 0.0);

        let frame = e.build_frame();
        let transform = frame.draws()[0].transform;
        // The translation column, and the scale on the diagonal: scale applied
        // first, then the translation, which is what `translation * scale`
        // means. Getting the order backwards would scale the position too, and
        // it would present as entities clustered near the origin.
        assert_eq!(transform.cols[3][0], 3.0);
        assert_eq!(transform.cols[3][1], -4.0);
        assert_eq!(transform.cols[0][0], TRIANGLE_SCALE);
        assert_eq!(transform.cols[1][1], TRIANGLE_SCALE);
    }

    #[test]
    fn the_camera_widens_with_the_canvas_rather_than_stretching() {
        // A wide canvas shows more world sideways; the vertical extent is
        // fixed. If this inverted, a triangle would be visibly squashed at any
        // aspect but 1:1.
        let square = Engine::new(400, 400).view_projection();
        let wide = Engine::new(800, 400).view_projection();
        assert_eq!(square.cols[1][1], wide.cols[1][1], "y scale is fixed");
        assert!(
            wide.cols[0][0] < square.cols[0][0],
            "a wider canvas maps a wider box onto the same clip range"
        );
        // A square canvas is isotropic: x and y scale alike.
        assert_eq!(square.cols[0][0], square.cols[1][1]);
    }

    #[test]
    fn rendering_without_a_canvas_is_an_error_rather_than_a_silent_no_op() {
        // The page turns this into text. A `render` that quietly did nothing
        // would leave a blank canvas and no explanation, which is the one thing
        // #812 says an instrument may never do.
        let mut e = Engine::default();
        let why = e.render_frame().expect_err("a detached engine cannot draw");
        assert!(matches!(why, RenderError::Detached));
        assert!(
            why.to_string().contains("canvas"),
            "the message reaches the page verbatim, so it has to say what is wrong"
        );
        assert!(e.renderer_description().is_none());
    }

    #[test]
    fn version_is_reported() {
        assert!(!version().is_empty());
    }
}
