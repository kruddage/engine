// SPDX-License-Identifier: GPL-2.0-or-later

//! The scene's storage: dense, index-addressed columns and the handles that
//! address them.
//!
//! **Tier: `world`.** May reach for `base`. Nothing about GPUs, frames or
//! backends belongs here.
//!
//! ## Why the columns are separate
//!
//! Storage is struct-of-arrays because the whole point of the Rust half is
//! that TypeScript reads it as a typed array without a copy. An array of
//! structs would force TS to stride over fields it does not want; a column
//! per field means `new Float32Array(memory.buffer, ptr, len)` **is** the
//! view. Keeping that true is what makes the boundary batched rather than
//! per-object — see the `## The boundary is batched` principle on #812.
//!
//! ## Scope
//!
//! This is the storage substrate: slots, generations, tombstones, and the
//! column vocabulary. The full carry — topological parent-before-child
//! ordering, and the compaction policy that keeps iteration dense as
//! tombstones accumulate — is #828. What is here is what the shell needs to
//! stand up and what #828 builds on, not a finished world.

/// A handle to a slot in a [`Store`].
///
/// The generation is what makes a stale handle detectable rather than
/// dangerous: freeing a slot bumps its generation, so a handle held across
/// the free resolves to `None` instead of silently addressing whatever was
/// allocated into the slot next.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Handle {
    index: u32,
    generation: u32,
}

impl Handle {
    /// The slot this handle addresses.
    ///
    /// Only meaningful together with the [`Store`] that issued it, and only
    /// while [`Store::contains`] still holds.
    pub fn index(self) -> u32 {
        self.index
    }

    /// The generation this handle was issued at.
    pub fn generation(self) -> u32 {
        self.generation
    }
}

/// The state of one slot.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Slot {
    /// Live at this generation.
    Live(u32),
    /// A tombstone: dead at this generation, reusable.
    Dead(u32),
}

/// A generational slot table.
///
/// The table owns no component data. Columns are separate `Vec`s indexed by
/// [`Handle::index`], which is what keeps them contiguous for the typed-array
/// views the TypeScript half builds over them.
#[derive(Debug, Default)]
pub struct Store {
    slots: Vec<Slot>,
    /// Indices of tombstoned slots, most recently freed first.
    free: Vec<u32>,
    live: usize,
}

impl Store {
    /// An empty store.
    pub fn new() -> Self {
        Self::default()
    }

    /// The number of live slots.
    pub fn len(&self) -> usize {
        self.live
    }

    /// Whether the store holds no live slots.
    pub fn is_empty(&self) -> bool {
        self.live == 0
    }

    /// The number of slots the columns must be sized to, live and tombstoned
    /// alike.
    ///
    /// Callers sizing a column use this, not [`Store::len`]: a tombstoned slot
    /// still occupies its index, and a column shorter than the table would
    /// mean a live handle indexing past its end.
    pub fn capacity(&self) -> usize {
        self.slots.len()
    }

    /// Allocates a slot and returns a handle to it.
    ///
    /// Reuses a tombstone when one is available, so the table stays as dense
    /// as the live set allows and column indices stay small.
    pub fn alloc(&mut self) -> Handle {
        self.live += 1;
        match self.free.pop() {
            Some(index) => {
                let generation = match self.slots[index as usize] {
                    Slot::Dead(generation) => generation,
                    // Unreachable: an index only enters `free` on `free()`,
                    // which writes a tombstone before pushing it.
                    Slot::Live(_) => unreachable!("free list held a live slot"),
                };
                self.slots[index as usize] = Slot::Live(generation);
                Handle { index, generation }
            }
            None => {
                let index = self.slots.len() as u32;
                self.slots.push(Slot::Live(0));
                Handle {
                    index,
                    generation: 0,
                }
            }
        }
    }

    /// Whether the handle still addresses a live slot.
    pub fn contains(&self, handle: Handle) -> bool {
        matches!(
            self.slots.get(handle.index as usize),
            Some(&Slot::Live(generation)) if generation == handle.generation
        )
    }

    /// Frees the slot a handle addresses, leaving a tombstone.
    ///
    /// Returns whether anything was freed — a stale handle frees nothing
    /// rather than freeing whoever holds the slot now.
    pub fn free(&mut self, handle: Handle) -> bool {
        if !self.contains(handle) {
            return false;
        }
        // Bumping the generation here, not on the next alloc, is what makes
        // every handle to this slot stale from the moment of the free.
        self.slots[handle.index as usize] = Slot::Dead(handle.generation.wrapping_add(1));
        self.free.push(handle.index);
        self.live -= 1;
        true
    }

    /// The live handle for a slot index, or `None` if the slot is tombstoned
    /// or out of range.
    ///
    /// This is how a caller holding a bare index — anything that crossed a
    /// language boundary, where a generational handle does not fit — gets
    /// back to a checked one.
    pub fn handle(&self, index: u32) -> Option<Handle> {
        match self.slots.get(index as usize) {
            Some(&Slot::Live(generation)) => Some(Handle { index, generation }),
            Some(&Slot::Dead(_)) | None => None,
        }
    }

    /// Iterates the live handles in slot order.
    pub fn iter(&self) -> impl Iterator<Item = Handle> + '_ {
        self.slots
            .iter()
            .enumerate()
            .filter_map(|(index, slot)| match *slot {
                Slot::Live(generation) => Some(Handle {
                    index: index as u32,
                    generation,
                }),
                Slot::Dead(_) => None,
            })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_fresh_store_is_empty() {
        let store = Store::new();
        assert!(store.is_empty());
        assert_eq!(store.capacity(), 0);
    }

    #[test]
    fn allocation_hands_out_distinct_slots() {
        let mut store = Store::new();
        let a = store.alloc();
        let b = store.alloc();
        assert_ne!(a.index(), b.index());
        assert_eq!(store.len(), 2);
        assert_eq!(store.capacity(), 2);
    }

    #[test]
    fn a_freed_handle_stops_resolving() {
        let mut store = Store::new();
        let h = store.alloc();
        assert!(store.contains(h));
        assert!(store.free(h));
        assert!(!store.contains(h));
        assert_eq!(store.len(), 0);
    }

    #[test]
    fn freeing_twice_is_a_no_op_not_a_double_free() {
        let mut store = Store::new();
        let h = store.alloc();
        assert!(store.free(h));
        assert!(!store.free(h));
        assert_eq!(store.len(), 0);
    }

    #[test]
    fn a_reused_slot_does_not_answer_to_the_old_handle() {
        let mut store = Store::new();
        let old = store.alloc();
        store.free(old);
        let new = store.alloc();
        // The whole point of the generation: same slot, different handle.
        assert_eq!(old.index(), new.index());
        assert_ne!(old.generation(), new.generation());
        assert!(store.contains(new));
        assert!(!store.contains(old));
    }

    #[test]
    fn tombstones_are_reused_before_the_table_grows() {
        let mut store = Store::new();
        let a = store.alloc();
        let _b = store.alloc();
        store.free(a);
        store.alloc();
        assert_eq!(
            store.capacity(),
            2,
            "the freed slot should have been reused"
        );
    }

    #[test]
    fn iteration_skips_tombstones() {
        let mut store = Store::new();
        let a = store.alloc();
        let b = store.alloc();
        let c = store.alloc();
        store.free(b);
        assert_eq!(store.iter().collect::<Vec<_>>(), vec![a, c]);
    }

    #[test]
    fn a_bare_index_resolves_only_while_the_slot_is_live() {
        let mut store = Store::new();
        let h = store.alloc();
        assert_eq!(store.handle(h.index()), Some(h));
        store.free(h);
        assert_eq!(store.handle(h.index()), None);
        assert_eq!(store.handle(99), None);
    }

    #[test]
    fn capacity_covers_every_live_index() {
        // A column sized to `capacity()` must be indexable by every live
        // handle — the invariant a column allocator depends on.
        let mut store = Store::new();
        for _ in 0..8 {
            store.alloc();
        }
        let doomed: Vec<_> = store.iter().step_by(3).collect();
        for h in doomed {
            store.free(h);
        }
        store.alloc();
        for h in store.iter() {
            assert!((h.index() as usize) < store.capacity());
        }
    }
}
