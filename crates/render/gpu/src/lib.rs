// SPDX-License-Identifier: GPL-2.0-or-later

//! GPU resources, as handles rather than objects.
//!
//! **Tier: `render`.** May reach for `base` and `world`. It owns no backend:
//! a handle here names a resource that some backend holds, and the backend
//! is the only thing that can turn it back into an object.
//!
//! ## Why handles and not objects
//!
//! Two reasons, both carried from the old tree:
//!
//! - **A handle crosses the language boundary; a `WebGLTexture` does not.**
//!   The TypeScript half needs to name a texture — to show it in the
//!   inspector, to hand it back as a render target — and a 32-bit integer is
//!   the only representation both halves can hold. The old engine reached the
//!   same conclusion with its opaque texture-handle pair.
//! - **A handle can outlive the resource safely.** A dropped texture leaves a
//!   stale handle that resolves to nothing, where a raw pointer or a live JS
//!   object reference would either dangle or quietly keep the GPU memory
//!   alive.
//!
//! Every handle is generational for the same reason [`krudd_world::Handle`]
//! is: reusing a slot must not resurrect an old handle.

use core::fmt;
use core::marker::PhantomData;
use core::num::NonZeroU32;

/// A typed, generational handle to a GPU resource.
///
/// The `K` parameter is what stops a [`BufferId`] being passed where a
/// [`TextureId`] is wanted. It is a zero-sized marker: the handle is still a
/// pair of `u32`s at runtime.
pub struct Id<K: Kind> {
    /// Slot index plus one. The offset buys the `NonZeroU32` niche, which is
    /// what makes `Option<Id>` the same size as `Id` — the representation a
    /// backend's slot table wants, since most of its entries are empty.
    slot: NonZeroU32,
    generation: u32,
    kind: PhantomData<fn() -> K>,
}

/// The marker trait for what an [`Id`] points at.
pub trait Kind: 'static {
    /// The resource name, for logs and error messages.
    const NAME: &'static str;
}

/// Marks an [`Id`] as naming a buffer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Buffer;
impl Kind for Buffer {
    const NAME: &'static str = "buffer";
}

/// Marks an [`Id`] as naming a texture.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Texture;
impl Kind for Texture {
    const NAME: &'static str = "texture";
}

/// Marks an [`Id`] as naming a shader program / pipeline.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Pipeline;
impl Kind for Pipeline {
    const NAME: &'static str = "pipeline";
}

/// A handle to a GPU buffer.
pub type BufferId = Id<Buffer>;
/// A handle to a GPU texture.
pub type TextureId = Id<Texture>;
/// A handle to a GPU pipeline.
pub type PipelineId = Id<Pipeline>;

impl<K: Kind> Id<K> {
    /// Builds a handle from a slot index and a generation.
    ///
    /// Only a backend calls this — it is the thing that knows the slot is
    /// real. Everyone else receives handles rather than making them.
    pub fn new(slot: u32, generation: u32) -> Self {
        Self {
            slot: NonZeroU32::new(slot.wrapping_add(1)).expect("slot index u32::MAX is reserved"),
            generation,
            kind: PhantomData,
        }
    }

    /// The slot index this handle addresses.
    pub fn slot(self) -> u32 {
        self.slot.get() - 1
    }

    /// The generation this handle was issued at.
    pub fn generation(self) -> u32 {
        self.generation
    }

    /// The handle packed into a single `u64`, slot in the low half.
    ///
    /// This is the form that crosses into TypeScript: one number rather than
    /// two, so a handle cannot be reassembled out of a mismatched pair.
    pub fn to_bits(self) -> u64 {
        (u64::from(self.generation) << 32) | u64::from(self.slot())
    }

    /// The inverse of [`Id::to_bits`].
    pub fn from_bits(bits: u64) -> Self {
        Self::new(bits as u32, (bits >> 32) as u32)
    }
}

// Derived impls would require `K: Clone` and friends, which no marker type
// needs to satisfy — the `PhantomData<fn() -> K>` holds no `K`.
impl<K: Kind> Clone for Id<K> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<K: Kind> Copy for Id<K> {}
impl<K: Kind> PartialEq for Id<K> {
    fn eq(&self, other: &Self) -> bool {
        self.slot == other.slot && self.generation == other.generation
    }
}
impl<K: Kind> Eq for Id<K> {}
impl<K: Kind> fmt::Debug for Id<K> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}#{}v{}", K::NAME, self.slot(), self.generation)
    }
}

/// How the contents of a texture are laid out.
///
/// A deliberately short list: it covers what a WebGL2 backend can render to
/// and sample from without an extension, which is the only backend #812 is
/// building. Widening it is the WebGPU work, not this crate's business.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TextureFormat {
    /// Eight bits per channel, sRGB-encoded colour with alpha.
    Rgba8UnormSrgb,
    /// Eight bits per channel, linear colour with alpha.
    Rgba8Unorm,
    /// A single 32-bit float channel — the depth attachment.
    Depth32Float,
}

impl TextureFormat {
    /// Bytes one texel occupies.
    pub fn bytes_per_texel(self) -> u32 {
        match self {
            Self::Rgba8UnormSrgb | Self::Rgba8Unorm => 4,
            Self::Depth32Float => 4,
        }
    }

    /// Whether this format is a depth attachment rather than a colour one.
    pub fn is_depth(self) -> bool {
        matches!(self, Self::Depth32Float)
    }
}

/// What a buffer is bound as.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BufferUsage {
    /// Per-vertex attribute data.
    Vertex,
    /// Indices into a vertex buffer.
    Index,
    /// Shader-visible constants.
    Uniform,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_handle_round_trips_through_its_bit_pattern() {
        let id = TextureId::new(7, 3);
        assert_eq!(TextureId::from_bits(id.to_bits()), id);
        assert_eq!(id.slot(), 7);
        assert_eq!(id.generation(), 3);
    }

    #[test]
    fn slot_zero_is_representable() {
        // The internal +1 offset is there for the niche; it must not make the
        // first slot a backend allocates unaddressable.
        let id = BufferId::new(0, 0);
        assert_eq!(id.slot(), 0);
        assert_eq!(id.to_bits(), 0);
    }

    #[test]
    fn the_same_slot_at_a_new_generation_is_a_different_handle() {
        assert_ne!(BufferId::new(4, 0), BufferId::new(4, 1));
    }

    #[test]
    fn a_handle_is_no_bigger_than_the_two_numbers_in_it() {
        assert_eq!(size_of::<TextureId>(), size_of::<u64>());
        // NonZeroU32 gives the niche, so an optional handle costs nothing
        // extra — the representation the backend's slot tables want.
        assert_eq!(size_of::<Option<TextureId>>(), size_of::<TextureId>());
    }

    #[test]
    fn debug_names_the_resource_kind() {
        assert_eq!(format!("{:?}", PipelineId::new(2, 9)), "pipeline#2v9");
    }

    #[test]
    fn depth_is_distinguishable_from_colour() {
        assert!(TextureFormat::Depth32Float.is_depth());
        assert!(!TextureFormat::Rgba8UnormSrgb.is_depth());
    }
}
