# @kruddage/site

Stages the deployable static site from `@kruddage/engine`'s artifacts.

```sh
pnpm --filter @kruddage/site run build          # -> packages/site/dist
pnpm --filter @kruddage/site run build /tmp/out # or anywhere
```

This is what CI uploads and what `gh-pages` serves. It replaces
`.github/scripts/stage-site.sh`.

## What it does

Copies the whitelisted artifacts into a clean output directory, renaming the
ones the engine declares cache-bustable and rewriting the entry document's
references to match.

The whitelist matters: the kruddmake build directory holds object files,
archives and dependency checkouts that must never reach the Pages branch. That
part is unchanged from the shell script. What changed is where the rules come
from — this package asks `@kruddage/engine` what was built, which of those files
may be renamed, and what hash to rename them to. It does not look at
`<repo>/build`, does not run `krudd.sh`, and does not derive the hash itself.
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
