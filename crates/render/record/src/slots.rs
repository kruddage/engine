// SPDX-License-Identifier: GPL-2.0-or-later

//! The generational slot table behind every handle [`Recorder`](crate::Recorder)
//! hands out.
//!
//! Mirrors `krudd-webgl`'s table of the same shape (`crates/render/webgl/src/slots.rs`):
//! a handle is a slot index plus a generation, and reusing a slot without
//! bumping the generation is exactly what would let a handle from before a
//! `destroy_*` call resolve to whatever now occupies its slot. A recorder
//! that got this wrong would be worse than useless as a test oracle — it
//! would pass a test built on the assumption that stale handles go stale.
//!
//! Unlike the backend's table, this one stores nothing: [`Recorder`] tracks
//! handle *liveness*, not GPU objects, since there is no GPU object to keep.
//! Every slot is `bool` rather than `Option<T>` for the same reason.

use krudd_gpu::{Id, Kind};

/// A generational slot table that tracks liveness only.
pub(crate) struct Slots<K: Kind> {
    /// `true` where a slot is occupied, `false` where it is free.
    live: Vec<bool>,
    /// The generation each slot is currently at. Parallel to `live`.
    generations: Vec<u32>,
    /// Freed slots, newest first — reusing one is the whole reason the
    /// generation has to exist.
    free: Vec<u32>,
    kind: core::marker::PhantomData<fn() -> K>,
}

impl<K: Kind> Default for Slots<K> {
    fn default() -> Self {
        Self::new()
    }
}

impl<K: Kind> Slots<K> {
    /// An empty table.
    pub(crate) fn new() -> Self {
        Self {
            live: Vec::new(),
            generations: Vec::new(),
            free: Vec::new(),
            kind: core::marker::PhantomData,
        }
    }

    /// Allocates a handle for a new resource.
    pub(crate) fn insert(&mut self) -> Id<K> {
        match self.free.pop() {
            Some(slot) => {
                let i = slot as usize;
                self.live[i] = true;
                Id::new(slot, self.generations[i])
            }
            None => {
                let slot = self.live.len() as u32;
                self.live.push(true);
                self.generations.push(0);
                Id::new(slot, 0)
            }
        }
    }

    /// Whether a handle currently names a live slot, rather than a free one
    /// or a generation that has since moved on.
    pub(crate) fn is_live(&self, id: Id<K>) -> bool {
        let i = id.slot() as usize;
        self.generations.get(i) == Some(&id.generation()) && self.live.get(i) == Some(&true)
    }

    /// Frees a slot, advancing its generation so every outstanding handle to
    /// it goes stale. Returns whether the handle named a live slot — a
    /// second free of the same handle is a no-op, not a double-free.
    pub(crate) fn remove(&mut self, id: Id<K>) -> bool {
        if !self.is_live(id) {
            return false;
        }
        let i = id.slot() as usize;
        self.live[i] = false;
        // Wrapping rather than saturating: a saturated generation would stop
        // distinguishing handles silently, where wrapping needs 2^32 frees of
        // one slot to collide.
        self.generations[i] = self.generations[i].wrapping_add(1);
        self.free.push(id.slot());
        true
    }

    /// How many slots are currently occupied.
    pub(crate) fn live_count(&self) -> usize {
        self.live.iter().filter(|&&l| l).count()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use krudd_gpu::Texture;

    type Table = Slots<Texture>;

    #[test]
    fn a_reused_slot_does_not_answer_to_the_old_handle() {
        let mut table = Table::new();
        let stale = table.insert();
        table.remove(stale);

        let fresh = table.insert();
        assert_eq!(fresh.slot(), stale.slot(), "the slot should be reused");
        assert_ne!(fresh.generation(), stale.generation());
        assert!(table.is_live(fresh));
        assert!(!table.is_live(stale));
    }

    #[test]
    fn a_second_free_of_the_same_handle_is_not_a_free() {
        let mut table = Table::new();
        let id = table.insert();
        assert!(table.remove(id));
        assert!(!table.remove(id));
    }

    #[test]
    fn an_out_of_range_handle_is_not_live() {
        let table = Table::new();
        assert!(!table.is_live(Id::new(3, 0)));
    }

    #[test]
    fn live_count_tracks_inserts_and_removes() {
        let mut table = Table::new();
        let a = table.insert();
        let _b = table.insert();
        assert_eq!(table.live_count(), 2);
        table.remove(a);
        assert_eq!(table.live_count(), 1);
    }
}
