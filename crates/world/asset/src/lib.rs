// SPDX-License-Identifier: GPL-2.0-or-later

//! Asset decoding: bytes in, engine data out.
//!
//! **Tier: `world`.** May reach for `base`. Decoding is Rust's half of the
//! split because it is the hot, allocation-heavy path — a texture or a mesh
//! decoded in TypeScript would be GC pressure measured in megabytes per load.
//!
//! ## Scope
//!
//! What is here is the vocabulary every codec shares: the [`Decode`] trait,
//! the error type, and the rule that a codec never panics on hostile input.
//! Real codecs, and the parameterised-asset `params` / `edit` vocabulary
//! carried from the old tree, are #825.

use core::fmt;

/// Why a decode failed.
///
/// Deliberately a closed enum rather than a string: an asset that fails to
/// load is a thing the editor has to show the author, and "which of these
/// four" is a decision the UI can make. Codecs that need to say more attach
/// it to [`DecodeError::Malformed`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum DecodeError {
    /// The bytes ran out before the decoder had a complete asset.
    ///
    /// Distinct from [`DecodeError::Malformed`] because it is the one failure
    /// that a retry can fix: a truncated fetch is a network problem, not a
    /// broken asset.
    Truncated,
    /// The header did not identify an asset this codec decodes.
    UnknownFormat,
    /// The format was recognised but the contents did not hold together.
    Malformed(&'static str),
    /// The asset is a version of the format this codec does not implement.
    UnsupportedVersion(u32),
}

impl fmt::Display for DecodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Truncated => write!(f, "asset data ended early"),
            Self::UnknownFormat => write!(f, "not a format this codec decodes"),
            Self::Malformed(why) => write!(f, "malformed asset: {why}"),
            Self::UnsupportedVersion(v) => write!(f, "unsupported format version {v}"),
        }
    }
}

impl core::error::Error for DecodeError {}

/// The result of a decode.
pub type Result<T> = core::result::Result<T, DecodeError>;

/// A codec: one asset format, decoded from a byte slice.
///
/// Implementors must treat the input as hostile. Assets arrive over the
/// network and out of project directories the engine did not write, so a
/// codec that panics on a bad length field takes the whole wasm module with
/// it — there is no process boundary to contain it. Every failure is a
/// [`DecodeError`], never a panic and never an out-of-bounds index.
pub trait Decode: Sized {
    /// A short name for the format, for logs and error messages.
    const FORMAT: &'static str;

    /// Whether these bytes look like this format, cheaply — a magic-number
    /// check, not a parse.
    ///
    /// Used to pick a codec before committing to a decode. A `true` here does
    /// not promise [`Decode::decode`] will succeed.
    fn sniff(bytes: &[u8]) -> bool;

    /// Decodes the asset.
    fn decode(bytes: &[u8]) -> Result<Self>;
}

/// A cursor over asset bytes that returns [`DecodeError::Truncated`] instead
/// of panicking when a read runs off the end.
///
/// Every codec reads through this rather than indexing the slice directly.
/// That is the mechanism behind the no-panic rule above: the bounds check
/// exists once, here, instead of at every read site where it can be
/// forgotten.
pub struct Reader<'a> {
    bytes: &'a [u8],
    offset: usize,
}

impl<'a> Reader<'a> {
    /// A reader positioned at the start of `bytes`.
    pub fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, offset: 0 }
    }

    /// How many bytes are left unread.
    pub fn remaining(&self) -> usize {
        self.bytes.len() - self.offset
    }

    /// Reads `n` bytes.
    pub fn take(&mut self, n: usize) -> Result<&'a [u8]> {
        let end = self.offset.checked_add(n).ok_or(DecodeError::Truncated)?;
        let slice = self
            .bytes
            .get(self.offset..end)
            .ok_or(DecodeError::Truncated)?;
        self.offset = end;
        Ok(slice)
    }

    /// Reads a little-endian `u32`.
    ///
    /// Little-endian throughout: it is wasm's native order and the browser's,
    /// so a decode is a load rather than a byte swap.
    pub fn u32_le(&mut self) -> Result<u32> {
        let bytes: [u8; 4] = self.take(4)?.try_into().expect("take returned 4 bytes");
        Ok(u32::from_le_bytes(bytes))
    }

    /// Reads a little-endian `f32`.
    pub fn f32_le(&mut self) -> Result<f32> {
        Ok(f32::from_bits(self.u32_le()?))
    }

    /// Reads a [`krudd_math::Vec3`] as three little-endian `f32`s.
    pub fn vec3_le(&mut self) -> Result<krudd_math::Vec3> {
        Ok(krudd_math::Vec3::new(
            self.f32_le()?,
            self.f32_le()?,
            self.f32_le()?,
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reading_past_the_end_is_an_error_not_a_panic() {
        let mut r = Reader::new(&[1, 2, 3]);
        assert_eq!(r.u32_le(), Err(DecodeError::Truncated));
    }

    #[test]
    fn a_length_field_that_overflows_usize_is_truncated_not_a_wrap() {
        // The classic hostile-input shape: a huge length that, added to the
        // offset, wraps to something in range.
        let mut r = Reader::new(&[0; 8]);
        assert_eq!(r.take(usize::MAX), Err(DecodeError::Truncated));
    }

    #[test]
    fn reads_are_little_endian() {
        let mut r = Reader::new(&[0x78, 0x56, 0x34, 0x12]);
        assert_eq!(r.u32_le(), Ok(0x1234_5678));
    }

    #[test]
    fn a_failed_read_does_not_advance_the_cursor() {
        let mut r = Reader::new(&[1, 2, 3]);
        assert!(r.u32_le().is_err());
        assert_eq!(r.remaining(), 3);
        assert_eq!(r.take(3), Ok(&[1u8, 2, 3][..]));
    }

    #[test]
    fn vec3_reads_three_floats_in_order() {
        let mut bytes = Vec::new();
        for f in [1.0f32, 2.0, 3.0] {
            bytes.extend_from_slice(&f.to_le_bytes());
        }
        let mut r = Reader::new(&bytes);
        assert_eq!(r.vec3_le(), Ok(krudd_math::Vec3::new(1.0, 2.0, 3.0)));
        assert_eq!(r.remaining(), 0);
    }

    #[test]
    fn errors_render_a_human_readable_reason() {
        assert_eq!(
            DecodeError::UnsupportedVersion(7).to_string(),
            "unsupported format version 7"
        );
    }
}
