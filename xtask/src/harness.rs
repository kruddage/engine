// SPDX-License-Identifier: GPL-2.0-or-later

//! `test-web` and `bench` — the Node harnesses over the built boundary.
//!
//! The Rust half is tested by `cargo test`, which runs on the host because
//! only `krudd-web` knows wasm-bindgen exists. That leaves the boundary
//! itself: the generated glue, the typed-array views over linear memory, and
//! the rule that a view goes stale when memory grows. None of that exists
//! until the wasm is built, and none of it can be asserted from Rust.
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

use std::path::{Path, PathBuf};

use crate::Options;
use crate::sh::{self, Run};
use crate::web;

/// Where the bundled harnesses land. Under `target/` because they are build
/// output, gitignored with the rest of it.
const OUT_DIR: &str = "target/harness";

/// The `node:test` suite over the memory contract.
const TEST_ENTRY: &str = "packages/base/boundary/harness/memory.test.ts";

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
    let bundle = bundle(root, TEST_ENTRY, "memory.test")?;
    node(root)
        .args(["--test".as_ref(), bundle.as_os_str()])
        .check()
}

/// Runs the benchmark against an already-built module.
pub fn run_bench(root: &Path) -> Result<(), String> {
    let bundle = bundle(root, BENCH_ENTRY, "bench")?;
    node(root).args([bundle.as_os_str()]).check()
}

/// A `node` invocation pointed at the wasm the build just produced.
///
/// The path is passed explicitly rather than discovered, so a harness can
/// never quietly run against a module left over from an older build.
fn node(root: &Path) -> Run {
    Run::new("node", root).env("KRUDD_WASM", &web::wasm_path(root).to_string_lossy())
}

/// Bundles one harness entry point into `target/harness/<name>.mjs`.
fn bundle(root: &Path, entry: &str, name: &str) -> Result<PathBuf, String> {
    let out = PathBuf::from(OUT_DIR).join(format!("{name}.mjs"));
    Run::new("pnpm", root)
        .args([
            "exec",
            "esbuild",
            entry,
            "--bundle",
            "--format=esm",
            // Node, so the builtins the harnesses import stay external and
            // `import.meta.url` keeps working in the generated glue.
            "--platform=node",
            "--target=node22",
            "--sourcemap",
            &format!("--outfile={}", out.display()),
        ])
        .check()?;
    Ok(root.join(out))
}
