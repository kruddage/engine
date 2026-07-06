<!--
Before merging, this PR must:
  1. Carry exactly ONE release:* label (enforced by release-label-gate).
  2. Either add a CHANGELOG.md entry under [Unreleased], or be internal-only
     (tests / CI / docs) — mark internal-only PRs "[no changelog]" in the title
     or below (enforced by changelog-gate). See docs/ci-and-release.md.
-->

## What

<!-- What does this change do, and why? -->

## Release label

<!-- Tick the one that matches the label you applied. -->

- [ ] `release:feature` — user-facing feature (minor bump)
- [ ] `release:fix` — user-facing bug fix (patch bump)
- [ ] `release:breaking` — breaking change: ABI, file format, or API (major bump)
- [ ] `release:chore` — internal-only: tests, CI, docs, refactor (no bump)

## Changelog

<!--
Added a bullet under [Unreleased] in CHANGELOG.md? Link/paste it here.
Internal-only PR with no user-visible effect? Write "[no changelog]" and why.
-->

## Related issues

<!-- e.g. Closes #N -->
