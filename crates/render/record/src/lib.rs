// SPDX-License-Identifier: GPL-2.0-or-later

//! A [`Commands`](krudd_render::Commands) implementation that records every
//! call instead of driving a GPU — the render test oracle #826 asks for.
//!
//! **Tier: `render`.** May reach for `base`, `world` and its own tier, and
//! reaches for nothing else: no wgpu, no `web-sys`. The whole point of this
//! crate is that `cargo test` over whatever drives [`Commands`] stays a real
//! test run rather than a browser harness, and pulling in a GPU dependency
//! here just to never call it would quietly undo that.
//!
//! The old `renderer_null.h` is the direct ancestor: every entry point
//! appended a call record to an in-memory log, a test reset the log, drove
//! the thing under test, and asserted on the exact call sequence — pass
//! culling, execution order, transient lifetimes, barrier emission — with no
//! GPU, no adapter and no flakiness. [`Recorder`] and [`Call`] are that same
//! shape, carried into Rust.
//!
//! ## What this crate is not
//!
//! It is not the frame graph. #823 owns the pass DAG, the transient
//! lifetimes and the automatic barrier emission that will eventually be
//! asserted through this; this crate only owns the recorder those
//! assertions run against and the [`Call`] vocabulary they are asserted in
//! terms of. It also does not implement [`Commands`](krudd_render::Commands)
//! for `krudd-webgl` — that lands only once something drives a real backend
//! through the trait, which is #823's job, not this one's.

mod call;
mod recorder;
mod slots;

pub use call::Call;
pub use recorder::Recorder;
