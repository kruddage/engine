# @kruddage/site

Stages the deployable static site from `@kruddage/engine`'s artifacts.

```sh
pnpm --filter @kruddage/site run build          # -> packages/site/dist
pnpm --filter @kruddage/site run build /tmp/out # or anywhere
```

This is what CI uploads and what `gh-pages` serves. It replaces
`.github/scripts/stage-site.sh`.

## The layout it stages

```
/            @kruddage/editor's build — the editor is the site
/game/       the engine's own page: shell.html.in, its loader, its WASM,
             the PWA files and the runtime assets
```

#946 had these the other way round: the engine held the root and the editor sat
at `/editor/`, because the editor was a skeleton and the shell was the only
editor that existed. #953 reversed it once the editor did the job.

Everything the engine's page references is relative — the loader tag,
`manifest.webmanifest` (`start_url` and `scope` are both `"."`), `sw.js`, the
icons, `assets/` — so the whole set moves by being copied into one directory and
nothing inside it has to learn its own URL. The service worker's scope narrows
with it, which is right: it caches the game host, and the editor is not its
business.

An unbuilt editor is now an **error** rather than a note. It used to be
skippable — the engine still served at the root without it — but the root is the
editor's document now, so skipping stages a site that 404s on the way in.

## What it does

Copies the whitelisted artifacts into a clean output directory, renaming the
ones the engine declares cache-bustable and rewriting the entry document's
references to match.

The whitelist matters: the kruddmake build directory holds object files,
archives and dependency checkouts that must never reach the Pages branch. That
part is unchanged from the shell script. What changed is where the rules come
from — this package asks `@kruddage/engine` what was built, which of those files
may be renamed, and what hash to rename them to. It does not look at
`<repo>/build`, does not run kruddmake, and does not derive the hash itself.
`pnpm check` fails the workspace if that ever stops being true.

## The failure this prevents

`index.wasm` is served under a hashed name and the page finds it only via a
`Module.locateFile` hook that the shell template baked the same hash into at
build time. `stage-site.sh` computed its half with `git rev-parse --short HEAD`
and the Scheme computed the other half in `introspect.scm`. They agreed by
construction, and nothing verified it — a divergence would have produced a site
that staged cleanly, deployed cleanly, and 404'd on the WASM module in the
browser.

`scripts/build.mjs` now takes the stem from the manifest, which took it from the
built HTML. There is one hash, and it comes from the artifact.

The staging step also fails loudly when the entry document has lost the loader
reference it is supposed to rewrite. `sed` succeeded silently on no match, which
meant a shell template that stopped emitting its `{{{ SCRIPT }}}` substitution
would have staged an `index.html` pointing at a file nobody wrote.
