// SPDX-License-Identifier: GPL-2.0-or-later

//! The tier order, and the check that keeps it true.
//!
//! "Package based system, avoid API leaks" is the oldest motivation behind
//! the rewrite, and the tier order is how it is enforced structurally rather
//! than by review. A crate or package sits in a tier, the tier is its parent
//! directory, and it may depend only on its own tier or a lower one.
//!
//! The rules the check enforces, both directions:
//!
//! - **No upward dependency.** `base` cannot reach for `world`; `world`
//!   cannot reach for `render`.
//! - **Nothing reaches for a shell.** Stronger than the tier rule, because it
//!   also forbids one shell depending on another: two shells are alternative
//!   hosts, not a layer. There is one today — the web page — and the rule is
//!   stated for the next one rather than for it.
//! - **On-disk layout matches the order**, so the ordering reads without
//!   consulting a list, and every crate/package on disk is a declared
//!   workspace member — a crate the workspace does not know about is neither
//!   built nor checked.
//!
//! See docs/architecture.md for what each tier is for.

use std::collections::BTreeMap;
use std::path::Path;

use crate::sh;

/// The tiers, lowest first. A tier may depend on itself and anything before
/// it.
///
/// Some of these hold nothing yet. They are listed anyway so that the slot a
/// future crate belongs in is already decided — the old tree's mistake was
/// that the ordering existed only in people's heads until someone wrote a
/// manifest.
pub const TIERS: &[&str] = &["base", "world", "render", "audio", "ui", "game", "shell"];

/// The tier that nothing may depend on, in any direction.
const SHELL: &str = "shell";

/// One crate or package, and where it sits.
struct Unit {
    /// The crate or package name, as dependents write it.
    name: String,
    /// Its tier.
    tier: &'static str,
    /// Path relative to the workspace root, for error messages.
    path: String,
    /// The first-party names it depends on.
    deps: Vec<String>,
}

/// Runs the whole check over both trees.
pub fn check(root: &Path) -> Result<(), String> {
    let crates = rust_units(root)?;
    let packages = ts_units(root)?;
    let mut problems = Vec::new();

    check_members(root, &crates)?;
    problems.extend(check_units("crate", &crates));
    problems.extend(check_units("package", &packages));

    if problems.is_empty() {
        println!(
            "tiers: {} crates and {} packages, no upward dependencies",
            crates.len(),
            packages.len()
        );
        return Ok(());
    }
    Err(format!(
        "the tier order is violated:\n  {}\n\nSee docs/architecture.md — a unit may depend on its own tier or a lower one, and nothing may depend on a shell.",
        problems.join("\n  ")
    ))
}

/// Checks the dependency edges of one set of units.
fn check_units(kind: &str, units: &[Unit]) -> Vec<String> {
    let tiers: BTreeMap<&str, &'static str> =
        units.iter().map(|u| (u.name.as_str(), u.tier)).collect();
    let mut problems = Vec::new();

    for unit in units {
        for dep in &unit.deps {
            let Some(&dep_tier) = tiers.get(dep.as_str()) else {
                problems.push(format!(
                    "{kind} `{}` ({}) depends on `{dep}`, which is not a workspace member",
                    unit.name, unit.path
                ));
                continue;
            };
            if dep_tier == SHELL {
                problems.push(format!(
                    "{kind} `{}` ({}) depends on `{dep}`, a shell — nothing may depend on a shell",
                    unit.name, unit.path
                ));
                continue;
            }
            if rank(dep_tier) > rank(unit.tier) {
                problems.push(format!(
                    "{kind} `{}` (tier {}) depends on `{dep}` (tier {dep_tier}) — that is upward",
                    unit.name, unit.tier
                ));
            }
        }
    }
    problems
}

/// Where a tier sits in the order.
fn rank(tier: &str) -> usize {
    TIERS
        .iter()
        .position(|t| *t == tier)
        .expect("units are only built for known tiers")
}

/// Every crate under `crates/<tier>/<name>/`.
fn rust_units(root: &Path) -> Result<Vec<Unit>, String> {
    let mut units = Vec::new();
    for (tier, dir) in tier_dirs(&root.join("crates"))? {
        let manifest_path = dir.join("Cargo.toml");
        let manifest = sh::read(&manifest_path)?;
        let name = sh::toml_string(&manifest, "package", "name").ok_or_else(|| {
            format!(
                "{}: no `name` under [package] — xtask cannot place it in a tier",
                manifest_path.display()
            )
        })?;
        let deps = sh::toml_keys(
            &manifest,
            &["dependencies", "dev-dependencies", "build-dependencies"],
        )
        .into_iter()
        .filter(|d| d.starts_with("krudd-"))
        .collect();
        units.push(Unit {
            name,
            tier,
            path: relative(root, &dir),
            deps,
        });
    }
    Ok(units)
}

/// Every package under `packages/<tier>/<name>/`.
fn ts_units(root: &Path) -> Result<Vec<Unit>, String> {
    let mut units = Vec::new();
    for (tier, dir) in tier_dirs(&root.join("packages"))? {
        let manifest_path = dir.join("package.json");
        let manifest = sh::read(&manifest_path)?;
        let name = json_string(&manifest, "name").ok_or_else(|| {
            format!(
                "{}: no `name` — xtask cannot place it in a tier",
                manifest_path.display()
            )
        })?;
        units.push(Unit {
            name,
            tier,
            path: relative(root, &dir),
            deps: workspace_deps(&manifest),
        });
    }
    Ok(units)
}

/// The `<tier>/<name>` directories under `parent`, skipping anything not in a
/// known tier.
fn tier_dirs(parent: &Path) -> Result<Vec<(&'static str, std::path::PathBuf)>, String> {
    let mut found = Vec::new();
    for tier in TIERS {
        let tier_dir = parent.join(tier);
        if !tier_dir.is_dir() {
            continue;
        }
        let entries =
            std::fs::read_dir(&tier_dir).map_err(|e| format!("{}: {e}", tier_dir.display()))?;
        let mut dirs: Vec<_> = entries
            .filter_map(Result::ok)
            .map(|e| e.path())
            .filter(|p| p.is_dir())
            .collect();
        // Sorted so the report reads the same on every machine.
        dirs.sort();
        found.extend(dirs.into_iter().map(|d| (*tier, d)));
    }
    Ok(found)
}

/// Asserts every crate on disk is a declared workspace member, and every
/// declared member exists.
///
/// A crate missing from `members` compiles when something depends on it and
/// is silently skipped by `cargo test --workspace` otherwise — exactly the
/// drift the old tree's manifest existed to prevent.
fn check_members(root: &Path, crates: &[Unit]) -> Result<(), String> {
    let manifest = sh::read(&root.join("Cargo.toml"))?;
    let declared: Vec<String> = toml_array(&manifest, "members");
    let mut problems = Vec::new();

    for unit in crates {
        if !declared.contains(&unit.path) {
            problems.push(format!(
                "crate `{}` at {} is not in the workspace `members` list",
                unit.name, unit.path
            ));
        }
    }
    for member in &declared {
        if !root.join(member).join("Cargo.toml").is_file() {
            problems.push(format!(
                "workspace member `{member}` has no Cargo.toml — stale entry?"
            ));
        }
    }
    if problems.is_empty() {
        Ok(())
    } else {
        Err(problems.join("\n  "))
    }
}

/// The string entries of a `key = [ ... ]` array in a first-party manifest.
fn toml_array(manifest: &str, key: &str) -> Vec<String> {
    let mut values = Vec::new();
    let mut collecting = false;
    for line in manifest.lines() {
        let line = line.trim();
        if !collecting {
            let Some(rest) = line.strip_prefix(key) else {
                continue;
            };
            let rest = rest.trim_start();
            if !rest.starts_with('=') {
                continue;
            }
            collecting = true;
            continue;
        }
        if line.starts_with(']') {
            break;
        }
        if let Some(value) = line.trim_end_matches(',').strip_prefix('"') {
            values.push(value.trim_end_matches('"').to_string());
        }
    }
    values
}

/// The value of a top-level `"key": "value"` in a first-party `package.json`.
///
/// Same trade as [`sh::toml_string`]: a line scanner over manifests we own,
/// rather than a JSON dependency in the one crate that must build first.
fn json_string(manifest: &str, key: &str) -> Option<String> {
    let needle = format!("\"{key}\"");
    for line in manifest.lines() {
        let line = line.trim();
        let Some(rest) = line.strip_prefix(&needle) else {
            continue;
        };
        let rest = rest.trim_start().strip_prefix(':')?.trim();
        return Some(rest.trim_end_matches(',').trim_matches('"').to_string());
    }
    None
}

/// The `@krudd/*` packages a `package.json` depends on.
///
/// Workspace deps are written `"@krudd/x": "workspace:*"`, so the protocol is
/// the marker — a registry dependency that happened to share the scope would
/// not match, which is the correct answer.
fn workspace_deps(manifest: &str) -> Vec<String> {
    manifest
        .lines()
        .filter_map(|line| {
            let line = line.trim();
            let (name, spec) = line.split_once(':')?;
            let name = name.trim().trim_matches('"');
            if !name.starts_with("@krudd/") || !spec.contains("workspace:") {
                return None;
            }
            Some(name.to_string())
        })
        .collect()
}

/// A path relative to the workspace root, with forward slashes, so it matches
/// what a manifest writes.
fn relative(root: &Path, path: &Path) -> String {
    path.strip_prefix(root)
        .unwrap_or(path)
        .to_string_lossy()
        .replace('\\', "/")
}
