# Coding Standard

## Philosophy

This engine is built by many hands and many agents, most of which never meet.
The code is the only thing they share, so the code has to carry the
coordination: anyone should be able to open any file and find it shaped the way
they already expect.

We get that by writing in the canonical form. C follows Kernighan & Ritchie.
Scheme follows the line that runs back to McCarthy. Not out of nostalgia — these
are the styles with the longest, cleanest lineage, the ones a careful programmer
has recognized on sight for decades. Code written this way is legible to anyone
who knows the tradition, and it is the form least likely to be written wrong,
because it is the form that has been written *well* the most.

From that, one operating rule: prefer uniformity over individual taste. When two
forms are both correct, the more canonical one wins — it is the one every
contributor already knows, so it is the one that costs no one a judgment call.
The standards here exist to remove decisions, not to record preferences.

---

## C Modules

C code in this project follows the Linux kernel coding style.
The authoritative reference is `Documentation/process/coding-style.rst`
in the kernel source tree (also at https://kernel.org/doc/html/latest/process/coding-style.html).
What follows is a project-specific digest of the rules that matter most here.

---

## Indentation

Use **tabs**, not spaces. One tab per level. Configure your editor to display
tabs as 8 columns — the 8-column indent is intentional; it makes deeply nested
code visibly uncomfortable, which is a feature.

```c
int32_t engine_foo(int32_t x)
{
	if (x < 0) {
		x = 0;
	}
	return x;
}
```

---

## Line length

80 columns. If a line runs longer, break it. Statements, expressions, and
function signatures all fit within 80 columns with a little care.

---

## Braces

K&R style. Opening brace goes at the end of the line for all compound
statements (if, for, while, switch). Exception: the opening brace of a
**function body** goes on its own line.

```c
/* control flow — brace at end of line */
if (condition) {
	do_thing();
} else {
	do_other();
}

/* function body — brace on its own line */
int32_t engine_ping(void)
{
	return 1;
}
```

Omit braces on single-statement branches only when both the if and else
(if present) are single statements and the result is unambiguous.

---

## Naming

Lowercase with underscores everywhere. No camelCase, no PascalCase.

- Functions and variables: `engine_ping`, `frame_count`
- Constants and macros: `ENGINE_VERSION`, `MAX_SPRITES`
- Prefixed by module: `engine_`, `renderer_`, `input_`

---

## Functions

Short. Do one thing. If a function needs more than two levels of indentation,
consider splitting it. Name functions as verb phrases: `engine_init`,
`renderer_clear`, `input_poll`.

Declare functions that are not part of the public ABI as `static`.

---

## Comments

Use `/* ... */`, not `//`. Block comments use this form:

```c
/*
 * This explains the why, not the what.
 * Keep it short.
 */
```

Don't comment what the code obviously does. Comment non-obvious invariants,
hardware quirks, or ABI constraints.

---

## Types

Use fixed-width types from `<stdint.h>` for anything that crosses the WASM ABI
boundary: `int32_t`, `uint32_t`, `int64_t`, etc. Avoid `int` and `long` in
public headers.

---

## Structs and typedefs

No typedefs to hide struct or pointer types. Write `struct foo`, not a typedef
alias. Exception: opaque handles where the caller must never dereference the
pointer may use a typedef for clarity.

```c
/* preferred */
struct engine_state {
	int32_t frame;
};

/* avoid */
typedef struct engine_state engine_state_t;
```

---

## Headers

Every `.c` file includes its own `.h` first. Guards use the filename in
uppercase with underscores:

```c
#ifndef ENGINE_H
#define ENGINE_H
/* ... */
#endif /* ENGINE_H */
```

No `#pragma once`.

---

## ABI exports

Functions exported through the WASM ABI are declared in the module's `.h`
file. All other functions are `static`. Keep exported surface area small.

---

## Scheme — `.scm` files

Scheme is not a second-class citizen here — the README frames it as "the build
system and the game." `.scm` files carry the `build.scm` manifests, the engine's
scripting runtime, and whole games' logic. Write it in the spirit of the
tradition that runs back to McCarthy: small procedures, clear data flow, no
cleverness a reader has to unwind.

**Comments.** Every `.scm` file opens with the SPDX license header:

```scheme
; SPDX-License-Identifier: GPL-2.0-or-later
```

Every other comment is a full-line comment that begins with the `;;!` marker
(leading whitespace before it is fine). Lift asides onto their own `;;!` line
directly above the code they annotate:

```scheme
;;! Whose turn it is: 1 = white (ivory), 2 = black (ebony). White moves first.
(define *chess-turn* 1)
```

Trailing same-line comments — `(define x 90)  ; note` — are **not** allowed, and
CI enforces this (`.github/scripts/lint-scm-comments.py`, the "Lint .scm
comments" job). The reason is uniformity, not distaste for short notes: with one
comment form and one place it can go, every file comes out shaped the same way,
and no contributor — human or agent — ever has to decide "is this note short
enough to put inline." The `;;!` marker also keeps documentation greppable as a
single class, the way a `#!` shebang opts a line in. If a rewrite feels like it
costs a line of vertical space, that is the standard working as intended.

**Indentation.** Spaces, never tabs, indented per the canonical Scheme
convention: special forms (`define`, `let`, `cond`, `lambda`, ...) indent their
body two spaces past the opening paren, and ordinary call arguments align
under the first argument:

```scheme
(define (chess-square? p)
  (and (pair? p) (< -1 (car p) 8) (< -1 (cadr p) 8)))

(kruddgui-rect* (list bx by bs bs)
                (if active kruddgui-active-bg kruddgui-idle-bg))
```

This applies uniformly to `build.scm` manifests too — one indentation rule for
the whole extension rather than a separate convention for declarative data.
It is not an inconsistency with C's tabs (`CODING_STANDARD.md`'s C section);
each language keeps its own convention, and Scheme's alignment can't be
expressed on 8-column tab stops in the first place. The convention has a
single mechanical arbiter — Emacs `scheme-mode`'s stock indenter — rather than
a per-line human judgment call, which is what makes it enforceable: CI checks
it (`.github/scripts/indent-scm.py --check`, the "Check .scm indentation"
job), mirroring the comment-convention lint above. Run
`python3 .github/scripts/indent-scm.py <file.scm>...` to reindent in place.

**Naming.** Lowercase with hyphens — `chess-reset`, `all-digits?`. This is
Scheme's `kebab-case`, not C's `snake_case`; each language keeps its own
convention. Prefix procedures by their module or subject (`chess-`, `krudd-`,
`kruddgui-`). Suffix boolean predicates with `?` (`chess-square?`, `contains?`)
and mutating procedures with `!` (`chess-move!`, `scene-outline!`). Wrap mutable
module-level state in earmuffs (`*chess-turn*`, `*chess-sel*`).
