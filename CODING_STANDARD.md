# Coding Standard — C Modules

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
