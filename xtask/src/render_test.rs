// SPDX-License-Identifier: GPL-2.0-or-later

//! `render-test` — the screenshot-and-compare harness against the built page.
//!
//! Deliberately **not** part of `cargo xtask check`. `check` is the merge
//! gate, and a contributor runs it locally; launching a real browser inside
//! it would make a green local run depend on having a working display. This
//! is its own subcommand and its own CI step instead, the same shape `bench`
//! already uses in `ci.yml` for the same reason — a number (or here, a
//! picture) that is expensive or environment-sensitive to produce does not
//! belong in the gate everyone runs on every save.
//!
//! Everything about *how* the page is driven — the DevTools Protocol
//! transport, the deterministic virtual clock, the `xvfb-run` re-exec, the
//! pixel comparison, the tolerance — lives in the TypeScript under
//! `packages/shell/web/harness/`; see `render-test.ts`'s header for that
//! half of the story. This module only builds the site, serves it on a
//! loopback port the way `cargo xtask serve` does, and hands the driver the
//! URL — the same division of labour `harness.rs` already has with the
//! boundary tests.

use crate::Options;
use crate::serve;
use crate::sh::{self, Run};
use crate::web;

/// The driver's entry point.
const ENTRY: &str = "packages/shell/web/harness/render-test.ts";

/// Builds, serves `dist/`, and runs the screenshot driver against it.
///
/// Not forced to `--release`, unlike `bench`: what is under test is *what
/// the page draws*, which does not depend on optimisation level, so there is
/// no reason to pay for an LTO build here as well as the one `bench` already
/// forces and the one `dist` produces at the end of CI.
pub fn run(opts: &Options) -> Result<(), String> {
    let root = sh::workspace_root();
    let artifacts = web::build(opts)?;

    // An OS-assigned loopback port, not the fixed default `cargo xtask serve`
    // uses: this must never collide with a contributor's own dev server, or
    // with anything else already bound on a CI runner.
    let addr = serve::spawn(artifacts.dist, "127.0.0.1:0")?;

    let bundle = sh::bundle_ts(&root, ENTRY, "render-test")?;
    Run::new("node", &root)
        .args([bundle.as_os_str()])
        .env("KRUDD_BASE_URL", &format!("http://{addr}/"))
        .check()
}
