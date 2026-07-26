// SPDX-License-Identifier: GPL-2.0-or-later

//! Running programs, finding the workspace, and reading first-party
//! manifests.

use std::path::{Path, PathBuf};
use std::process::Command;

/// The workspace root — the directory holding the root `Cargo.toml`.
///
/// Derived from where this crate's manifest is rather than from the current
/// directory, so `cargo xtask` behaves the same from any subdirectory.
pub fn workspace_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("xtask/ always has a parent")
        .to_path_buf()
}

/// The version to stamp into the build.
///
/// `version.txt` is the released version, maintained by release-please.
/// `KRUDD_VERSION` overrides it so a CI preview build can mark itself as one
/// and never be mistaken for a release.
pub fn version(root: &Path) -> String {
    if let Ok(v) = std::env::var("KRUDD_VERSION")
        && !v.is_empty()
    {
        return v;
    }
    std::fs::read_to_string(root.join("version.txt"))
        .map(|s| s.trim().to_string())
        .unwrap_or_else(|_| "dev".to_string())
}

/// A command to run, built up and then executed.
pub struct Run {
    command: Command,
    label: String,
}

impl Run {
    /// A command run from the workspace root.
    pub fn new(program: &str, root: &Path) -> Self {
        let mut command = Command::new(program);
        command.current_dir(root);
        Self {
            command,
            label: program.to_string(),
        }
    }

    /// The `cargo` that invoked this xtask, so a `+toolchain` override or a
    /// non-default rustup install is not silently dropped.
    pub fn cargo(root: &Path) -> Self {
        let cargo = std::env::var("CARGO").unwrap_or_else(|_| "cargo".to_string());
        let mut run = Self::new(&cargo, root);
        run.label = "cargo".to_string();
        run
    }

    /// Adds arguments.
    pub fn args<I: IntoIterator<Item = S>, S: AsRef<std::ffi::OsStr>>(mut self, args: I) -> Self {
        self.command.args(args);
        self
    }

    /// Sets an environment variable.
    pub fn env(mut self, key: &str, value: &str) -> Self {
        self.command.env(key, value);
        self
    }

    /// Runs it, inheriting stdio, and fails if it exits non-zero.
    pub fn check(mut self) -> Result<(), String> {
        let shown = self.shown();
        println!("\x1b[1;34m>\x1b[0m {shown}");
        let status = self.command.status().map_err(|e| {
            format!(
                "could not run `{}`: {e}{}",
                self.label,
                install_hint(&self.label)
            )
        })?;
        if status.success() {
            Ok(())
        } else {
            Err(format!("`{shown}` failed ({status})"))
        }
    }

    /// Runs it and captures stdout, without echoing the command.
    pub fn capture(mut self) -> Result<String, String> {
        let output = self.command.output().map_err(|e| {
            format!(
                "could not run `{}`: {e}{}",
                self.label,
                install_hint(&self.label)
            )
        })?;
        if !output.status.success() {
            return Err(format!("`{}` failed ({})", self.shown(), output.status));
        }
        Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
    }

    fn shown(&self) -> String {
        let args: Vec<String> = self
            .command
            .get_args()
            .map(|a| a.to_string_lossy().into_owned())
            .collect();
        format!("{} {}", self.label, args.join(" "))
            .trim_end()
            .to_string()
    }
}

/// A hint pointing at how to get a missing tool, so a fresh checkout gets a
/// pointer rather than "No such file or directory".
fn install_hint(program: &str) -> &'static str {
    match program {
        "pnpm" => {
            "\n  pnpm is the package manager for the TypeScript half — install it with `npm install -g pnpm` or `corepack enable pnpm` (see docs/toolchain.md)"
        }
        "wasm-bindgen" => {
            "\n  run `cargo xtask build-web` again; xtask installs the matching wasm-bindgen CLI itself"
        }
        _ => "",
    }
}

/// Reads a file, reporting the path in the error rather than just the errno.
pub fn read(path: &Path) -> Result<String, String> {
    std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))
}

/// Writes a file, creating parent directories.
pub fn write(path: &Path, contents: &str) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
    }
    std::fs::write(path, contents).map_err(|e| format!("{}: {e}", path.display()))
}

/// Copies a file, creating parent directories.
pub fn copy(from: &Path, to: &Path) -> Result<(), String> {
    if let Some(parent) = to.parent() {
        std::fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
    }
    std::fs::copy(from, to)
        .map(|_| ())
        .map_err(|e| format!("{} -> {}: {e}", from.display(), to.display()))
}

/// The value of a `key = "value"` line in a first-party TOML manifest.
///
/// A deliberately naive scanner rather than a TOML dependency: it only ever
/// reads manifests in this repository, whose shape is ours to keep simple,
/// and xtask having no dependencies is worth more than generality here. It
/// stops at the first match, which is why callers pass the section they mean.
pub fn toml_string(manifest: &str, section: &str, key: &str) -> Option<String> {
    let mut in_section = false;
    for line in manifest.lines() {
        let line = line.trim();
        if let Some(name) = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
            in_section = name == section;
            continue;
        }
        if !in_section {
            continue;
        }
        let Some((found, value)) = line.split_once('=') else {
            continue;
        };
        if found.trim() == key {
            return Some(value.trim().trim_matches('"').to_string());
        }
    }
    None
}

/// The keys declared in the given TOML sections, in order.
pub fn toml_keys(manifest: &str, sections: &[&str]) -> Vec<String> {
    let mut keys = Vec::new();
    let mut in_section = false;
    for line in manifest.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if let Some(name) = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
            in_section = sections.contains(&name);
            continue;
        }
        if !in_section {
            continue;
        }
        if let Some((key, _)) = line.split_once('=') {
            // `foo.workspace = true` and `foo = { ... }` both name crate
            // `foo`; take the part before the first dot.
            let key = key.trim().split('.').next().unwrap_or("").trim();
            if !key.is_empty() {
                keys.push(key.to_string());
            }
        }
    }
    keys
}
