<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Flatpak packaging + self-hosted registry

> **This directory describes a build that no longer exists.** #814 deleted the
> C and Scheme tree, including the Qt editor this packages, so every path and
> command below that reaches into `krudd/` is stale — `krudd.sh`, `krudd_qt`,
> `krudd/third_party/sync.sh`, the Vulkan backend. `flatpak-build.yml` is
> dispatch-only and fails on a guard step that says so.
>
> It is kept, rather than deleted with the tree, because the *packaging* half
> is still correct and still wanted: the manifest shape, the signing opt-in,
> the OSTree export, and the self-hosted `gh-pages` registry a Steam Deck
> subscribes to. [#821](https://github.com/kruddage/engine/issues/821) rebuilds
> the editor on Tauri 2 and repoints all of it. Until then the registry is
> frozen at whatever it last published and `flatpak update` pulls nothing new —
> the accepted cost of delete-and-replace over strangler-fig
> ([#812](https://github.com/kruddage/engine/issues/812)).
>
> Everything below is preserved as written, as the reference #821 works from.

Closes [#686](https://github.com/kruddage/engine/issues/686) — "Distribute the
native editor as a Flatpak from a self-hosted registry". This is the
packaging/registry-hosting half of that issue; the other half — `editor-qt`
rendering an actual scene instead of an animated clear — is tracked
separately in #667/#675/#676 and isn't gated on this.

## What's here

- `io.github.kruddage.Editor.yml` — the `flatpak-builder` manifest for the Qt
  editor shell (`krudd editor-qt`, see the "Build from source" section of the
  [root README](../../README.md#build-from-source)). Builds
  `krudd_qt` **entirely inside the sandbox**, against `org.kde.Sdk`'s own Qt6
  and Vulkan loader/headers — nothing is vendored (the native GPU path is
  Vulkan, an ordinary SDK dependency, not a prebuilt library).
- `.github/workflows/flatpak-build.yml` — runs `flatpak-builder` and — when a
  signing key is configured — exports and publishes a signed OSTree repo to
  this repo's `gh-pages` branch under `/flatpak/`.
- `make-flatpakrepo.sh` — writes the `.flatpakrepo` file users add as a
  remote, with the signing key's public half embedded.
- `io.github.kruddage.Editor.metainfo.xml` — AppStream metadata (name, summary,
  description, release). Plasma Discover (the SteamOS Desktop store) needs this
  to list and tile the app; without it a configured remote installs only from
  the command line. Installed to `/app/share/metainfo/` by the manifest, which
  also installs a `512x512` icon named for the app id so the tile isn't blank.
  Its `<release>` version/date are placeholders substituted at build time from
  `version.txt`, so the version Discover shows is the version that was
  packaged — never hand-edit a number into it.

## Installing from the published registry

```sh
flatpak remote-add --user krudd https://kruddage.github.io/engine/flatpak/krudd.flatpakrepo
flatpak install --user krudd io.github.kruddage.Editor
flatpak run io.github.kruddage.Editor
```

Updates land the normal Flatpak way — `flatpak update` — whenever a signed
build is published. That happens automatically on every engine **release**:
release-please cuts a version tag/GitHub Release (see
`.github/workflows/release-please.yml`) and then dispatches `flatpak-build.yml`
**at that tag**, which rebuilds and re-signs the registry from exactly the
released source. A push to `main` that changes the packaging files themselves
also republishes, and the workflow can be run manually (`workflow_dispatch`) at
any time — dispatch it at a `vX.Y.Z` tag to re-publish that release.

Why the explicit dispatch rather than just triggering on the `release` event:
release-please creates the Release with `GITHUB_TOKEN`, and Actions does not
start workflow runs from `GITHUB_TOKEN`-authored events. The `release` trigger
is still declared (it works if release-please is ever moved to a PAT or App
token), but it fires nothing today — which is exactly how the published
registry silently froze at 18.3.1 while `main` ran on to 18.8.2
([#790](https://github.com/kruddage/engine/issues/790)). `workflow_dispatch` is
one of the two documented exceptions to that recursion rule, so dispatching is
what actually starts the build.

## On a Steam Deck (Discover)

The SteamOS Desktop store, **Discover**, browses Flathub by default; this
editor is a *self-hosted* remote, not a Flathub app, so you add the remote once
and Discover then treats it like any other store app. In Desktop Mode:

```sh
flatpak remote-add --user krudd https://kruddage.github.io/engine/flatpak/krudd.flatpakrepo
```

After that, "KRUDD Editor" shows up in Discover's search — install, launch, and
update it from the GUI, no further terminal use. (Discover only lists it once
the app ships the AppStream metainfo above; that's what this packaging adds.)

Getting into Discover's built-in **Flathub** storefront — where no
`remote-add` is needed — is a separate, larger effort: a submission to the
`flathub/flathub` repo, which won't pass review while the editor only presents
an animated clear (see the caveat below).

## Hosting your own registry

Nothing in the workflow is kruddage-specific — the registry URL is derived
from `github.repository_owner`/`github.event.repository.name` at publish
time, so a fork publishes to *its own* `gh-pages`, at its own
`<owner>.github.io/<repo>/flatpak/`. To turn it on for a fork:

1. Generate a keypair: `gpg --batch --quick-generate-key "<your name> <you@example.com>" default default never`
2. Export the private key and add it as this fork's `FLATPAK_GPG_PRIVATE_KEY`
   repo secret: `gpg --export-secret-keys --armor <keyid>`. Add
   `FLATPAK_GPG_PASSPHRASE` too if the key has one.
3. Enable GitHub Pages (`gh-pages` branch) on the fork, same as the main site.
4. Push to `main` (or run the workflow manually) — the job signs, exports,
   and publishes automatically. No secret configured means the workflow
   still builds and validates the Flatpak (unsigned bundle as a workflow
   artifact) but skips publishing.

Without a signing key, `flatpak-builder` and the bundle export still run —
the workflow always validates the packaging, it just won't push an unsigned
build to anyone's registry.

## Building it locally

You only need `flatpak` and `flatpak-builder` — Qt6 and the Vulkan loader both
come from `org.kde.Sdk` inside the sandbox, so there is no local toolchain to
install first. One thing *does* have to be staged: `flatpak-builder` runs the
module build in a sandbox with **no network**, so the prebuilt s7 artifacts the
engine links (`krudd/third_party/sync.sh`) must already be in the working tree
before you start — otherwise the build dies on `Could not resolve host:
github.com`. Any previous `./krudd.sh build` will have fetched them; from a
fresh checkout, fetch them directly:

```sh
# once per checkout — populates krudd/third_party/ (+ .sha256 sidecars)
root="$PWD" ./krudd/third_party/sync.sh

flatpak remote-add --if-not-exists --user flathub \
  https://flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --force-clean --install-deps-from=flathub \
  packaging/flatpak/build-dir packaging/flatpak/io.github.kruddage.Editor.yml

# optional: produce an installable .flatpak bundle
flatpak build-export packaging/flatpak/export-repo packaging/flatpak/build-dir
flatpak build-bundle packaging/flatpak/export-repo krudd-editor.flatpak io.github.kruddage.Editor
```

## Known caveat

`editor-qt` still renders an animated clear colour, not a real scene — the
Vulkan backend presents, but does not yet translate the renderer's draw stream
(see the `SCOPE` note atop
[`renderer_vulkan.c`](../../krudd/engine/render/vulkan/renderer_vulkan.c)). A
signed, published build today installs and
runs, but shows a color-cycling window. That's tracked separately
(#667/#675/#676), not blocked on anything here.
