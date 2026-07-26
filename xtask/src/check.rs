// SPDX-License-Identifier: GPL-2.0-or-later

//! `check` — everything CI gates on, in one command.
//!
//! CI runs this and nothing else. That is the point: a gate that only exists
//! in a workflow file is a gate nobody can run before pushing, and the old
//! tree accumulated several. If it fails here it fails there, and the other
//! way round.
//!
//! The build runs first because the TypeScript half typechecks against
//! wasm-bindgen's generated `.d.ts`, which does not exist until the wasm is
//! built.

use crate::Options;
use crate::harness;
use crate::sh::{self, Run};
use crate::tiers;
use crate::web;

/// Runs the full gate.
pub fn run(opts: &Options) -> Result<(), String> {
    let root = sh::workspace_root();

    // Structure first: a tier violation makes every failure below it noise.
    tiers::check(&root)?;

    Run::cargo(&root)
        .args(["fmt", "--all", "--check"])
        .check()?;
    Run::cargo(&root)
        .args([
            "clippy",
            "--workspace",
            "--all-targets",
            "--",
            "--deny",
            "warnings",
        ])
        .check()?;
    Run::cargo(&root).args(["test", "--workspace"]).check()?;

    // The wasm build, which also regenerates the glue the TS half imports.
    web::build(opts)?;

    // Plain `tsc`: the root tsconfig.json sets `noEmit`, so this only ever
    // typechecks. esbuild is the only thing that emits.
    Run::new("pnpm", &root).args(["exec", "tsc"]).check()?;
    // The harnesses again, against the Node config — they are the only
    // TypeScript here that is not browser code, and the split is what keeps
    // `process` and `node:fs` unreachable from everything that is.
    Run::new("pnpm", &root)
        .args(["exec", "tsc", "-p", "tsconfig.harness.json"])
        .check()?;
    Run::new("pnpm", &root)
        .args(["exec", "biome", "ci", "."])
        .check()?;

    // Last, because it is the only gate that runs the artifact rather than
    // reading it: the memory contract holds or it does not.
    harness::run_tests(&root)?;

    println!("\nall checks passed");
    Ok(())
}
