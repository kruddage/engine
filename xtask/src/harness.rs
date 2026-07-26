// SPDX-License-Identifier: GPL-2.0-or-later

//! `test-web` and `bench` — the Node harnesses over the TypeScript half.
//!
//! The Rust half is tested by `cargo test`, which runs on the host because
//! only `krudd-web` knows wasm-bindgen exists. That leaves everything written
//! in TypeScript: the boundary — the generated glue, the typed-array views
//! over linear memory, and the rule that a view goes stale when memory grows
//! — and the packages above it. None of the boundary exists until the wasm is
//! built, and none of it can be asserted from Rust.
//!
//! So these run the real module — the one `cargo xtask build-web` produced,
//! with the real glue and the real `@krudd/boundary` — under Node. Node
//! rather than a browser because nothing here touches a canvas or the DOM;
//! when something does, that is a browser harness and a different problem
//! (#818, #820).
//!
//! Both are bundled by esbuild first, for the same reason the page is: the
//! harnesses are TypeScript importing a package by its `exports` map, and
//! bundling is what resolves that. `node --experimental-strip-types` would
//! run the files but would not resolve the imports.

use std::path::Path;

use crate::Options;
use crate::sh::{self, Run};
use crate::web;

/// The `node:test` suites — one entry per contract, each bundled separately
/// because esbuild bundles an entry point and `node --test` runs files.
///
/// Most of them do not need the wasm at all — the board document, the
/// interpreter and the mode track are plain TypeScript and would each run
/// under `node --experimental-strip-types` on its own. They are listed with
/// the rest because one command that runs every TypeScript test is worth more
/// than a second command nobody remembers.
const TEST_ENTRIES: [(&str, &str); 6] = [
    (
        "packages/base/boundary/harness/memory.test.ts",
        "memory.test",
    ),
    (
        "packages/base/boundary/harness/viewport.test.ts",
        "viewport.test",
    ),
    (
        "packages/world/board/harness/document.test.ts",
        "document.test",
    ),
    (
        "packages/world/board/harness/validate.test.ts",
        "validate.test",
    ),
    ("packages/world/board/harness/run.test.ts", "run.test"),
    ("packages/shell/web/harness/track.test.ts", "track.test"),
];

/// The batched-versus-per-call benchmark.
const BENCH_ENTRY: &str = "packages/base/boundary/harness/bench.ts";

/// Builds, then runs the boundary tests.
pub fn test(opts: &Options) -> Result<(), String> {
    let root = sh::workspace_root();
    web::build(opts)?;
    run_tests(&root)
}

/// Builds, then runs the benchmark.
///
/// Always a release build, whatever was asked for. The batched path is a walk
/// over a typed array and does not care how Rust was compiled; the per-call
/// path is Rust code and cares enormously. Timing a debug build would flatter
/// the ratio by measuring an engine nobody ships.
pub fn bench(opts: &Options) -> Result<(), String> {
    let root = sh::workspace_root();
    web::build(&Options {
        release: true,
        port: opts.port,
        host: opts.host.clone(),
        skip_install: opts.skip_install,
    })?;
    run_bench(&root)
}

/// Runs the boundary tests against an already-built module.
pub fn run_tests(root: &Path) -> Result<(), String> {
    let mut args = vec![std::ffi::OsString::from("--test")];
    for (entry, name) in TEST_ENTRIES {
        args.push(sh::bundle_ts(root, entry, name)?.into_os_string());
    }
    node(root).args(args).check()
}

/// Runs the benchmark against an already-built module.
pub fn run_bench(root: &Path) -> Result<(), String> {
    let bundle = sh::bundle_ts(root, BENCH_ENTRY, "bench")?;
    node(root).args([bundle.as_os_str()]).check()
}

/// A `node` invocation pointed at the wasm the build just produced.
///
/// The path is passed explicitly rather than discovered, so a harness can
/// never quietly run against a module left over from an older build.
fn node(root: &Path) -> Run {
    Run::new("node", root).env("KRUDD_WASM", &web::wasm_path(root).to_string_lossy())
}
