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

/// How large one entity's triangle draws, in world units, before a board says
/// otherwise.
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

/// The position column's name: three `f32`s per slot, in world units.
const POSITION: &str = "position";

/// The velocity column's name: three `f32`s per slot, in units per second.
const VELOCITY: &str = "velocity";

/// The colour column's name: three `f32`s per slot, multiplied into the
/// shader's own vertex colour rather than replacing it — see [`WHITE_FILL`].
const COLOUR: &str = "colour";

/// What a fresh or newly-grown slot of the colour column is filled with.
///
/// White, not the zero every other column grows into: the fragment shader
/// multiplies its vertex colour by this column (see `triangle.wgsl`), and
/// white is the multiplicative identity, so an entity nobody has coloured
/// draws exactly as it always did. Zero would multiply every triangle to
/// black — invisible, not merely uncoloured — which is the bug this constant
/// exists to make impossible rather than merely tested against.
const WHITE_FILL: f32 = 1.0;

/// The element type a named column holds.
///
/// A closed set rather than a generic type parameter — there is a handful of
/// columns in this engine, not an open type system, and `ensure_column` takes
/// this as the string a caller crossing the boundary can name.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ElementKind {
    /// Three `f32`s per slot — a position, a velocity, anything spatial.
    Vec3,
    /// One `f32` per slot.
    F32,
    /// One `u32` per slot.
    U32,
}

impl ElementKind {
    /// How many `f32`s or `u32`s one slot occupies in this column.
    fn components(self) -> usize {
        match self {
            Self::Vec3 => 3,
            Self::F32 | Self::U32 => 1,
        }
    }

    /// Whether this kind is backed by `u32`s rather than `f32`s.
    ///
    /// What `Engine::column_is_ints` reports, so the TypeScript side picks
    /// `Uint32Array` over `Float32Array` for the view.
    fn is_ints(self) -> bool {
        matches!(self, Self::U32)
    }

    /// Parses the `kind` string `Engine::ensure_column` takes from
    /// JavaScript.
    ///
    /// Returns [`ColumnError`] rather than `JsValue` for the same reason
    /// [`Engine::present`] returns `RenderError` instead of one: `JsValue`
    /// panics off wasm, so keeping it out of the fallible internals is what
    /// lets the host tests exercise the error path at all.
    fn parse(kind: &str) -> Result<Self, ColumnError> {
        match kind {
            "vec3" => Ok(Self::Vec3),
            "f32" => Ok(Self::F32),
            "u32" => Ok(Self::U32),
            other => Err(ColumnError::UnknownKind(other.to_string())),
        }
    }
}

/// Why [`Engine::ensure_column`] refused to create or reuse a column.
///
/// Private, and deliberately not a `JsValue` — see [`ElementKind::parse`].
#[derive(Debug)]
enum ColumnError {
    /// `kind` was not `"vec3"`, `"f32"` or `"u32"`.
    UnknownKind(String),
    /// The column exists already, at a different [`ElementKind`].
    KindConflict {
        name: String,
        existing: ElementKind,
        requested: ElementKind,
    },
}

impl core::fmt::Display for ColumnError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::UnknownKind(kind) => write!(
                f,
                "unknown column kind \"{kind}\" — expected \"vec3\", \"f32\" or \"u32\""
            ),
            Self::KindConflict {
                name,
                existing,
                requested,
            } => write!(
                f,
                "column \"{name}\" already exists as {existing:?}; refusing to \
                 re-create it as {requested:?} — that would reinterpret its bytes"
            ),
        }
    }
}

/// A named column's storage: one `Vec` per element width.
///
/// An enum over the two backing vectors, not a generic — `f32` also backs
/// [`ElementKind::Vec3`], at three elements per slot, so the storage variant
/// alone would not say how many elements one slot occupies. [`Column::kind`]
/// carries that; this carries the bytes.
enum ColumnData {
    /// Backs [`ElementKind::Vec3`] and [`ElementKind::F32`].
    F32(Vec<f32>),
    /// Backs [`ElementKind::U32`].
    U32(Vec<u32>),
}

/// One named column: its element kind, its storage, and the value a new slot
/// is filled with — grown and cleared as a unit alongside every other column
/// in [`Engine::columns`].
struct Column {
    kind: ElementKind,
    data: ColumnData,
    /// What [`Column::grow_to_fit`] fills a newly-covered slot with.
    ///
    /// Zero for `position`, `velocity` and anything `ensure_column` creates —
    /// the ordinary zero value a `Vec::resize` would give anyway. `colour` is
    /// the one column where zero is wrong: it is multiplied into the shader's
    /// vertex colour, so a zero-filled ("black") default would make every
    /// entity nobody has coloured invisible rather than merely white. See
    /// [`WHITE_FILL`].
    ///
    /// Applied to `u32`-backed columns by truncation too, so the field stays
    /// one `f32` rather than a kind-dependent enum — nothing today needs a
    /// non-zero `u32` default, and the day something does, `0.0` truncates to
    /// `0` exactly.
    fill: f32,
}

impl Column {
    /// An empty column of `kind` whose new slots are filled with `fill` on
    /// growth. `0.0` for the ordinary case — `Engine::create_column` always
    /// passes that — and [`WHITE_FILL`] for `colour`. The caller grows it to
    /// capacity; `Engine::create_column_with_fill` does, immediately.
    fn with_fill(kind: ElementKind, fill: f32) -> Self {
        let data = match kind {
            ElementKind::Vec3 | ElementKind::F32 => ColumnData::F32(Vec::new()),
            ElementKind::U32 => ColumnData::U32(Vec::new()),
        };
        Self { kind, data, fill }
    }

    /// Resizes the column to `capacity` slots, filling any growth with
    /// [`Column::fill`].
    ///
    /// Called for every column together from [`Engine::grow_columns_to_fit`].
    /// A column created after entities already exist starts this call behind
    /// the others and catches up to the same slot count in one step, the same
    /// way `position` and `velocity` always have.
    fn grow_to_fit(&mut self, capacity: usize) {
        let elements = capacity * self.kind.components();
        let fill = self.fill;
        match &mut self.data {
            ColumnData::F32(v) => v.resize(elements, fill),
            ColumnData::U32(v) => v.resize(elements, fill as u32),
        }
    }

    /// Empties the column entirely — to no slots, not merely to zeroed ones.
    fn clear(&mut self) {
        match &mut self.data {
            ColumnData::F32(v) => v.clear(),
            ColumnData::U32(v) => v.clear(),
        }
    }

    /// The address of the column's storage in wasm linear memory.
    fn ptr(&self) -> usize {
        match &self.data {
            ColumnData::F32(v) => v.as_ptr() as usize,
            ColumnData::U32(v) => v.as_ptr() as usize,
        }
    }

    /// The column's length in elements — `f32`s or `u32`s, per `kind` — not
    /// in slots. A [`ElementKind::Vec3`] column over 8 slots reports 24, the
    /// same number `Engine::positions_len` always has.
    fn len(&self) -> usize {
        match &self.data {
            ColumnData::F32(v) => v.len(),
            ColumnData::U32(v) => v.len(),
        }
    }

    /// The column as `f32`s.
    ///
    /// Panics on a `u32`-backed column — an internal invariant bug, not a
    /// condition a caller can trigger: `ensure_column` refuses to change an
    /// existing column's kind, so nothing can turn `position` or `velocity`
    /// into a `u32` column out from under the code that assumes them `f32`.
    fn as_f32(&self) -> &[f32] {
        match &self.data {
            ColumnData::F32(v) => v,
            ColumnData::U32(_) => unreachable!("expected an f32-backed column"),
        }
    }

    /// The mutable counterpart to [`Column::as_f32`].
    fn as_f32_mut(&mut self) -> &mut [f32] {
        match &mut self.data {
            ColumnData::F32(v) => v,
            ColumnData::U32(_) => unreachable!("expected an f32-backed column"),
        }
    }

    /// The column as `u32`s. Panics on an `f32`-backed column — see
    /// [`Column::as_f32`].
    ///
    /// `cfg(test)`-only: nothing in the engine itself reads a `u32` column's
    /// contents from Rust — the whole point of the boundary is that
    /// TypeScript reads and writes them directly through the pointer. This
    /// exists so the host tests can assert on a `u32` column without one.
    #[cfg(test)]
    fn as_u32(&self) -> &[u32] {
        match &self.data {
            ColumnData::U32(v) => v,
            ColumnData::F32(_) => unreachable!("expected a u32-backed column"),
        }
    }

    /// The mutable counterpart to [`Column::as_u32`].
    #[cfg(test)]
    fn as_u32_mut(&mut self) -> &mut [u32] {
        match &mut self.data {
            ColumnData::U32(v) => v,
            ColumnData::F32(_) => unreachable!("expected a u32-backed column"),
        }
    }
}

/// Two distinct named columns' `f32` storage, both mutable at once.
///
/// A free function taking `&mut self.columns` rather than a method on
/// [`Engine`], so the borrow checker sees it touches only that field — the
/// same call in [`Engine::tick`] also reads `self.store` to know which slots
/// are live, and a method taking `&mut self` would make that a conflicting
/// borrow.
fn two_f32_mut<'a>(
    columns: &'a mut [(String, Column)],
    a: &str,
    b: &str,
) -> (&'a mut [f32], &'a mut [f32]) {
    let ia = columns
        .iter()
        .position(|(name, _)| name == a)
        .unwrap_or_else(|| panic!("column \"{a}\" does not exist"));
    let ib = columns
        .iter()
        .position(|(name, _)| name == b)
        .unwrap_or_else(|| panic!("column \"{b}\" does not exist"));
    assert_ne!(ia, ib, "two_f32_mut needs two distinct columns");
    // `split_at_mut` needs the lower index first; `a` and `b` may name either
    // column in either order, so split on whichever index is greater and hand
    // each half back in the order the caller asked for.
    if ia < ib {
        let (left, right) = columns.split_at_mut(ib);
        (left[ia].1.as_f32_mut(), right[0].1.as_f32_mut())
    } else {
        let (left, right) = columns.split_at_mut(ia);
        (right[0].1.as_f32_mut(), left[ib].1.as_f32_mut())
    }
}

/// One running engine.
///
/// Owns the world and the named columns TypeScript reads. Held by the shell
/// for the lifetime of the page.
#[wasm_bindgen]
pub struct Engine {
    store: Store,
    /// Named columns, scanned linearly rather than hashed: there are a
    /// handful, a lookup happens once per phase — never per entity, see
    /// `docs/boundary.md` — and a `Vec` keeps iteration order deterministic,
    /// which a `HashMap` would not. `position` and `velocity` are ordinary
    /// entries here, created in [`Engine::new`] through the same path
    /// [`Engine::ensure_column`] uses.
    columns: Vec<(String, Column)>,
    viewport: Viewport,
    frame_count: u32,
    elapsed: f32,
    /// The backend, once a canvas has been attached. `None` in the host tests,
    /// which have no canvas and want none — every method below works without
    /// it except [`Engine::render`].
    renderer: Option<Renderer>,
    /// The one pipeline, compiled when the renderer is attached.
    triangle: Option<PipelineId>,
    /// How many draws the last presented frame carried.
    ///
    /// What was *submitted*, not what the next frame would submit. The
    /// difference is the whole value of the number: a board whose paint lane
    /// has been cut still has eight entities the next frame *could* draw, and
    /// a readout that reported the prediction would say "8 draws" over a black
    /// screen. An instrument that reports what might happen is not measuring.
    last_draws: u32,
    /// How large a triangle draws, in world units.
    ///
    /// A setting rather than a constant because a board sets it: a node whose
    /// `scale` param the engine ignored would be a control that looks live and
    /// changes nothing, which teaches exactly the wrong thing about what a
    /// board is.
    scale: f32,
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
        let mut engine = Self {
            store: Store::new(),
            columns: Vec::new(),
            viewport: Viewport::new(width, height).fit_within(krudd_webgl::MAX_SURFACE_EXTENT),
            frame_count: 0,
            elapsed: 0.0,
            renderer: None,
            triangle: None,
            last_draws: 0,
            scale: TRIANGLE_SCALE,
        };
        // `position` and `velocity` are ordinary named columns, created
        // through the same path `ensure_column` uses — there is no bespoke
        // field left for either of them. `colour` joins them the same way,
        // differing only in its fill: see `WHITE_FILL`.
        engine.create_column(POSITION, ElementKind::Vec3);
        engine.create_column(VELOCITY, ElementKind::Vec3);
        engine.create_column_with_fill(COLOUR, ElementKind::Vec3, WHITE_FILL);
        engine
    }

    /// Sets how large one entity's triangle draws, in world units.
    ///
    /// One call for the whole world, like every other phase call — the scale
    /// goes into the transform of every draw the next frame builds.
    pub fn set_scale(&mut self, scale: f32) {
        self.scale = scale;
    }

    /// How large one entity's triangle draws.
    pub fn scale(&self) -> f32 {
        self.scale
    }

    /// Presents a frame with nothing in it.
    ///
    /// The counterpart to [`Engine::render`], and the reason it exists: a board
    /// whose paint lane runs nothing has to leave the screen showing *nothing*,
    /// not the last frame it happened to draw. A canvas still holding a picture
    /// after the wire that drew it was cut is the exact mixed signal
    /// `CODING_STANDARD.md` forbids — it says "working" and "did nothing" the
    /// same way.
    pub fn present_cleared(&mut self) -> Result<(), JsValue> {
        let mut frame = Frame::new(self.viewport);
        frame.view_projection = self.view_projection();
        self.present(&frame)
            .map_err(|why| JsValue::from_str(&why.to_string()))
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
        let frame = self.build_frame();
        self.present(&frame)
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

    /// How many draws the last presented frame carried.
    ///
    /// Zero before the first frame, and zero after a frame presented with
    /// nothing in it. For the page's debug readout — and it is a measurement
    /// rather than a prediction, so a cut wire reads as `0 draws` beside a
    /// black canvas instead of as the eight the world still holds.
    pub fn draw_count(&self) -> u32 {
        self.last_draws
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
        let slot = handle.index() as usize * 3;
        let position = self
            .column_mut(POSITION)
            .expect("created in `Engine::new`")
            .as_f32_mut();
        position[slot] = x;
        position[slot + 1] = y;
        position[slot + 2] = z;
        let velocity = self
            .column_mut(VELOCITY)
            .expect("created in `Engine::new`")
            .as_f32_mut();
        velocity[slot] = 0.0;
        velocity[slot + 1] = 0.0;
        velocity[slot + 2] = 0.0;
        handle.index()
    }

    /// Empties the world: no entities, no slots, every column back to
    /// nothing.
    ///
    /// What opening a different project needs, and the reason it is one call
    /// rather than a `despawn` per slot. A freed slot is a tombstone, and a
    /// tombstone still occupies its index — so a world emptied one entity at a
    /// time still reports the slot count it had, and anything that sizes
    /// itself from that count would build the new scene on top of the old
    /// one's shape. This is also the batched free `docs/boundary.md` asks for
    /// on principle: one crossing for the whole world rather than one per
    /// entity.
    ///
    /// The columns are emptied rather than merely resized, so a typed-array
    /// view taken beforehand goes stale exactly as it does across a spawn —
    /// same rule, same three ways to notice.
    pub fn clear(&mut self) {
        self.store.clear();
        for (_, column) in &mut self.columns {
            column.clear();
        }
        // Not the frame count, the clock or the renderer: this empties the
        // world, and an engine that also forgot how long it had been running
        // would be reporting a page that had just booted when it had not.
        self.last_draws = 0;
    }

    /// Sets an entity's velocity, in units per second.
    ///
    /// Returns whether the slot was live.
    pub fn set_velocity(&mut self, slot: u32, x: f32, y: f32, z: f32) -> bool {
        match self.live_handle(slot) {
            Some(handle) => {
                let i = handle.index() as usize * 3;
                let velocity = self
                    .column_mut(VELOCITY)
                    .expect("created in `Engine::new`")
                    .as_f32_mut();
                velocity[i] = x;
                velocity[i + 1] = y;
                velocity[i + 2] = z;
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
        // One name lookup for each column, not one per entity — see
        // `two_f32_mut`'s docs for why this is a free function rather than a
        // method, and `docs/boundary.md` for why the lookup count matters at
        // all.
        let (velocity, position) = two_f32_mut(&mut self.columns, VELOCITY, POSITION);
        for handle in self.store.iter() {
            let i = handle.index() as usize * 3;
            position[i] += velocity[i] * dt;
            position[i + 1] += velocity[i + 1] * dt;
            position[i + 2] += velocity[i + 2] * dt;
        }
        self.elapsed += dt;
        self.frame_count += 1;
    }

    /// Creates a named column if it does not already exist, sized to the
    /// current slot capacity.
    ///
    /// `kind` is `"vec3"`, `"f32"` or `"u32"`. Creating a column that already
    /// exists at the same kind is a no-op — a board re-declaring a column it
    /// already ensured must not lose what is in it. Creating one that already
    /// exists at a **different** kind is refused: silently reinterpreting the
    /// bytes of an existing column would hand back garbage of the wrong
    /// width rather than fail where the mistake was made.
    pub fn ensure_column(&mut self, name: &str, kind: &str) -> Result<(), JsValue> {
        self.ensure_column_checked(name, kind)
            .map_err(|why| JsValue::from_str(&why.to_string()))
    }

    /// The address of a named column's storage in wasm linear memory, or
    /// `None` if no column of that name exists.
    ///
    /// Paired with [`Engine::column_len`] to build a typed-array view. Goes
    /// stale the same three ways every column does — see `docs/boundary.md`.
    pub fn column_ptr(&self, name: &str) -> Option<usize> {
        self.column(name).map(Column::ptr)
    }

    /// A named column's length in elements — `f32`s or `u32`s, not slots — or
    /// `None` if no column of that name exists.
    ///
    /// A [`ElementKind::Vec3`] column over 8 slots reports 24, matching what
    /// [`Engine::positions_len`] means today.
    pub fn column_len(&self, name: &str) -> Option<usize> {
        self.column(name).map(Column::len)
    }

    /// Whether a named column is `u32`-backed, or `None` if it does not
    /// exist.
    ///
    /// So the TypeScript side can pick `Uint32Array` over `Float32Array` for
    /// the view without guessing from the name.
    pub fn column_is_ints(&self, name: &str) -> Option<bool> {
        self.column(name).map(|column| column.kind.is_ints())
    }

    /// The address of the position column in wasm linear memory.
    ///
    /// A thin wrapper over [`Engine::column_ptr`] — kept as its own export
    /// because `docs/boundary.md`, `cargo xtask bench` and the harnesses all
    /// name it, and rewriting every caller to the generic lookup is out of
    /// scope for the column this method now delegates to.
    pub fn positions_ptr(&self) -> usize {
        self.column_ptr(POSITION).expect("created in `Engine::new`")
    }

    /// The length of the position column, in `f32`s — three per slot.
    pub fn positions_len(&self) -> usize {
        self.column_len(POSITION).expect("created in `Engine::new`")
    }

    /// The address of the velocity column in wasm linear memory.
    ///
    /// A thin wrapper over [`Engine::column_ptr`] — see [`Engine::positions_ptr`]
    /// for why it stays exported under its own name.
    pub fn velocities_ptr(&self) -> usize {
        self.column_ptr(VELOCITY).expect("created in `Engine::new`")
    }

    /// The length of the velocity column, in `f32`s — three per slot.
    ///
    /// Always equal to [`Engine::positions_len`]: the two columns are indexed
    /// by the same slot and grown together.
    pub fn velocities_len(&self) -> usize {
        self.column_len(VELOCITY).expect("created in `Engine::new`")
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

    /// World-space `x` for a normalised screen-space `x` in `[0, 1]`, 0 at the
    /// viewport's left edge.
    ///
    /// Reads the scale and offset back out of [`Engine::view_projection`] —
    /// the same matrix [`Engine::build_frame`] submits — rather than
    /// recomputing `VIEW_EXTENT` and the viewport aspect independently. A
    /// pick built from a second copy of the camera arithmetic is a pick that
    /// can disagree with the draw at some aspect ratio, and the person who
    /// finds that is tapping the screen, not running a test.
    ///
    /// This is one scalar call per axis rather than one call returning a
    /// `[f32; 2]` pair: the latter is a `Box<[f32]>`, which allocates on both
    /// sides of the boundary on every call — fine once at boot, not for
    /// something a pointer move can trigger every frame. See
    /// `docs/boundary.md`'s "what may be exported".
    ///
    /// It decomposes into independent per-axis calls **only because the
    /// camera is orthographic and axis-aligned**: clip-space `x` here depends
    /// on world `x` alone, with no `y` or `z` term to fold in. A camera that
    /// rotated, or a perspective projection, would couple the axes through
    /// the same row of the matrix, and unprojecting would need a matrix
    /// inverse behind one call that returns both coordinates together —
    /// not two calls that each pretend the other axis does not exist.
    pub fn world_x_from_screen(&self, x: f32) -> f32 {
        let clip_x = x * 2.0 - 1.0;
        let m = self.view_projection();
        (clip_x - m.cols[3][0]) / m.cols[0][0]
    }

    /// World-space `y` for a normalised screen-space `y` in `[0, 1]`, 0 at the
    /// viewport's top edge.
    ///
    /// **Screen `y` runs down; world `y` runs up.** `y = 0`, the top of the
    /// viewport, has to land at the *top* of the camera's box — clip-space
    /// `+1` — and `y = 1`, the bottom, at clip-space `-1`. So the map from
    /// screen to clip is `1.0 - 2.0 * y`, not `2.0 * y - 1.0`: the sign flip
    /// is not a typo carried over from [`Engine::world_x_from_screen`], it is
    /// the one place this function has to differ from it. Getting it backwards
    /// would still compile, still return a plausible-looking number, and place
    /// every tap on the wrong row.
    ///
    /// See [`Engine::world_x_from_screen`] for why this is a scalar call
    /// rather than a heap-returning pair, and why the split into two calls
    /// only holds together for an orthographic, axis-aligned camera.
    pub fn world_y_from_screen(&self, y: f32) -> f32 {
        let clip_y = 1.0 - y * 2.0;
        let m = self.view_projection();
        (clip_y - m.cols[3][1]) / m.cols[1][1]
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
        let i = handle.index() as usize * 3;
        let position = self
            .column(POSITION)
            .expect("created in `Engine::new`")
            .as_f32();
        Some(Box::new([position[i], position[i + 1], position[i + 2]]))
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
                let i = handle.index() as usize * 3;
                let position = self
                    .column_mut(POSITION)
                    .expect("created in `Engine::new`")
                    .as_f32_mut();
                position[i] = x;
                position[i + 1] = y;
                position[i + 2] = z;
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

    /// Puts one frame on the screen, without the `JsValue`.
    ///
    /// The exported methods are one-line wrappers around this so that the
    /// decision of what is worth reporting is testable: `JsValue::from_str`
    /// panics off wasm, so anything that touches it can only be exercised in a
    /// browser. It is also the one place that knows an empty frame is
    /// presented exactly like a full one — the difference between them is what
    /// is in the draw list, and nothing else.
    fn present(&mut self, frame: &Frame) -> Result<(), RenderError> {
        let submitted = frame.draws().len() as u32;
        let renderer = self.renderer.as_mut().ok_or(RenderError::Detached)?;
        self.last_draws = submitted;
        match draw(renderer, frame) {
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
        let scale = Mat4::from_scale(Vec3::new(self.scale, self.scale, 1.0));
        let position = self
            .column(POSITION)
            .expect("created in `Engine::new`")
            .as_f32();
        let colour = self
            .column(COLOUR)
            .expect("created in `Engine::new`")
            .as_f32();
        for handle in self.store.iter() {
            let i = handle.index() as usize * 3;
            let p = Vec3::new(position[i], position[i + 1], position[i + 2]);
            let c = Vec3::new(colour[i], colour[i + 1], colour[i + 2]);
            frame.push(Draw {
                pipeline,
                transform: Mat4::from_translation(p).mul(&scale),
                colour: c,
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

    /// Extends every column to cover every slot the store can hand out.
    ///
    /// Called after every alloc rather than sized up front: the store reuses
    /// tombstones, so the columns only grow when it has genuinely run out of
    /// slots to recycle. Every column grows together, whatever its name or
    /// kind, so a column created later still lines up with `position` and
    /// `velocity` by slot index.
    fn grow_columns_to_fit(&mut self) {
        let capacity = self.store.capacity();
        for (_, column) in &mut self.columns {
            column.grow_to_fit(capacity);
        }
    }

    /// A named column's index into [`Engine::columns`], or `None` if no
    /// column of that name exists.
    ///
    /// Linear, not hashed — see the field's own doc comment for why that is
    /// the right trade here.
    fn column_index(&self, name: &str) -> Option<usize> {
        self.columns.iter().position(|(n, _)| n == name)
    }

    /// A named column, or `None` if it does not exist.
    fn column(&self, name: &str) -> Option<&Column> {
        self.column_index(name).map(|i| &self.columns[i].1)
    }

    /// The mutable counterpart to [`Engine::column`].
    fn column_mut(&mut self, name: &str) -> Option<&mut Column> {
        let index = self.column_index(name)?;
        Some(&mut self.columns[index].1)
    }

    /// Appends a new column, sized to the current slot capacity and
    /// zero-filled on growth.
    ///
    /// The common path — [`Engine::ensure_column`] uses it for every column a
    /// board declares, and [`Engine::new`] uses it for `position` and
    /// `velocity`, both of which want the ordinary zero. The `colour` column
    /// wants white instead, so [`Engine::new`] creates it through
    /// [`Engine::create_column_with_fill`] instead of this one.
    fn create_column(&mut self, name: &str, kind: ElementKind) {
        self.create_column_with_fill(name, kind, 0.0);
    }

    /// [`Engine::create_column`], but new slots are filled with `fill` rather
    /// than zero — the one path that creates a column, so there is one place
    /// that has to remember to size a fresh column to what the others already
    /// hold.
    fn create_column_with_fill(&mut self, name: &str, kind: ElementKind, fill: f32) {
        let mut column = Column::with_fill(kind, fill);
        column.grow_to_fit(self.store.capacity());
        self.columns.push((name.to_string(), column));
    }

    /// [`Engine::ensure_column`]'s logic, without the `JsValue` at the edge —
    /// see [`ElementKind::parse`] for why that split exists. This is what the
    /// host tests call directly, so the error path is exercised at all.
    fn ensure_column_checked(&mut self, name: &str, kind: &str) -> Result<(), ColumnError> {
        let kind = ElementKind::parse(kind)?;
        match self.column_index(name) {
            Some(index) => {
                let existing = self.columns[index].1.kind;
                if existing == kind {
                    Ok(())
                } else {
                    Err(ColumnError::KindConflict {
                        name: name.to_string(),
                        existing,
                        requested: kind,
                    })
                }
            }
            None => {
                self.create_column(name, kind);
                Ok(())
            }
        }
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

    /// Reads one slot out of the position column as a [`Vec3`], the way the
    /// tests used to read the old `positions: Vec<Vec3>` field directly.
    fn position_at(e: &Engine, slot: u32) -> Vec3 {
        let i = slot as usize * 3;
        let p = e.column(POSITION).unwrap().as_f32();
        Vec3::new(p[i], p[i + 1], p[i + 2])
    }

    /// The velocity counterpart to [`position_at`].
    fn velocity_at(e: &Engine, slot: u32) -> Vec3 {
        let i = slot as usize * 3;
        let v = e.column(VELOCITY).unwrap().as_f32();
        Vec3::new(v[i], v[i + 1], v[i + 2])
    }

    /// Writes one slot of the velocity column directly, the way a page fills
    /// it through the batched view rather than through `set_velocity`.
    fn write_velocity(e: &mut Engine, slot: u32, value: Vec3) {
        let i = slot as usize * 3;
        let v = e.column_mut(VELOCITY).unwrap().as_f32_mut();
        v[i] = value.x;
        v[i + 1] = value.y;
        v[i + 2] = value.z;
    }

    /// The colour counterpart to [`position_at`].
    fn colour_at(e: &Engine, slot: u32) -> Vec3 {
        let i = slot as usize * 3;
        let c = e.column(COLOUR).unwrap().as_f32();
        Vec3::new(c[i], c[i + 1], c[i + 2])
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
        assert_eq!(position_at(&e, 0), Vec3::new(1.0, 2.0, 3.0));
    }

    #[test]
    fn a_fresh_colour_column_defaults_to_white_not_black() {
        // The fragment shader multiplies its vertex colour by this column, so
        // the zero value every other column grows into would render an
        // uncoloured entity invisible rather than merely uncoloured. Assert
        // the actual values rather than trusting the constant's name.
        let mut e = Engine::default();
        let a = e.spawn(0.0, 0.0, 0.0);
        assert_eq!(colour_at(&e, a), Vec3::new(1.0, 1.0, 1.0));
    }

    #[test]
    fn a_spawned_entitys_colour_survives_a_column_growth() {
        // `grow_columns_to_fit` extends every column together whenever the
        // store runs out of slots to recycle. If the colour column's fill
        // were applied only at creation rather than on every growth, an
        // entity spawned before the column last grew would end up reading
        // whatever the *next* growth happened to zero-fill, not white.
        let mut e = Engine::default();
        let first = e.spawn(0.0, 0.0, 0.0);
        assert_eq!(colour_at(&e, first), Vec3::new(1.0, 1.0, 1.0));

        for i in 1..16 {
            e.spawn(i as f32, 0.0, 0.0);
        }

        assert_eq!(
            colour_at(&e, first),
            Vec3::new(1.0, 1.0, 1.0),
            "the first entity's colour must not have been disturbed by later growth"
        );
    }

    #[test]
    fn tick_integrates_velocity_for_every_live_entity() {
        let mut e = Engine::default();
        let a = e.spawn(0.0, 0.0, 0.0);
        let b = e.spawn(10.0, 0.0, 0.0);
        assert!(e.set_velocity(a, 1.0, 0.0, 0.0));
        assert!(e.set_velocity(b, -1.0, 0.0, 0.0));

        e.tick(0.5);

        assert_eq!(position_at(&e, a).x, 0.5);
        assert_eq!(position_at(&e, b).x, 9.5);
        assert_eq!(e.frame_count(), 1);
        assert_eq!(e.elapsed(), 0.5);
    }

    #[test]
    fn the_two_columns_are_indexed_alike_and_grow_together() {
        // The page indexes both with `slot * 3`, so a slot that exists in one
        // column and not the other reads another entity's velocity rather than
        // failing.
        let mut e = Engine::default();
        assert_eq!(e.velocities_len(), 0);
        e.spawn(0.0, 0.0, 0.0);
        e.spawn(1.0, 0.0, 0.0);
        assert_eq!(e.velocities_len(), e.positions_len());
        assert_eq!(e.velocities_len(), 6);
    }

    #[test]
    fn a_write_into_the_velocity_column_is_what_tick_integrates() {
        // The batched write path: the page fills the column through a view and
        // never calls `set_velocity`. If `tick` read from anywhere else, a
        // world driven the batched way would sit still.
        let mut e = Engine::default();
        let a = e.spawn(0.0, 0.0, 0.0);
        write_velocity(&mut e, a, Vec3::new(2.0, -3.0, 0.0));

        e.tick(0.5);

        assert_eq!(position_at(&e, a), Vec3::new(1.0, -1.5, 0.0));
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
        assert_eq!(position_at(&e, a).x, 0.0);
    }

    #[test]
    fn clearing_empties_the_world_and_every_column() {
        let mut e = Engine::default();
        for _ in 0..8 {
            let slot = e.spawn(1.0, 2.0, 3.0);
            e.set_velocity(slot, 1.0, 0.0, 0.0);
        }

        e.clear();

        assert_eq!(e.entity_count(), 0);
        // The difference from despawning each one: a tombstone keeps its
        // index, so a world emptied slot by slot still reports the width it
        // had. Opening another project must not inherit that width.
        assert_eq!(e.slot_count(), 0);
        assert_eq!(e.positions_len(), 0);
        assert_eq!(e.velocities_len(), 0);
        // `colour` is emptied the same way as `position` and `velocity` — a
        // column that stayed sized after `clear` would answer with the last
        // project's colours the moment the next one spawned into it.
        assert_eq!(e.column_len(COLOUR), Some(0));
        assert_eq!(e.draw_count(), 0);
    }

    #[test]
    fn a_world_spawned_after_a_clear_is_the_one_a_fresh_engine_builds() {
        // What opening a project has to be worth: the same board, spawned into
        // a used engine, is the board a fresh boot would have built — same
        // slots, same order, same columns.
        let mut used = Engine::default();
        for _ in 0..3 {
            used.spawn(9.0, 9.0, 9.0);
        }
        used.clear();

        let mut fresh = Engine::default();
        let mut used_slots = Vec::new();
        let mut fresh_slots = Vec::new();
        for i in 0..3 {
            let x = i as f32;
            used_slots.push(used.spawn(x, 0.0, 0.0));
            fresh_slots.push(fresh.spawn(x, 0.0, 0.0));
        }

        assert_eq!(used_slots, fresh_slots);
        assert_eq!(
            used.column(POSITION).unwrap().as_f32(),
            fresh.column(POSITION).unwrap().as_f32()
        );
        assert_eq!(
            used.column(VELOCITY).unwrap().as_f32(),
            fresh.column(VELOCITY).unwrap().as_f32()
        );
        assert_eq!(used.slot_count(), fresh.slot_count());
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
            position_at(&e, b),
            Vec3::ZERO,
            "the recycled slot kept the old entity's velocity"
        );
        assert_eq!(
            velocity_at(&e, b),
            Vec3::ZERO,
            "the recycled slot's velocity should have been reset by `spawn`, not inherited"
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
        assert_eq!(position_at(&e, a), Vec3::new(4.0, 5.0, 6.0));
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
    fn the_draw_count_is_what_was_presented_not_what_could_be() {
        // A board whose paint lane has been cut still has eight entities the
        // next frame *could* draw. A readout that reported the prediction would
        // say "8 draws" over a black screen, which is an instrument saying
        // "working" and "did nothing" the same way.
        let mut e = Engine::new(100, 100);
        e.triangle = Some(fake_pipeline());
        e.spawn(0.0, 0.0, 0.0);

        assert_eq!(e.build_frame().draws().len(), 1, "one *could* be drawn");
        assert_eq!(
            e.draw_count(),
            0,
            "but nothing has been presented, so nothing has been drawn"
        );

        // And a frame that never reached a surface has not been presented
        // either — this engine has no renderer, so it cannot have drawn
        // anything however many entities it holds.
        let frame = e.build_frame();
        // The internal form, not `present_cleared` — the exported one maps its
        // error through `JsValue::from_str`, which panics off wasm.
        assert!(e.present(&frame).is_err());
        assert_eq!(e.draw_count(), 0);

        // What the browser proves, and this cannot: after a real present the
        // count is what was submitted, and after a cleared one it is zero.
        // `cargo xtask render-test` is where that is held.
    }

    #[test]
    fn every_live_entity_becomes_one_triangle() {
        let mut e = Engine::new(100, 100);
        e.triangle = Some(fake_pipeline());
        e.spawn(0.0, 0.0, 0.0);
        let b = e.spawn(1.0, 0.0, 0.0);
        e.spawn(2.0, 0.0, 0.0);
        assert_eq!(e.build_frame().draws().len(), 3);

        assert!(e.despawn(b));
        assert_eq!(
            e.build_frame().draws().len(),
            2,
            "a tombstoned slot is not drawn"
        );

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
    fn a_draws_colour_is_the_entitys_colour_column_not_a_constant() {
        // `build_frame` has to read the column per entity, not stamp every
        // draw with the same value — spawning two entities and giving only
        // one of them a colour is what would catch a build that read slot 0
        // for every draw.
        let mut e = Engine::new(100, 100);
        e.triangle = Some(fake_pipeline());
        let a = e.spawn(0.0, 0.0, 0.0);
        let b = e.spawn(1.0, 0.0, 0.0);
        {
            let colour = e.column_mut(COLOUR).unwrap().as_f32_mut();
            let i = b as usize * 3;
            colour[i] = 1.0;
            colour[i + 1] = 0.0;
            colour[i + 2] = 0.0;
        }

        let frame = e.build_frame();
        assert_eq!(
            colour_at(&e, a),
            Vec3::new(1.0, 1.0, 1.0),
            "still the default"
        );
        assert_eq!(frame.draws()[a as usize].colour, Vec3::new(1.0, 1.0, 1.0));
        assert_eq!(frame.draws()[b as usize].colour, Vec3::new(1.0, 0.0, 0.0));
    }

    #[test]
    fn the_scale_a_board_sets_is_the_scale_a_draw_carries() {
        // A `scale` param the engine ignored would be a control that looks live
        // and changes nothing.
        let mut e = Engine::new(100, 100);
        e.triangle = Some(fake_pipeline());
        e.spawn(0.0, 0.0, 0.0);
        assert_eq!(
            e.scale(),
            TRIANGLE_SCALE,
            "the default is what it always was"
        );

        e.set_scale(1.5);
        let transform = e.build_frame().draws()[0].transform;
        assert_eq!(transform.cols[0][0], 1.5);
        assert_eq!(transform.cols[1][1], 1.5);
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

    /// Projects a world point through `m` by hand — the same four-term dot
    /// product [`Mat4::orthographic`]'s own test uses — so the round-trip
    /// tests below check the real matrix rather than the shortcut formula
    /// [`Engine::world_x_from_screen`] and [`Engine::world_y_from_screen`]
    /// happen to use internally.
    fn project(m: &Mat4, p: [f32; 4]) -> [f32; 4] {
        let mut out = [0.0f32; 4];
        for (r, cell) in out.iter_mut().enumerate() {
            *cell = (0..4).map(|k| m.cols[k][r] * p[k]).sum();
        }
        out
    }

    /// Round-trips a handful of world points through the real camera matrix
    /// and back, at a given viewport, and asserts each lands where it started.
    ///
    /// This is the test that matters for `world_x_from_screen` and
    /// `world_y_from_screen`: the two are only safe to keep separate from
    /// `build_frame`'s own camera because something proves they read the
    /// *same* matrix, every time this file changes. Without it, the pick and
    /// the draw could drift apart silently at some aspect ratio, found by a
    /// person tapping the screen rather than by CI.
    fn assert_round_trips(e: &Engine, points: &[(f32, f32)]) {
        let m = e.view_projection();
        for &(wx, wy) in points {
            let clip = project(&m, [wx, wy, 0.0, 1.0]);
            // The inverse of the maps `world_x_from_screen`/`world_y_from_screen`
            // apply going in: clip -1..1 back to screen 0..1, with the y flip
            // undone the same way it was applied.
            let screen_x = (clip[0] + 1.0) / 2.0;
            let screen_y = (1.0 - clip[1]) / 2.0;
            let got_x = e.world_x_from_screen(screen_x);
            let got_y = e.world_y_from_screen(screen_y);
            assert!(
                (got_x - wx).abs() < 1e-4,
                "x round-trip: {wx} -> screen {screen_x} -> {got_x}"
            );
            assert!(
                (got_y - wy).abs() < 1e-4,
                "y round-trip: {wy} -> screen {screen_y} -> {got_y}"
            );
        }
    }

    /// The points every round-trip test checks: the origin, all four corners
    /// of the camera's box at this viewport, and one point off both axes so a
    /// bug that only shows up away from the centreline is not missed.
    fn probe_points(e: &Engine) -> Vec<(f32, f32)> {
        let m = e.view_projection();
        // Recover the box's half-extents from the matrix rather than
        // hardcoding `VIEW_EXTENT`, so this stays correct if that constant
        // ever moves.
        let half_width = 1.0 / m.cols[0][0];
        let half_height = 1.0 / m.cols[1][1];
        vec![
            (0.0, 0.0),
            (-half_width, half_height),
            (half_width, half_height),
            (-half_width, -half_height),
            (half_width, -half_height),
            (half_width * 0.37, -half_height * 0.71),
        ]
    }

    #[test]
    fn world_from_screen_round_trips_through_the_real_camera_at_a_square_viewport() {
        let e = Engine::new(400, 400);
        let points = probe_points(&e);
        assert_round_trips(&e, &points);
    }

    #[test]
    fn world_from_screen_round_trips_through_the_real_camera_at_a_phone_shaped_viewport() {
        // The #853 case: a portrait phone, much taller than it is wide. The
        // widened-box camera is at its most anisotropic here, which is where a
        // formula that quietly assumed a square viewport would show it first.
        let e = Engine::new(1080, 2256);
        let points = probe_points(&e);
        assert_round_trips(&e, &points);
    }

    #[test]
    fn screen_top_left_is_world_top_left_not_bottom_left() {
        // The likeliest bug this pair of functions has: screen y runs down,
        // world y runs up, and a naive port of the x formula would flip only
        // one of the two axes it needed to.
        let e = Engine::new(400, 400);
        let half_height = VIEW_EXTENT;
        let half_width = e.view_projection().cols[0][0].recip();

        assert!(
            (e.world_x_from_screen(0.0) - -half_width).abs() < 1e-4,
            "screen x=0 is the left edge"
        );
        assert!(
            (e.world_y_from_screen(0.0) - half_height).abs() < 1e-4,
            "screen y=0 (the top of the viewport) must be the *top* of the \
             world box, not the bottom"
        );
        assert!(
            (e.world_y_from_screen(1.0) - -half_height).abs() < 1e-4,
            "screen y=1 (the bottom of the viewport) is the bottom of the box"
        );
    }

    #[test]
    fn world_from_screen_matches_a_hand_worked_conversion_at_a_wide_viewport() {
        // A worked example independent of `project`/`probe_points`, so a bug
        // shared between the implementation and the round-trip helper above
        // would not sail through both. `800x400` widens the box 2:1, so the
        // left edge is world x = -4 rather than -2.
        let e = Engine::new(800, 400);
        assert!((e.world_x_from_screen(0.0) - -4.0).abs() < 1e-4);
        assert!((e.world_x_from_screen(1.0) - 4.0).abs() < 1e-4);
        assert!((e.world_x_from_screen(0.5) - 0.0).abs() < 1e-4);
        assert!((e.world_y_from_screen(0.0) - VIEW_EXTENT).abs() < 1e-4);
        assert!((e.world_y_from_screen(1.0) - -VIEW_EXTENT).abs() < 1e-4);
    }

    #[test]
    fn rendering_without_a_canvas_is_an_error_rather_than_a_silent_no_op() {
        // The page turns this into text. A `render` that quietly did nothing
        // would leave a blank canvas and no explanation, which is the one thing
        // #812 says an instrument may never do.
        let mut e = Engine::default();
        let frame = e.build_frame();
        let why = e
            .present(&frame)
            .expect_err("a detached engine cannot draw");
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

    #[test]
    fn a_u32_and_an_f32_column_grow_alongside_position_and_hold_their_own_values() {
        let mut e = Engine::default();
        e.ensure_column("tag", "u32").unwrap();
        e.ensure_column("hp", "f32").unwrap();

        let a = e.spawn(0.0, 0.0, 0.0);
        let b = e.spawn(1.0, 0.0, 0.0);

        // One element per slot each, grown in lockstep with `position` —
        // `positions_len` is three per slot, these are one.
        assert_eq!(e.column_len("tag"), Some(2));
        assert_eq!(e.column_len("hp"), Some(2));
        assert_eq!(e.positions_len(), 6);

        e.column_mut("tag").unwrap().as_u32_mut()[a as usize] = 7;
        e.column_mut("tag").unwrap().as_u32_mut()[b as usize] = 9;
        e.column_mut("hp").unwrap().as_f32_mut()[a as usize] = 10.0;
        e.column_mut("hp").unwrap().as_f32_mut()[b as usize] = 20.0;

        assert_eq!(e.column("tag").unwrap().as_u32(), &[7, 9]);
        assert_eq!(e.column("hp").unwrap().as_f32(), &[10.0, 20.0]);
        // Each column holds its own values — writing "tag" must not perturb
        // "hp", nor either of them the pre-existing `position` column.
        assert_eq!(e.column("hp").unwrap().as_f32()[a as usize], 10.0);
        assert_eq!(position_at(&e, a), Vec3::new(0.0, 0.0, 0.0));
    }

    #[test]
    fn ensuring_a_column_that_already_exists_at_the_same_kind_is_a_no_op() {
        let mut e = Engine::default();
        e.ensure_column("tag", "u32").unwrap();
        e.spawn(0.0, 0.0, 0.0);
        e.column_mut("tag").unwrap().as_u32_mut()[0] = 5;

        assert!(e.ensure_column("tag", "u32").is_ok());

        assert_eq!(
            e.column("tag").unwrap().as_u32()[0],
            5,
            "re-ensuring the same kind must not reset what the column holds"
        );
        assert_eq!(
            e.columns.len(),
            4,
            "position, velocity, colour, tag — re-ensuring must not append a duplicate entry"
        );
    }

    #[test]
    fn clearing_empties_every_column_not_just_position_and_velocity() {
        let mut e = Engine::default();
        e.ensure_column("tag", "u32").unwrap();
        e.spawn(0.0, 0.0, 0.0);
        e.spawn(1.0, 0.0, 0.0);

        e.clear();

        assert_eq!(e.column_len("tag"), Some(0));
        assert_eq!(e.positions_len(), 0);
        assert_eq!(e.velocities_len(), 0);
    }

    #[test]
    fn re_ensuring_a_column_with_a_conflicting_kind_fails_rather_than_reinterpreting() {
        let mut e = Engine::default();
        e.ensure_column("tag", "u32").unwrap();
        e.spawn(0.0, 0.0, 0.0);
        e.column_mut("tag").unwrap().as_u32_mut()[0] = 42;

        // The `_checked` form, not `ensure_column`: the latter maps its error
        // through `JsValue::from_str`, which panics off wasm — the same
        // reason the tests call `Engine::present` instead of `render`.
        let result = e.ensure_column_checked("tag", "f32");

        assert!(
            result.is_err(),
            "changing an existing column's kind must fail loudly, not reinterpret its bytes"
        );
        // And the failed attempt must not have touched the column at all.
        assert_eq!(e.column_is_ints("tag"), Some(true));
        assert_eq!(e.column("tag").unwrap().as_u32()[0], 42);
    }

    #[test]
    fn ensure_column_rejects_an_unknown_kind() {
        let mut e = Engine::default();
        assert!(e.ensure_column_checked("tag", "vec2").is_err());
        assert_eq!(
            e.column_index("tag"),
            None,
            "nothing should have been created"
        );
    }

    #[test]
    fn a_tombstoned_slot_keeps_its_width_in_every_column() {
        let mut e = Engine::default();
        e.ensure_column("tag", "u32").unwrap();
        let a = e.spawn(1.0, 2.0, 3.0);
        e.column_mut("tag").unwrap().as_u32_mut()[a as usize] = 99;

        assert!(e.despawn(a));

        // The slot is tombstoned, not removed — every column stays the width
        // it was, the same guarantee `position` and `velocity` already gave,
        // now extended to a column that did not exist when that guarantee was
        // first written.
        assert_eq!(e.column_len("tag"), Some(1));
        assert_eq!(e.positions_len(), 3);
        assert_eq!(e.velocities_len(), 3);
        assert_eq!(e.column("tag").unwrap().as_u32()[a as usize], 99);
    }

    #[test]
    fn a_tombstoned_slot_keeps_its_width_in_the_colour_column() {
        // Named separately from the generic column above: `colour` is the one
        // column with a non-zero fill, and a despawn must not shrink it out
        // from under an index another live entity still uses — the same
        // tombstone guarantee `position` and `velocity` give, extended to a
        // column whose default is not the zero a resize gives for free.
        let mut e = Engine::default();
        let a = e.spawn(1.0, 2.0, 3.0);

        assert!(e.despawn(a));

        assert_eq!(e.column_len(COLOUR), Some(3));
        assert_eq!(
            colour_at(&e, a),
            Vec3::new(1.0, 1.0, 1.0),
            "a tombstoned slot's colour is left exactly as it was, not reset"
        );
    }

    #[test]
    fn positions_ptr_and_velocities_ptr_agree_with_the_named_lookup() {
        let mut e = Engine::default();
        e.spawn(1.0, 2.0, 3.0);

        assert_eq!(e.positions_ptr(), e.column_ptr("position").unwrap());
        assert_eq!(e.positions_len(), e.column_len("position").unwrap());
        assert_eq!(e.velocities_ptr(), e.column_ptr("velocity").unwrap());
        assert_eq!(e.velocities_len(), e.column_len("velocity").unwrap());
        assert_eq!(e.column_is_ints("position"), Some(false));
        assert_eq!(e.column_is_ints("velocity"), Some(false));
    }
}
