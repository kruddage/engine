# projects/

The games. One directory each, and a game is **a single `.scm` the engine loads at runtime**
(#976) — not a module the engine links, which is why these sit beside `krudd/` rather than
inside it (#1013).

This file is the contract. Before it existed the rules were spread across `manifest.scm`'s
tier note, three `build.scm` headers and the README tree, which between them stated most of
it and agreed on all of it, but left "what may a project do" with no single answer. Those
places now point here instead of restating it.

One of the four is not a game: `default` is the scene the page opens on — a checkered floor
and four animated props — which the renderer used to seed from C into every boot regardless
of what the URL had asked for (#1034). It is a project like the others and holds the staged
slot; nothing about being the default makes it a different kind of thing.

## What a project is

```
projects/<name>/
  <name>.scm     the game — the one file the engine evaluates
  build.scm      what the build does with it
  <name>_test.c  a native test, if the game wants one
```

The `.scm` is the whole game. There is no second source file, no import mechanism and no
package format; a project that has outgrown one file has outgrown what a project currently
is, and that is a design question rather than a missing feature (#976).

The directory is named in `krudd/kruddmake/manifest.scm`, last, after every engine tier. A
manifest entry beginning `projects/` resolves against the repository root rather than
`krudd/engine/` — one prefix, one predicate, `rz-project-path?` in `resolve.scm` (#1016).

## What a project may do

- **Reach the engine.** `(root …)` in a `(sources …)` or `(private …)` clause resolves
  against the engine root from here exactly as it does from an engine module, and `(link …)`
  may name any engine library. A project reaches for the engine freely; that direction is
  never the problem.
- **Ship itself** with `(project-source "<name>.scm")`. The build copies the source into
  `assets/` beside `index.html` and names it in `assets/projects.json`, which is the list the
  page offers. Any number of projects may declare one, and a project that does not is
  unreachable from the page — the page cannot list a directory over HTTP, so what the build
  wrote down is the only way it learns what exists.
- **Claim the staged slot** with `(staged-project "<name>.scm")` — ship, plus embed into the
  WASM image under the fixed symbol `core/engine.c` evaluates at boot, so the page opens on a
  scene with no network round trip. The staged project is what a bare URL opens, by launcher
  slot rather than by name, so the boot path picks it up without learning which project it
  is (#1034). **Exactly one project may**, and that is a decision rather than a technicality;
  the argument is at `resolve-check-staged` in `resolve.scm`. `default` holds it — it is the
  project that exists to be the thing the page opens on. `staged-project` implies
  `project-source`, so a directory declares one or the other, never both.
- **Declare an `(embed …)` for its own test**, so the test drives the real source with no
  filesystem under it. The embedded symbol belongs to that test and nothing else.
- **Declare an `(executable …)` and a `(test …)`.** All three projects do; it is why all
  three are in the manifest at all.

## What a project may not do

| | Rule | Held by |
|---|---|---|
| 1 | Declare a `(library …)` or `(interface-library …)` | **Enforced** — `resolve-check-projects` |
| 2 | Be linked by an engine module | **Enforced**, by rule 1 — there is nothing to name |
| 3 | Claim the staged slot when another project has | **Enforced** — `resolve-check-staged` |
| 4 | Reach into another project | **Trusted** |

**Rule 1** is the one worth enforcing rather than documenting, and it is the reason rule 2
holds. `resolve-check-tiers` reads `manifest.scm`'s order and fails generation on a library
link that inverts it, but it is silent about `projects/` today for an accidental reason:
nothing there declares a library, so there is no name for an engine module to link and no
edge for the tier check to see. That is the door standing open rather than the door being
shut. A project declaring a library would be inside the tier order the moment an engine
module linked it, and the manifest's "a project may reach for anything" would stop being
true — so the declaration fails at generation, naming the project and the rule, rather than
the link failing later somewhere it is harder to read.

**Rule 4** is trusted because the escape hatch that would break it is the same one that makes
legitimate paths expressible: `(raw "../ducks/ducks.scm")` from another project's `build.scm`
would resolve, since `raw` exists precisely to pass a path through unexamined. Forbidding it
would mean either giving up `raw` or teaching the checker to interpret it, and neither is
worth it for a rule nothing has come close to breaking. If one ever does, the check belongs
beside rule 1 rather than in a new mechanism.

## What a project is not

Not a tier. `manifest.scm` is an ordered list where a module may only reach for one above it,
and `projects/*` is listed last so nothing can be below it — but the ordering is not really
what keeps a project out of the tiers. Rule 1 is. A project declares no library, so there is
no edge to point at one, and its position in the list is a statement rather than a
constraint doing work.

Not multi-file, not importable, not a package. Still explicitly out (#976).
