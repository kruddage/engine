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
//! The exported surface here is deliberately tiny, and none of it is
//! per-object. [`Engine::tick`] advances the whole world in one call, and
//! TypeScript reads the result as a `Float32Array` mapped straight over wasm
//! linear memory via [`Engine::positions_ptr`] — no serialisation, no copy,
//! no call per entity. Violating that would look like "wasm is slow" and
//! would not be.
//!
//! ## Scope
//!
//! This is the loadable artifact #815 asks for and the thing #816's TypeScript
//! build links against, not the engine. It draws nothing — the WebGL2
//! renderer is #818 and the HTML shell around it is #819. Generating both
//! sides of this boundary from one spec, rather than hand-writing the pair,
//! is #817 and #824.

use krudd_math::Vec3;
use krudd_render::{Frame, Viewport};
use krudd_world::{Handle, Store};
use wasm_bindgen::prelude::wasm_bindgen;

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
}

impl Default for Engine {
    fn default() -> Self {
        Self::new(1, 1)
    }
}

#[wasm_bindgen]
impl Engine {
    /// Boots an engine rendering into a canvas of the given size, in physical
    /// pixels.
    #[wasm_bindgen(constructor)]
    pub fn new(width: u32, height: u32) -> Self {
        Self {
            store: Store::new(),
            positions: Vec::new(),
            velocities: Vec::new(),
            viewport: Viewport::new(width, height),
            frame_count: 0,
            elapsed: 0.0,
        }
    }

    /// Spawns an entity and returns its slot index.
    ///
    /// The slot index, not the generational handle: a `u32` is what a
    /// TypeScript caller can hold, and it is also the index into the position
    /// view, which is the only thing the page does with it. Handing out the
    /// generation as well is #817's business, once the boundary is generated
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
    pub fn resize(&mut self, width: u32, height: u32) {
        self.viewport = Viewport::new(width, height);
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
}

impl Engine {
    /// Builds the frame the backend would draw.
    ///
    /// Not exported: a `Frame` is Rust's own value, and nothing on the page
    /// can do anything with it until there is a backend to hand it to (#818).
    /// It exists now so the tick path is exercised end to end rather than
    /// stopping at the position column.
    pub fn build_frame(&self) -> Frame {
        Frame::new(self.viewport)
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

#[cfg(test)]
mod tests {
    use super::*;

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
    fn resize_clamps_a_zero_sized_canvas() {
        let mut e = Engine::new(800, 600);
        e.resize(0, 0);
        assert_eq!((e.width(), e.height()), (1, 1));
    }

    #[test]
    fn the_frame_carries_the_current_viewport() {
        let mut e = Engine::new(320, 240);
        e.resize(640, 480);
        assert_eq!(e.build_frame().viewport, Viewport::new(640, 480));
    }

    #[test]
    fn version_is_reported() {
        assert!(!version().is_empty());
    }
}
