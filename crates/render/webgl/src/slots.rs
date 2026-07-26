// SPDX-License-Identifier: GPL-2.0-or-later

//! The slot table behind every handle the backend hands out.
//!
//! `krudd_gpu::Id` is a slot index plus a generation, and this is the half
//! that makes the generation mean something: a freed slot's generation
//! advances, so a handle issued before the free resolves to `None` rather
//! than to whatever now occupies the slot.
//!
//! It is generic over what it stores so that the invariant can be tested
//! without a GPU — the table is the part with the off-by-one in it, and a
//! `Device` is not needed to catch that.

use krudd_gpu::{Id, Kind};

/// A generational slot table.
pub struct Slots<K: Kind, T> {
    /// The occupants, `None` where a slot is free.
    entries: Vec<Option<T>>,
    /// The generation each slot is currently at. Parallel to `entries`.
    generations: Vec<u32>,
    /// Freed slots, newest first. Reusing a slot is what makes the generation
    /// necessary in the first place.
    free: Vec<u32>,
    /// The handle type this table issues.
    kind: std::marker::PhantomData<fn() -> K>,
}

impl<K: Kind, T> Default for Slots<K, T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<K: Kind, T> Slots<K, T> {
    /// An empty table.
    pub fn new() -> Self {
        Self {
            entries: Vec::new(),
            generations: Vec::new(),
            free: Vec::new(),
            kind: std::marker::PhantomData,
        }
    }

    /// Stores a value and returns the handle that names it.
    pub fn insert(&mut self, value: T) -> Id<K> {
        match self.free.pop() {
            Some(slot) => {
                let i = slot as usize;
                self.entries[i] = Some(value);
                Id::new(slot, self.generations[i])
            }
            None => {
                let slot = self.entries.len() as u32;
                self.entries.push(Some(value));
                self.generations.push(0);
                Id::new(slot, 0)
            }
        }
    }

    /// The value a handle names, or `None` if the handle is stale or the slot
    /// is free.
    pub fn get(&self, id: Id<K>) -> Option<&T> {
        let i = id.slot() as usize;
        if *self.generations.get(i)? != id.generation() {
            return None;
        }
        self.entries.get(i)?.as_ref()
    }

    /// Frees a slot, advancing its generation so every outstanding handle to
    /// it goes stale. Returns whether the handle named a live slot.
    pub fn remove(&mut self, id: Id<K>) -> bool {
        let i = id.slot() as usize;
        if self.get(id).is_none() {
            return false;
        }
        self.entries[i] = None;
        // Wrapping rather than saturating: a generation that saturated would
        // stop distinguishing handles silently, where wrapping needs 2^32
        // frees of one slot to collide.
        self.generations[i] = self.generations[i].wrapping_add(1);
        self.free.push(id.slot());
        true
    }

    /// How many slots are occupied.
    ///
    /// Test-only: the renderer never asks, but a test does — a `remove` that
    /// bumped the generation without vacating the slot would keep the GPU
    /// resource alive forever while every handle to it reported dead, and
    /// `get` alone cannot tell those two apart.
    #[cfg(test)]
    pub fn occupied(&self) -> usize {
        self.entries.iter().filter(|e| e.is_some()).count()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use krudd_gpu::Pipeline;

    type Table = Slots<Pipeline, &'static str>;

    #[test]
    fn a_stored_value_is_reachable_through_its_handle() {
        let mut table = Table::new();
        let id = table.insert("triangle");
        assert_eq!(table.get(id), Some(&"triangle"));
        assert_eq!(table.occupied(), 1);
    }

    #[test]
    fn slots_are_handed_out_from_zero_upward() {
        let mut table = Table::new();
        assert_eq!(table.insert("a").slot(), 0);
        assert_eq!(table.insert("b").slot(), 1);
    }

    #[test]
    fn a_handle_to_a_freed_slot_does_not_resolve() {
        let mut table = Table::new();
        let id = table.insert("gone");
        assert!(table.remove(id));
        assert_eq!(table.get(id), None);
        assert!(!table.remove(id), "a second free is not a free");
        assert_eq!(table.occupied(), 0);
    }

    #[test]
    fn a_recycled_slot_does_not_resurrect_the_old_handle() {
        // The whole reason the generation exists. Without it the stale handle
        // below would read the new occupant, and the bug would present as one
        // pipeline drawing with another's shader.
        let mut table = Table::new();
        let stale = table.insert("old");
        table.remove(stale);

        let fresh = table.insert("new");
        assert_eq!(fresh.slot(), stale.slot(), "the slot should be reused");
        assert_ne!(fresh.generation(), stale.generation());
        assert_eq!(table.get(fresh), Some(&"new"));
        assert_eq!(table.get(stale), None);
    }

    #[test]
    fn an_out_of_range_handle_does_not_resolve() {
        let table = Table::new();
        assert_eq!(table.get(Id::new(7, 0)), None);
    }
}
