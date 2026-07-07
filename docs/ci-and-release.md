# CI, Release & Repo Hygiene — `.github/`

This is the as-is map of the repository's automation: the CI gates a pull
request faces, how the version number is derived, how the site deploys, and how
a release is cut. It is a **review artifact** — it describes the machinery as it
stands today so the rest of the consolidation initiative ([#302](https://github.com/kruddage/engine/issues/302))
has a fixed reference to work against. Where the current design is deliberate it
says so; where it is drift-prone it flags the seam rather than fixing it here.

The audit was taken at `VERSION` **5.0.0**. Line references are to the state at
that time and will drift as the files change — treat them as pointers, not
guarantees.

## Workflow inventory

Eight workflows live in `.github/workflows/`. They split into four roles:
the PR/push test matrix, two standalone PR gates, the deploy pipeline, and the
label/release plumbing.

| Workflow | Trigger | Role |
|----------|---------|------|
| `ci.yml` | `pull_request`; `push` to `main` | The main test matrix — build, lints, sanitizers, coverage, clang-tidy, and (PR-only) preview build + deploy. |
| `changelog-gate.yml` | `pull_request` (opened/synchronize/reopened/edited) | Fails a PR that touches non-exempt files without updating `CHANGELOG.md`. |
| `release-label-gate.yml` | `pull_request` (opened/reopened/labeled/unlabeled/synchronize) | Fails a PR unless it carries exactly one `release:*` label. |
| `deploy.yml` | `push` to `main`; `workflow_dispatch` | Builds and stages the Release site, then calls `pages-deploy` in `main` mode. |
| `pages-deploy.yml` | `workflow_call` | The **single writer** to `gh-pages`. Both main deploys and PR previews funnel through it. |
| `sync-labels.yml` | `push` to `main` touching `.github/labels.yml`; `workflow_dispatch` | Applies `labels.yml` to the repo's actual label set. |
| `release.yml` | `push` of a `v*.*.*` tag | Builds Release and creates a GitHub Release with the build artifacts. |
| `auto-merge.yml` | — | **Dead.** The file is 100% commented out; it defines and triggers nothing. |

## The gate matrix a PR faces

All gates below run on `pull_request`. They are the checks a contributor sees on
a PR; whether each one actually *blocks* merge is a separate question — see
[Required checks](#required-checks-the-invisible-set) below.

| Gate | Where | What it enforces |
|------|-------|------------------|
| `build` | `ci.yml` `build` job | WASM Debug build in the pinned emsdk container, then **plugin symbol resolution** (`check-plugin-symbols.mjs`) — a side module whose imports aren't provided becomes a stub that throws at `dlopen`, invisible to a link-only build. |
| `plugin-lint` | `ci.yml` `plugin-lint` | Bans `EM_ASM_*` in `plugins/` — those macros silently break in `SIDE_MODULE=1` builds (runtime throw, no compile/link error). Pure source grep, no toolchain. |
| `malloc-lint` | `ci.yml` `malloc-lint` | Bans raw `malloc`/`free`/`realloc`/`calloc` outside `modules/memory/`, the sole allocator seam. Pure source grep. |
| `sanitizers` | `ci.yml` `sanitizers` | Native ASan/UBSan build, then `ctest`. |
| `coverage` | `ci.yml` `coverage` | Native coverage build, `ctest`, then an lcov line-coverage gate. **Threshold `80` is a bare literal** at `ci.yml:128`. |
| `clang-tidy` | `ci.yml` `clang-tidy` | `run-clang-tidy` over `modules/` + `plugins/` non-test `.c`. |
| `preview-build` / `preview-deploy` | `ci.yml`, PR-only | Release build + Release-config symbol check, staged and published to `pr-preview/pr-<n>/`. Gated on the six jobs above via `needs:`. |
| `changelog-gate` | `changelog-gate.yml` | See below. |
| `release-label-gate` | `release-label-gate.yml` | See below. |

**Tests have no standalone job.** `ctest` runs only as a sub-step of
`sanitizers` and `coverage`; there is no plain "run the tests" gate. If both of
those jobs were ever skipped or restructured, the test suite would stop running
with no obvious signal.

**Native jobs pin the runner image.** `sanitizers`, `coverage`, and `clang-tidy`
run on `ubuntu-24.04` rather than `ubuntu-latest`, and each `apt-get install` is
preceded by `apt-get update`. Pinning the image (rather than pinning individual
apt package versions, which rot as the archive drops old versions) keeps the
native toolchain — and therefore sanitizer behaviour, the coverage percentage,
and which clang-tidy findings fire — deterministic instead of drifting when the
`ubuntu-latest` alias advances.

### Running the source lints locally — `scripts/lint`

The source greps have one entry point, `scripts/lint`, used by both CI and local
runs:

```sh
scripts/lint              # em-asm + raw-malloc (+ symbol check if ./build exists)
scripts/lint em-asm       # ban EM_ASM_* in plugin sources
scripts/lint raw-malloc   # ban raw malloc/free/realloc/calloc outside memory
scripts/lint symbols DIR  # plugin symbol resolution over a build tree
scripts/lint imports DIR  # plugin banned-import check (needs wasm-objdump)
```

CI's `plugin-lint` and `malloc-lint` jobs call `scripts/lint em-asm` /
`scripts/lint raw-malloc` (job names unchanged, so the required-check set is
unaffected). The `imports` check is exposed but **not** part of the default run —
whether it is enforced in CI is still open (#304). Each shell check ships a
`*.test.sh` next to it (`check-plugin-no-em-asm`, `check-no-raw-malloc`,
`check-changelog`, `check-plugin-imports`); those tests run in the matching CI
jobs, so a grep that stops catching a real violation fails a test rather than
shipping.

### The changelog gate

`check-changelog.sh` diffs `BASE_SHA..HEAD_SHA` and fails if any **non-exempt**
file changed without `CHANGELOG.md` also changing. Exempt paths (a regex in
`check-changelog.sh`, pinned by `check-changelog.test.sh` so drift is caught by
a test): `.github/`, `docs/`, `README.md`, `CODING_STANDARD.md`, `CLAUDE.md`,
and test sources (`*_test.c`, `*.test.mjs`, `*.test.sh`).
A PR can also opt out entirely with `[no changelog]` in its title or body
(`check-changelog.sh:24-29`) — an escape hatch that leaves no auditable trace
after merge.

### The release-label gate

`release-label-gate.yml` requires exactly one of `release:feature`,
`release:fix`, `release:breaking`, `release:chore`. The label list is inlined as
JavaScript at `release-label-gate.yml:18-23` **and** defined again in
`.github/labels.yml` — two copies that must be kept in sync by hand.

## The version chain

The version is assembled from four files that must agree:

```
VERSION                       # "5.0.0" — hand-edited, the semver source
  │
  ▼
CMakeLists.txt:3-42           # reads VERSION into project(); derives a build
  │                           # number and short commit hash from git
  ▼
modules/core/version.h.in     # @PROJECT_VERSION_*@ + @ENGINE_BUILD_NUMBER@ →
  │                           # version.h (ENGINE_VERSION_FULL = "x.y.z.N")
  ▼
modules/core/shell.html.in    # <title> and .version span show "x.y.z.N";
                              # @GIT_COMMIT_HASH@ rewrites .wasm cache-bust paths
```

The build number `N` is derived at configure time (`CMakeLists.txt:8-42`):

1. Find the commit that last set the current version string —
   `git log -1 --format=%H -S"${PROJECT_VERSION}" -- VERSION` (the "anchor").
2. Count commits since it — `git rev-list --count HEAD ^<anchor>`.

This is clever but brittle, and the brittleness is the subject of a dedicated
sub-issue ([#306](https://github.com/kruddage/engine/issues/306)):

- The `-S` pickaxe matches on the version **string**, so if a version ever
  recurs in `VERSION`'s history the anchor lands on the wrong commit.
- A no-op edit to `VERSION` (rewriting the same value) moves the anchor.
- On a **shallow clone** the anchor or the count silently comes back wrong —
  the scheme is quietly coupled to every workflow setting `fetch-depth: 0`.

When git is absent or the commands fail, the build degrades to
`ENGINE_BUILD_NUMBER 0` and `GIT_COMMIT_HASH "unknown"` rather than failing.

## Deploy & Pages topology

Publishing to `gh-pages` is deliberately funnelled through a **single writer**,
and this design is explicitly *not* a consolidation target — it is the correct
serialization.

```
deploy.yml  (push to main)          ci.yml preview-build (PR)
   │  build + stage → `site` artifact   │  build + stage → `site` artifact
   └──────────────┬─────────────────────┘
                  ▼
        pages-deploy.yml  (workflow_call)
        concurrency: group=pages, cancel-in-progress=false
                  │
        ┌─────────┴──────────┐
   mode=main               mode=preview
   gh-pages root           pr-preview/pr-<n>/
   keep_files: true        pr-preview-action
```

`pages-deploy.yml` is the only workflow that writes `gh-pages`. Because every
publisher reaches it through `workflow_call` and it holds a single shared
`concurrency` queue (`cancel-in-progress: false`), a main deploy and a PR
preview can never race — they run one at a time, oldest first. `keep_files: true`
on the main publish preserves the `pr-preview/` subtrees so previews and the
main site coexist on one branch.

`stage-site.sh` copies only the web outputs (never the raw `build/` tree, which
contains a `_deps/` gitlink that breaks the Pages branch build) and renames
`index.js` / every `.wasm` with the short commit hash for cache-busting,
patching `index.html` to match.

## Release flow

Cutting a release today is **three manual, hand-coordinated steps**:

1. Edit `VERSION` to the new semver.
2. In `CHANGELOG.md`, rename `## [Unreleased]` → `## [x.y.z] - YYYY-MM-DD` and
   open a fresh empty `[Unreleased]`.
3. Push a `vX.Y.Z` tag, which fires `release.yml`.

`release.yml` builds Release and publishes a GitHub Release via
`softprops/action-gh-release` with `generate_release_notes: true`. This is a
**second, divergent note source**: the GitHub Release notes are regenerated from
merged PR titles, while `CHANGELOG.md`'s `[Unreleased]` is the hand-written,
in-app "What's New" source (baked into the WASM at build time). The two are
reconciled only by discipline.

Nothing consumes the `release:*` labels beyond the PR gate. The label declares
the intended bump (feature→minor, fix→patch, breaking→major, chore→none) but no
workflow reads it to *compute* the next version or drive the cut. Closing that
gap is [#307](https://github.com/kruddage/engine/issues/307).

## Labels

`.github/labels.yml` is the source of truth for the repo's labels;
`sync-labels.yml` applies it on any push to `main` that touches the file.
`skip-delete: true` means the sync **adds and updates but never removes** — a
label created by hand in the UI is never reaped, so the "edit `labels.yml`,
don't click" rule is convention, not enforcement.

## Required checks — the invisible set

Which of the gates above actually **block** merge to `main` is **not represented
in this repository**. The gate-only workflows (`changelog-gate`,
`release-label-gate`) are not in any `needs:` graph, and `ci.yml`'s jobs are not
marked required anywhere in-repo. That decision lives solely in GitHub
branch-protection / ruleset configuration, which is hand-maintained and invisible
to code review: **renaming a CI job can silently drop it from the required set
with no diff.** Documenting and codifying this set is
[#310](https://github.com/kruddage/engine/issues/310); until then, the intended
required set is a matter of convention and should be confirmed against the live
branch-protection settings.

## Known drift-prone seams (audit summary)

Flagged here for reference; each is owned by a sub-issue of #302.

- **Dead / orphaned machinery** — `auto-merge.yml` runs nothing;
  `scripts/check-plugin-imports.sh` (a real WABT banned-import check) is wired to
  no workflow ([#304](https://github.com/kruddage/engine/issues/304)).
- **Duplication & magic constants** — the container-build + `stage-site` +
  "Trust the workspace" + ccache block is copy-pasted across `ci.yml`,
  `deploy.yml`, and `release.yml`; `emscripten/emsdk:6.0.2` is hardcoded in four
  places; the coverage threshold and the `release:*` label list are each defined
  twice ([#305](https://github.com/kruddage/engine/issues/305)).
- **ccache churn** — cache keys are `${{ github.sha }}`, so every commit writes a
  fresh entry; `clang-tidy` has no cache ([#305](https://github.com/kruddage/engine/issues/305)).
- **Brittle version derivation** — the `-S` anchor trick
  ([#306](https://github.com/kruddage/engine/issues/306)).
- **No release automation / two note sources**
  ([#307](https://github.com/kruddage/engine/issues/307)).
- **Scattered lint surface** — the source greps now share the `scripts/lint`
  entry point, each shell check has a `*.test.sh`, the native jobs pin
  `ubuntu-24.04` and `apt-get update` before installing
  ([#308](https://github.com/kruddage/engine/issues/308)). Still open there: the
  inline `github-script` label check and the awk coverage gate remain outside the
  entry point.
- **Missing hygiene** — no `CODEOWNERS`, no `dependabot`, no issue/PR templates;
  third-party actions float on major tags rather than SHAs
  ([#309](https://github.com/kruddage/engine/issues/309)).
- **Invisible required-check set**
  ([#310](https://github.com/kruddage/engine/issues/310)).
