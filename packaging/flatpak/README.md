<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Flatpak packaging (build-only, proof of life)

Tracking issue: [#686](https://github.com/kruddage/engine/issues/686) —
"Distribute the native editor as a Flatpak from a self-hosted registry".

This directory holds the `flatpak-builder` manifest for `editor-qt`
(`krudd_qt`, the Qt editor shell — see [`docs/qt-editor-shell.md`](../../docs/qt-editor-shell.md))
and the CI job (`.github/workflows/flatpak-build.yml`) that builds it. What's
here today is deliberately narrower than #686's end state:

- **Build-only.** CI produces an unsigned `.flatpak` bundle as a workflow
  artifact. Nothing is signed, no OSTree repo is exported, nothing is
  published to GitHub Pages or anywhere else. There is no self-hosted
  registry yet — that's the rest of #686, gated on the items below.
- **Manual trigger.** Like every other native-editor target, this is opt-in
  and left out of the default per-PR CI (see the root README's CI table) —
  `workflow_dispatch`, or a push touching this directory.
- **Binary is staged, not built in-sandbox.** CI compiles `krudd_qt` on the
  runner itself (via `./setup.sh` + `./krudd.sh build`, the same recipe as
  the docs) and the Flatpak module just installs that binary — it does not
  compile against `org.kde.Sdk`'s Qt6 inside the `flatpak-builder` sandbox.
  That means the bundle validates the *packaging pipeline* (manifest,
  staging, `flatpak-builder`, bundle export) but its Qt6 is built against the
  CI runner's system Qt, not the `org.kde.Platform` runtime's — ABI
  compatibility at actual runtime is **unverified**. A real distributable
  build should compile `krudd_qt` as its own `flatpak-builder` module against
  the SDK's Qt6, with native Dawn supplied as a prebuilt module (Dawn's own
  build needs `gclient`-fetched third-party deps that don't fit the
  sandbox's declarative `sources:` model, so it's vendored in rather than
  built from scratch — see the manifest's comments).
- **Ships the known proof-of-life caveat.** `editor-qt` still renders an
  animated clear colour, not a real scene (`docs/qt-editor-shell.md`). A
  signed, published bundle would ship a color-cycling window — #686 gates
  that on the editor actually rendering something.

## Building it locally

Same prerequisites as [`docs/qt-editor-shell.md`](../../docs/qt-editor-shell.md)
(native Dawn, Qt6) plus `flatpak` and `flatpak-builder`:

```sh
./setup.sh
. ./.krudd-env
KRUDD_QT=1 ./krudd.sh build
install -Dm755 build/bin/krudd_qt packaging/flatpak/dist/bin/krudd-editor-qt

flatpak remote-add --if-not-exists --user flathub \
  https://flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --force-clean --install-deps-from=flathub \
  packaging/flatpak/build-dir packaging/flatpak/io.github.kruddage.Editor.yml

# optional: produce an installable .flatpak bundle
flatpak build-export packaging/flatpak/export-repo packaging/flatpak/build-dir
flatpak build-bundle packaging/flatpak/export-repo krudd-editor.flatpak io.github.kruddage.Editor
```

## What's still needed for #686's actual ask (a self-hosted registry)

- [ ] Build `krudd_qt` as a real in-sandbox module (Qt6 from `org.kde.Sdk`),
      with Dawn vendored as a prebuilt module pinned to the same revision as
      `setup.sh`'s `DAWN_REV`.
- [ ] A GPG signing key, stored as a repo secret, for the exported repo.
- [ ] Export the OSTree repo into the Pages deploy alongside the site, and
      publish a `.flatpakref`.
- [ ] Verify install + launch on real Deck hardware (Game Mode + Desktop
      Mode) — meaningless before `editor-qt` renders a real scene.
