// SPDX-License-Identifier: GPL-2.0-or-later

//! Arithmetic and the spatial types.
//!
//! **Tier: `base`.** No engine concepts live here — no entities, no GPU
//! resources, no frames. The spatial types are geometry rather than world
//! data, which is what lets `base` sit strictly below `world`. This crate
//! depends on nothing.
//!
//! Everything is `f32` and `#[repr(C)]`: these types are written into buffers
//! that both the GPU and the TypeScript half read as flat typed arrays, so the
//! layout is part of the contract, not an implementation detail.

/// A 2-component vector.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct Vec2 {
    /// The x component.
    pub x: f32,
    /// The y component.
    pub y: f32,
}

impl Vec2 {
    /// The zero vector.
    pub const ZERO: Self = Self { x: 0.0, y: 0.0 };

    /// Builds a vector from its components.
    pub const fn new(x: f32, y: f32) -> Self {
        Self { x, y }
    }

    /// The dot product of two vectors.
    pub fn dot(self, rhs: Self) -> f32 {
        self.x * rhs.x + self.y * rhs.y
    }

    /// The euclidean length of the vector.
    pub fn length(self) -> f32 {
        self.dot(self).sqrt()
    }
}

/// A 3-component vector.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct Vec3 {
    /// The x component.
    pub x: f32,
    /// The y component.
    pub y: f32,
    /// The z component.
    pub z: f32,
}

impl Vec3 {
    /// The zero vector.
    pub const ZERO: Self = Self {
        x: 0.0,
        y: 0.0,
        z: 0.0,
    };

    /// Builds a vector from its components.
    pub const fn new(x: f32, y: f32, z: f32) -> Self {
        Self { x, y, z }
    }

    /// The dot product of two vectors.
    pub fn dot(self, rhs: Self) -> f32 {
        self.x * rhs.x + self.y * rhs.y + self.z * rhs.z
    }

    /// The cross product of two vectors.
    pub fn cross(self, rhs: Self) -> Self {
        Self::new(
            self.y * rhs.z - self.z * rhs.y,
            self.z * rhs.x - self.x * rhs.z,
            self.x * rhs.y - self.y * rhs.x,
        )
    }

    /// The euclidean length of the vector.
    pub fn length(self) -> f32 {
        self.dot(self).sqrt()
    }

    /// The vector scaled to unit length, or [`Vec3::ZERO`] if it has no
    /// length to normalise.
    ///
    /// Returning zero rather than `NaN` is deliberate: a degenerate normal
    /// that renders as nothing is diagnosable, and one that poisons every
    /// downstream multiply is not.
    pub fn normalized(self) -> Self {
        let len = self.length();
        if len == 0.0 {
            Self::ZERO
        } else {
            Self::new(self.x / len, self.y / len, self.z / len)
        }
    }
}

/// A 4x4 matrix, stored **column-major** — the order both GLSL and WGSL
/// expect, so a matrix can be handed to a uniform buffer without a transpose.
///
/// `cols[c][r]` is the element in column `c`, row `r`.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct Mat4 {
    /// The four columns, each a column of four rows.
    pub cols: [[f32; 4]; 4],
}

impl Default for Mat4 {
    fn default() -> Self {
        Self::IDENTITY
    }
}

impl Mat4 {
    /// The identity matrix.
    pub const IDENTITY: Self = Self {
        cols: [
            [1.0, 0.0, 0.0, 0.0],
            [0.0, 1.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [0.0, 0.0, 0.0, 1.0],
        ],
    };

    /// A translation matrix.
    pub const fn from_translation(t: Vec3) -> Self {
        let mut m = Self::IDENTITY;
        m.cols[3] = [t.x, t.y, t.z, 1.0];
        m
    }

    /// A non-uniform scale matrix.
    pub const fn from_scale(s: Vec3) -> Self {
        let mut m = Self::IDENTITY;
        m.cols[0][0] = s.x;
        m.cols[1][1] = s.y;
        m.cols[2][2] = s.z;
        m
    }

    /// An orthographic projection mapping the box `left..right`,
    /// `bottom..top`, `near..far` onto clip space, with `x` and `y` in
    /// `-1..1` and depth in `0..1`.
    ///
    /// Depth is `0..1` rather than OpenGL's `-1..1` because WebGPU, Vulkan and
    /// Direct3D all use it and only GL does not — so the one backend that
    /// needs the other convention converts, rather than every matrix the
    /// engine builds carrying GL's.
    pub fn orthographic(left: f32, right: f32, bottom: f32, top: f32, near: f32, far: f32) -> Self {
        let mut m = Self::IDENTITY;
        m.cols[0][0] = 2.0 / (right - left);
        m.cols[1][1] = 2.0 / (top - bottom);
        m.cols[2][2] = 1.0 / (far - near);
        m.cols[3] = [
            (left + right) / (left - right),
            (top + bottom) / (bottom - top),
            -near / (far - near),
            1.0,
        ];
        m
    }

    /// The matrix product `self * rhs` — applies `rhs` first.
    pub fn mul(&self, rhs: &Self) -> Self {
        let mut out = [[0.0f32; 4]; 4];
        for (c, col) in out.iter_mut().enumerate() {
            for (r, cell) in col.iter_mut().enumerate() {
                *cell = (0..4).map(|k| self.cols[k][r] * rhs.cols[c][k]).sum();
            }
        }
        Self { cols: out }
    }

    /// The matrix as a flat column-major slice, ready for a uniform upload.
    pub fn as_slice(&self) -> &[f32; 16] {
        // SAFETY: `Mat4` is `#[repr(C)]` around a `[[f32; 4]; 4]`, which has
        // the same size, alignment and layout as `[f32; 16]`.
        unsafe { &*(self.cols.as_ptr().cast::<[f32; 16]>()) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vec3_cross_is_right_handed() {
        let x = Vec3::new(1.0, 0.0, 0.0);
        let y = Vec3::new(0.0, 1.0, 0.0);
        assert_eq!(x.cross(y), Vec3::new(0.0, 0.0, 1.0));
    }

    #[test]
    fn normalizing_a_zero_vector_yields_zero_not_nan() {
        assert_eq!(Vec3::ZERO.normalized(), Vec3::ZERO);
    }

    #[test]
    fn vec2_length_is_euclidean() {
        assert_eq!(Vec2::new(3.0, 4.0).length(), 5.0);
    }

    #[test]
    fn identity_is_the_multiplicative_unit() {
        let m = Mat4::from_translation(Vec3::new(1.0, 2.0, 3.0));
        assert_eq!(m.mul(&Mat4::IDENTITY), m);
        assert_eq!(Mat4::IDENTITY.mul(&m), m);
    }

    #[test]
    fn translation_composes_by_addition() {
        let a = Mat4::from_translation(Vec3::new(1.0, 2.0, 3.0));
        let b = Mat4::from_translation(Vec3::new(4.0, 5.0, 6.0));
        assert_eq!(a.mul(&b), Mat4::from_translation(Vec3::new(5.0, 7.0, 9.0)));
    }

    #[test]
    fn scale_then_translate_does_not_scale_the_translation() {
        let t = Mat4::from_translation(Vec3::new(1.0, 0.0, 0.0));
        let s = Mat4::from_scale(Vec3::new(2.0, 2.0, 2.0));
        assert_eq!(t.mul(&s).cols[3], [1.0, 0.0, 0.0, 1.0]);
    }

    #[test]
    fn orthographic_maps_the_box_corners_onto_clip_space() {
        let m = Mat4::orthographic(-2.0, 2.0, -1.0, 1.0, 1.0, 11.0);
        let project = |p: [f32; 4]| -> Vec<f32> {
            (0..4)
                .map(|r| (0..4).map(|k| m.cols[k][r] * p[k]).sum())
                .collect()
        };
        let close = |got: f32, want: f32| assert!((got - want).abs() < 1e-6, "{got} != {want}");

        // The far top-right corner lands at (1, 1, 1)...
        let far = project([2.0, 1.0, 11.0, 1.0]);
        close(far[0], 1.0);
        close(far[1], 1.0);
        close(far[2], 1.0);

        // ...the near bottom-left one at (-1, -1, 0). Depth is 0..1, not
        // -1..1: a near plane that mapped to -1 would be GL's convention.
        let near = project([-2.0, -1.0, 1.0, 1.0]);
        close(near[0], -1.0);
        close(near[1], -1.0);
        close(near[2], 0.0);
    }

    #[test]
    fn as_slice_is_column_major() {
        let m = Mat4::from_translation(Vec3::new(7.0, 8.0, 9.0));
        assert_eq!(&m.as_slice()[12..16], &[7.0, 8.0, 9.0, 1.0]);
    }
}
