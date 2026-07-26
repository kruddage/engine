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
//! [`Store`] is the substrate: slots, generations, tombstones, and the rule
//! that a column is sized to [`Store::capacity`] rather than [`Store::len`].
//! [`World`] is the columns that sit on top of it — `alive`, `mask`, `parent`,
//! the transform pair, the name blob, and the per-component `*_ref` columns —
//! keyed by a dense entity id rather than a generational [`Handle`], because
//! that id is what the TypeScript half will eventually view directly.
//!
//! Two things stay out of `World` on purpose: the at-rest scene format and
//! its near-memcpy ingest, and per-entity param overrides (script / material
//! / mesh / texture *values*, as opposed to the `*_ref` columns that merely
//! name them) — both belong to the parameterised-assets work. Nothing here
//! reaches the `#[wasm_bindgen]` surface either; that is `krudd-web`'s
//! business, laid out in `docs/boundary.md`.

use krudd_math::Mat4;

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

/// The `parent` value marking an entity as a root — no parent at all.
///
/// Spelled out as a constant rather than `-1i32` scattered through the crate.
/// This column is **not** `Option<u32>`: the layout is what a future
/// TypeScript view reads directly, and `Option`'s representation is Rust's
/// business, not a contract — see `CODING_STANDARD.md`. `i32` with a
/// sentinel is the boring, portable choice.
pub const ROOT: i32 = -1;

/// Which optional per-entity component a `*_ref` column names.
///
/// A closed set rather than a string key: what an entity can carry is
/// exactly this list until the parameterised-assets work grows it, so "which
/// of these four" is a compile-time match rather than a runtime string
/// comparison on every read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Component {
    /// Indexes into a mesh asset.
    Mesh,
    /// Indexes into a material asset.
    Material,
    /// Indexes into a script asset.
    Script,
    /// Indexes into a texture asset.
    Texture,
}

impl Component {
    /// The bit this component claims in a [`World`] entity's `mask`.
    fn mask_bit(self) -> u32 {
        match self {
            Self::Mesh => 1 << 0,
            Self::Material => 1 << 1,
            Self::Script => 1 << 2,
            Self::Texture => 1 << 3,
        }
    }
}

/// The scene: flat parallel columns keyed by a dense entity id, plus the
/// hierarchy and transform data every entity carries.
///
/// ## Why a dense `u32` id instead of [`Handle`]
///
/// Every other column in the engine names its rows by a bare index because
/// that is what a typed-array view can carry across the wasm boundary
/// without translation — see `docs/boundary.md`. A generational [`Handle`]
/// solves a problem `World` does not have: slots here are never reused (see
/// [`World::destroy`]), so there is no "stale handle vs. fresh handle at the
/// same index" ambiguity for a generation to disambiguate. The plain index
/// *is* the identity, forever.
///
/// [`Store`] still does the work of handing out that index — `World` calls
/// [`Store::alloc`] and never [`Store::free`], which is what keeps id
/// issuance monotonic instead of writing a second allocator beside the one
/// this crate already has.
#[derive(Debug, Default)]
pub struct World {
    /// Issues dense entity ids. Never freed — see [`World::destroy`].
    store: Store,
    /// `1` if the entity is live, `0` if tombstoned. `u32`, not `bool`: every
    /// column here is a fixed-width type a typed-array view could cover.
    alive: Vec<u32>,
    /// Component-bit OR-set: which `*_ref` columns are meaningful for this
    /// entity.
    mask: Vec<u32>,
    /// The parent's entity id, or [`ROOT`]. Invariant 1 (enforced in
    /// [`World::spawn`]): always less than this entity's own id.
    parent: Vec<i32>,
    /// Transform relative to `parent` (or to the world, for a root).
    local: Vec<Mat4>,
    /// Transform relative to the world, derived by
    /// [`World::propagate_transforms`]. Not authoritative between calls to
    /// it — write `local`, not this.
    world_xform: Vec<Mat4>,
    /// Byte offset of this entity's name into `names`.
    name_offset: Vec<u32>,
    /// Every entity's name, concatenated, each terminated by a `NUL` byte —
    /// one contiguous blob rather than a `Vec<String>`, so a name is a length
    /// and an offset rather than a second heap allocation per entity.
    names: Vec<u8>,
    /// `Component::Mesh`'s ref column. Meaningful only where `mask` has
    /// [`Component::Mesh`]'s bit set.
    mesh_ref: Vec<u32>,
    /// `Component::Material`'s ref column.
    material_ref: Vec<u32>,
    /// `Component::Script`'s ref column.
    script_ref: Vec<u32>,
    /// `Component::Texture`'s ref column.
    texture_ref: Vec<u32>,
}

impl World {
    /// An empty world.
    pub fn new() -> Self {
        Self::default()
    }

    /// The number of entities ever created, live and tombstoned alike.
    ///
    /// **Not a live count.** [`World::destroy`] never shrinks this — see its
    /// doc comment for why reuse is the wrong trade here. A long editing
    /// session leaks slots; that cost was accepted deliberately (#828) rather
    /// than overlooked.
    pub fn entity_count(&self) -> u32 {
        self.alive.len() as u32
    }

    /// Whether `entity` is live.
    pub fn is_alive(&self, entity: u32) -> bool {
        self.alive.get(entity as usize).is_some_and(|&a| a != 0)
    }

    /// Appends a new entity to every column and returns its dense id.
    ///
    /// ### Invariant 1: parent index < child index, always
    ///
    /// Creation only ever appends: `index` is `entity_count()` before the
    /// push, which is larger than every id that already exists. Requiring
    /// `parent` to already be live therefore gets `parent < index` for free,
    /// with no separate check or per-frame sort — the storage order *is* the
    /// topological order from the moment an entity is born into it. That is
    /// what lets [`World::propagate_transforms`] resolve every world
    /// transform in one forward pass, and what lets [`World::destroy`]
    /// cascade a tombstone in one forward pass too.
    ///
    /// # Panics
    ///
    /// Panics if `parent` is not [`ROOT`] and does not address a live entity.
    /// There is no way to append a child before its parent exists in this
    /// column order, so asking for that is a caller bug, not a recoverable
    /// runtime condition.
    pub fn spawn(&mut self, parent: i32, local: Mat4, name: &str) -> u32 {
        let index = self.entity_count();
        if parent != ROOT {
            assert!(
                parent >= 0 && (parent as u32) < index && self.is_alive(parent as u32),
                "parent {parent} does not address a live entity created earlier than this one"
            );
        }
        self.alive.push(1);
        self.mask.push(0);
        self.parent.push(parent);
        self.local.push(local);
        // Resolved for real by `propagate_transforms`; a root's world
        // transform equals its local one until then, which is already
        // correct for a root and merely stale (not wrong-shaped) for a
        // child.
        self.world_xform.push(local);
        self.name_offset.push(self.names.len() as u32);
        self.names.extend_from_slice(name.as_bytes());
        self.names.push(0);
        self.mesh_ref.push(0);
        self.material_ref.push(0);
        self.script_ref.push(0);
        self.texture_ref.push(0);
        let handle = self.store.alloc();
        debug_assert_eq!(
            handle.index(),
            index,
            "the slot table and the columns fell out of lockstep"
        );
        index
    }

    /// Tombstones `entity` and every descendant, transitively, without
    /// moving any surviving entity's id.
    ///
    /// ### Invariant 2: destruction tombstones, it does not swap-remove
    ///
    /// Swap-remove is the obvious way to keep an array dense after a
    /// deletion, and it is wrong here: moving some other entity into the
    /// freed slot silently changes what every `parent` value naming that
    /// index refers to. The failure does not crash. A surviving entity's
    /// `parent` field still holds a small, in-range id — it now just names
    /// whoever got moved into that slot, an entity it was never attached to,
    /// discovered only when transform propagation puts it in the wrong place
    /// several frames later. Tombstoning in place keeps every `parent`
    /// reference valid forever.
    ///
    /// Cascading in a single forward scan from `entity + 1`, rather than
    /// recursing into children, works for the same reason invariant 1 makes
    /// [`World::propagate_transforms`] a single pass: a descendant's id is
    /// always greater than its ancestor's, so by the time the scan reaches
    /// any given entity, its parent — whether `entity` itself or one of
    /// `entity`'s own descendants tombstoned earlier in this same loop — has
    /// already been marked dead if it is going to be.
    ///
    /// ### The high-water-mark trade
    ///
    /// The tombstoned slot is never reused. Reusing a freed low id for a
    /// brand-new entity would let a later `spawn` violate invariant 1 the
    /// moment that new entity acquired a child with a smaller id than some
    /// unrelated, older entity — nothing about a recycled id stops that.
    /// [`World::entity_count`] therefore only grows; a long editing session
    /// leaks slots, and that is accepted rather than compacted here. Any
    /// compaction scheme has to rewrite `parent` *and* preserve the
    /// topological order, which is a bigger design than this method.
    pub fn destroy(&mut self, entity: u32) {
        let entity = entity as usize;
        if entity >= self.alive.len() || self.alive[entity] == 0 {
            return; // stale or already dead: a no-op, matching `Store::free`
        }
        self.alive[entity] = 0;
        for child in (entity + 1)..self.alive.len() {
            let parent = self.parent[child];
            if self.alive[child] != 0 && parent >= 0 && self.alive[parent as usize] == 0 {
                self.alive[child] = 0;
            }
        }
    }

    /// The parent's entity id, or [`ROOT`].
    pub fn parent_of(&self, entity: u32) -> i32 {
        self.parent[entity as usize]
    }

    /// The entity's transform relative to its parent (or to the world, for a
    /// root).
    pub fn local(&self, entity: u32) -> Mat4 {
        self.local[entity as usize]
    }

    /// Sets the entity's local transform. Does not itself update
    /// `world_xform` — call [`World::propagate_transforms`] once per phase,
    /// not once per write, for the same reason the boundary is batched.
    pub fn set_local(&mut self, entity: u32, transform: Mat4) {
        self.local[entity as usize] = transform;
    }

    /// The entity's transform relative to the world, as of the last
    /// [`World::propagate_transforms`].
    pub fn world_xform(&self, entity: u32) -> Mat4 {
        self.world_xform[entity as usize]
    }

    /// Recomputes every `world_xform` in one forward pass over the columns.
    ///
    /// Invariant 1 (parent index < child index) is what makes this a single
    /// linear scan: a parent's `world_xform` is always written before any of
    /// its children are visited, so there is no recursion, no visited set,
    /// and no per-frame topological sort — the storage order already is one.
    pub fn propagate_transforms(&mut self) {
        for i in 0..self.local.len() {
            if self.alive[i] == 0 {
                continue; // a dead subtree's transform is meaningless; leave it stale
            }
            let parent = self.parent[i];
            self.world_xform[i] = if parent < 0 {
                self.local[i]
            } else {
                // `parent < i` always (invariant 1), so `world_xform[parent]`
                // was already written earlier in this same pass.
                self.world_xform[parent as usize].mul(&self.local[i])
            };
        }
    }

    /// The entity's name, read out of the shared `NUL`-terminated blob.
    ///
    /// Does not check liveness: a tombstoned entity's name is still valid
    /// bytes, just no longer meaningful. Callers that care check
    /// [`World::is_alive`] themselves.
    pub fn name(&self, entity: u32) -> &str {
        let start = self.name_offset[entity as usize] as usize;
        let end = start
            + self.names[start..]
                .iter()
                .position(|&b| b == 0)
                .expect("every name was pushed with a trailing NUL by `spawn`");
        core::str::from_utf8(&self.names[start..end])
            .expect("names are pushed as UTF-8 bytes by `spawn`")
    }

    /// The entity's component-bit mask.
    pub fn mask(&self, entity: u32) -> u32 {
        self.mask[entity as usize]
    }

    /// The entity's ref for `component`, or `None` if its bit is unset in
    /// `mask`.
    ///
    /// Presence lives in the mask bit rather than a sentinel in the ref
    /// column itself, so an unset ref column never needs a reserved value
    /// carved out of `u32`'s range.
    pub fn component_ref(&self, entity: u32, component: Component) -> Option<u32> {
        let entity = entity as usize;
        if self.mask[entity] & component.mask_bit() == 0 {
            None
        } else {
            Some(self.ref_column(component)[entity])
        }
    }

    /// Sets the entity's ref for `component` and sets its mask bit.
    pub fn set_component_ref(&mut self, entity: u32, component: Component, r: u32) {
        let index = entity as usize;
        self.ref_column_mut(component)[index] = r;
        self.mask[index] |= component.mask_bit();
    }

    /// Clears the entity's mask bit for `component`, without touching the
    /// stored ref — the same way `destroy` tombstones rather than erasing,
    /// so a stale read is a bug that shows up as a wrong answer rather than
    /// a use of freed memory.
    pub fn clear_component(&mut self, entity: u32, component: Component) {
        self.mask[entity as usize] &= !component.mask_bit();
    }

    /// The raw `*_ref` column for `component`, whole. What "TypeScript can
    /// view this directly" means for this column: a contiguous `&[u32]`, not
    /// a getter per entity.
    pub fn ref_column(&self, component: Component) -> &[u32] {
        match component {
            Component::Mesh => &self.mesh_ref,
            Component::Material => &self.material_ref,
            Component::Script => &self.script_ref,
            Component::Texture => &self.texture_ref,
        }
    }

    fn ref_column_mut(&mut self, component: Component) -> &mut Vec<u32> {
        match component {
            Component::Mesh => &mut self.mesh_ref,
            Component::Material => &mut self.material_ref,
            Component::Script => &mut self.script_ref,
            Component::Texture => &mut self.texture_ref,
        }
    }

    /// The `alive` column, whole.
    pub fn alive_column(&self) -> &[u32] {
        &self.alive
    }

    /// The `mask` column, whole.
    pub fn mask_column(&self) -> &[u32] {
        &self.mask
    }

    /// The `parent` column, whole — `i32`, `-1` for root, never
    /// `Option<u32>`. See [`ROOT`].
    pub fn parent_column(&self) -> &[i32] {
        &self.parent
    }

    /// The `local` transform column, whole.
    pub fn local_column(&self) -> &[Mat4] {
        &self.local
    }

    /// The `world_xform` column, whole, as of the last
    /// [`World::propagate_transforms`].
    pub fn world_xform_column(&self) -> &[Mat4] {
        &self.world_xform
    }

    /// The `name_offset` column, whole.
    pub fn name_offset_column(&self) -> &[u32] {
        &self.name_offset
    }

    /// The name blob, whole: every entity's name concatenated, each
    /// terminated by a `NUL` byte.
    pub fn name_blob(&self) -> &[u8] {
        &self.names
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

    use krudd_math::Vec3;

    #[test]
    fn parent_index_is_always_less_than_child_index() {
        let mut w = World::new();
        let root = w.spawn(ROOT, Mat4::IDENTITY, "root");
        let a = w.spawn(root as i32, Mat4::IDENTITY, "a");
        w.spawn(root as i32, Mat4::IDENTITY, "b");
        w.spawn(a as i32, Mat4::IDENTITY, "c");

        for entity in 0..w.entity_count() {
            let parent = w.parent_of(entity);
            if parent != ROOT {
                assert!(
                    (parent as u32) < entity,
                    "entity {entity} has parent {parent}, which is not a smaller index"
                );
            }
        }
    }

    #[test]
    #[should_panic(expected = "does not address a live entity")]
    fn spawning_a_child_of_an_entity_that_does_not_exist_yet_panics() {
        let mut w = World::new();
        w.spawn(5, Mat4::IDENTITY, "orphan");
    }

    #[test]
    #[should_panic(expected = "does not address a live entity")]
    fn spawning_a_child_of_a_tombstoned_entity_panics() {
        let mut w = World::new();
        let a = w.spawn(ROOT, Mat4::IDENTITY, "a");
        w.destroy(a);
        w.spawn(a as i32, Mat4::IDENTITY, "child-of-dead");
    }

    #[test]
    fn destroying_a_parent_does_not_move_a_surviving_entitys_index() {
        let mut w = World::new();
        let root = w.spawn(ROOT, Mat4::IDENTITY, "root");
        let a = w.spawn(root as i32, Mat4::IDENTITY, "a");
        let b = w.spawn(a as i32, Mat4::IDENTITY, "b");
        let c = w.spawn(root as i32, Mat4::IDENTITY, "c");

        w.destroy(a);

        // The count only ever grows — a swap-remove would have shrunk it to
        // fill the gap.
        assert_eq!(w.entity_count(), 4);
        assert!(!w.is_alive(a));
        assert!(!w.is_alive(b));
        // `root` and the unrelated sibling `c` did not move: same id, same
        // parent, same name. A swap-remove would move the array's last live
        // entity into `a`'s freed slot — exactly the "teleports under a
        // parent it was never attached to" bug this guards against.
        assert!(w.is_alive(root));
        assert!(w.is_alive(c));
        assert_eq!(c, 3);
        assert_eq!(w.parent_of(c), root as i32);
        assert_eq!(w.name(c), "c");
    }

    #[test]
    fn destroying_an_ancestor_tombstones_every_descendant_transitively() {
        let mut w = World::new();
        let root = w.spawn(ROOT, Mat4::IDENTITY, "root");
        let child = w.spawn(root as i32, Mat4::IDENTITY, "child");
        let grandchild = w.spawn(child as i32, Mat4::IDENTITY, "grandchild");
        let great_grandchild = w.spawn(grandchild as i32, Mat4::IDENTITY, "gg");
        let unrelated = w.spawn(ROOT, Mat4::IDENTITY, "unrelated");

        w.destroy(child);

        assert!(!w.is_alive(child));
        assert!(!w.is_alive(grandchild));
        assert!(!w.is_alive(great_grandchild));
        assert!(w.is_alive(root));
        assert!(w.is_alive(unrelated));
    }

    #[test]
    fn destroying_an_already_dead_entity_is_a_no_op() {
        let mut w = World::new();
        let a = w.spawn(ROOT, Mat4::IDENTITY, "a");
        w.destroy(a);
        assert_eq!(w.entity_count(), 1);
        w.destroy(a);
        assert_eq!(w.entity_count(), 1);
        assert!(!w.is_alive(a));
    }

    #[test]
    fn destroying_an_out_of_range_entity_is_a_no_op() {
        let mut w = World::new();
        w.spawn(ROOT, Mat4::IDENTITY, "a");
        w.destroy(99);
        assert_eq!(w.entity_count(), 1);
    }

    #[test]
    fn tombstoned_slots_are_never_reused_entity_count_stays_a_high_water_mark() {
        let mut w = World::new();
        let a = w.spawn(ROOT, Mat4::IDENTITY, "a");
        w.spawn(ROOT, Mat4::IDENTITY, "b");
        w.destroy(a);
        let c = w.spawn(ROOT, Mat4::IDENTITY, "c");

        // A pool that recycled `a`'s freed slot would hand `c` index 0 again.
        assert_eq!(c, 2);
        assert_eq!(w.entity_count(), 3);
    }

    #[test]
    fn propagate_transforms_resolves_world_space_in_one_forward_pass() {
        let mut w = World::new();
        let root = w.spawn(
            ROOT,
            Mat4::from_translation(Vec3::new(1.0, 0.0, 0.0)),
            "root",
        );
        let child = w.spawn(
            root as i32,
            Mat4::from_translation(Vec3::new(0.0, 2.0, 0.0)),
            "child",
        );
        let grandchild = w.spawn(
            child as i32,
            Mat4::from_translation(Vec3::new(0.0, 0.0, 3.0)),
            "gc",
        );

        w.propagate_transforms();

        assert_eq!(
            w.world_xform(root),
            Mat4::from_translation(Vec3::new(1.0, 0.0, 0.0))
        );
        assert_eq!(
            w.world_xform(child),
            Mat4::from_translation(Vec3::new(1.0, 2.0, 0.0))
        );
        assert_eq!(
            w.world_xform(grandchild),
            Mat4::from_translation(Vec3::new(1.0, 2.0, 3.0))
        );
    }

    #[test]
    fn propagate_transforms_leaves_a_tombstoned_entitys_world_xform_alone() {
        let mut w = World::new();
        let root = w.spawn(ROOT, Mat4::IDENTITY, "root");
        let doomed = w.spawn(
            root as i32,
            Mat4::from_translation(Vec3::new(9.0, 9.0, 9.0)),
            "doomed",
        );
        w.propagate_transforms();
        let before = w.world_xform(doomed);

        w.destroy(doomed);
        w.propagate_transforms();

        assert_eq!(w.world_xform(doomed), before);
    }

    #[test]
    fn names_are_nul_terminated_and_addressed_by_offset() {
        let mut w = World::new();
        let a = w.spawn(ROOT, Mat4::IDENTITY, "alpha");
        let b = w.spawn(ROOT, Mat4::IDENTITY, "beta");

        assert_eq!(w.name(a), "alpha");
        assert_eq!(w.name(b), "beta");
        // One contiguous blob with NUL separators, not a `Vec<String>` in
        // disguise.
        assert_eq!(w.name_blob(), b"alpha\0beta\0");
    }

    #[test]
    fn setting_a_component_ref_sets_its_mask_bit() {
        let mut w = World::new();
        let a = w.spawn(ROOT, Mat4::IDENTITY, "a");

        assert_eq!(w.component_ref(a, Component::Mesh), None);
        w.set_component_ref(a, Component::Mesh, 42);
        assert_eq!(w.component_ref(a, Component::Mesh), Some(42));
        assert_ne!(w.mask(a) & Component::Mesh.mask_bit(), 0);
        // Setting one component's ref does not conjure another's presence.
        assert_eq!(w.component_ref(a, Component::Material), None);
    }

    #[test]
    fn clearing_a_component_unsets_the_mask_bit_but_keeps_the_stored_ref() {
        let mut w = World::new();
        let a = w.spawn(ROOT, Mat4::IDENTITY, "a");
        w.set_component_ref(a, Component::Script, 7);

        w.clear_component(a, Component::Script);

        assert_eq!(w.component_ref(a, Component::Script), None);
        // The mask bit gates visibility; the stored ref is left in place,
        // the same way a tombstoned entity's columns are left in place.
        assert_eq!(w.ref_column(Component::Script)[a as usize], 7);
    }

    #[test]
    fn parent_column_is_a_contiguous_slice_of_fixed_width_i32() {
        let mut w = World::new();
        let root = w.spawn(ROOT, Mat4::IDENTITY, "root");
        let a = w.spawn(root as i32, Mat4::IDENTITY, "a");
        let b = w.spawn(root as i32, Mat4::IDENTITY, "b");
        let col = w.parent_column();

        assert_eq!(col.len(), 3);
        // Contiguity, not merely a matching length: each element sits
        // exactly one `i32` after the last, which is what lets a typed-array
        // view stride over it with no gaps.
        let ptr = col.as_ptr();
        for (i, elem) in col.iter().enumerate() {
            assert_eq!(elem as *const i32, ptr.wrapping_add(i));
        }
        assert_eq!(col[a as usize], root as i32);
        assert_eq!(col[b as usize], root as i32);
    }

    #[test]
    fn alive_and_mask_columns_are_fixed_width_u32() {
        let mut w = World::new();
        w.spawn(ROOT, Mat4::IDENTITY, "a");
        w.spawn(ROOT, Mat4::IDENTITY, "b");

        assert_eq!(size_of_val(w.alive_column()), 2 * size_of::<u32>());
        assert_eq!(size_of_val(w.mask_column()), 2 * size_of::<u32>());
    }
}
