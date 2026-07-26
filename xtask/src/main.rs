// SPDX-License-Identifier: GPL-2.0-or-later

//! `cargo xtask` — the build driver.
//!
//! Cargo compiles Rust. Everything else the build needs — running
//! wasm-bindgen, bundling TypeScript, assembling `dist/`, serving it, and
//! enforcing the tier order — happens here, so there is exactly one entry
//! point and no second build system to keep in sync.
//!
//! It is a plain Rust binary aliased to `cargo xtask` by `.cargo/config.toml`.
//! That keeps "krudd builds krudd" true: the build driver is a first-party
//! program in the same language as the engine, which is the seat the old
//! tree's Scheme `kruddmake` held.

mod check;
mod harness;
mod serve;
mod sh;
mod tiers;
mod web;

use std::process::ExitCode;

const USAGE: &str = "\
cargo xtask — the krudd build driver

USAGE:
    cargo xtask <COMMAND> [OPTIONS]

COMMANDS:
    build-web           Build the wasm module and the TypeScript bundle into dist/
    dist                build-web with release optimisation, and report the sizes
    serve               Build, then serve dist/ over HTTP
    check               Everything CI gates on: fmt, clippy, tests, tiers, typecheck, lint
    test-web            Run the boundary tests against the built wasm, under Node
    bench               Time the batched boundary against the per-call one
    tiers               Check the crate and package tier order on its own
    help                Show this

OPTIONS:
    --release           Optimised build (implied by `dist`)
    --port <PORT>       Port for `serve` (default 8080)
    --host <HOST>       Address for `serve` to bind (default 127.0.0.1)
    --skip-install      Don't run `pnpm install`, even if node_modules is missing
";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(command) = args.first().map(String::as_str) else {
        eprint!("{USAGE}");
        return ExitCode::FAILURE;
    };

    let opts = match Options::parse(&args[1..]) {
        Ok(opts) => opts,
        Err(why) => {
            eprintln!("xtask: {why}");
            return ExitCode::FAILURE;
        }
    };

    let result = match command {
        "build-web" => web::build(&opts).map(|_| ()),
        "dist" => web::dist(&opts),
        "serve" => serve::run(&opts),
        "check" => check::run(&opts),
        "test-web" => harness::test(&opts),
        "bench" => harness::bench(&opts),
        "tiers" => tiers::check(&sh::workspace_root()),
        "help" | "--help" | "-h" => {
            print!("{USAGE}");
            Ok(())
        }
        other => Err(format!("unknown command `{other}`\n\n{USAGE}")),
    };

    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(why) => {
            eprintln!("\nxtask: {why}");
            ExitCode::FAILURE
        }
    }
}

/// Flags shared across commands.
pub struct Options {
    /// Build with optimisation.
    pub release: bool,
    /// Port for `serve`.
    pub port: u16,
    /// Bind address for `serve`.
    pub host: String,
    /// Skip `pnpm install` even when `node_modules` is absent.
    pub skip_install: bool,
}

impl Options {
    fn parse(args: &[String]) -> Result<Self, String> {
        let mut opts = Self {
            release: false,
            port: 8080,
            host: "127.0.0.1".to_string(),
            skip_install: false,
        };
        let mut rest = args.iter();
        while let Some(arg) = rest.next() {
            match arg.as_str() {
                "--release" => opts.release = true,
                "--skip-install" => opts.skip_install = true,
                "--port" => {
                    let value = rest.next().ok_or("--port needs a value")?;
                    opts.port = value
                        .parse()
                        .map_err(|_| format!("`{value}` is not a port number"))?;
                }
                "--host" => {
                    opts.host = rest.next().ok_or("--host needs a value")?.clone();
                }
                other => return Err(format!("unknown option `{other}`")),
            }
        }
        Ok(opts)
    }
}
