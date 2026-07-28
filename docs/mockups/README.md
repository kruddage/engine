# Mockups

Throwaway layout studies. Nothing here is built, tested, or shipped — these
files exist so an argument about layout happens in something cheap to delete
rather than in code that isn't.

## `editor-layout.html` — the map editor, on a desktop and on a phone

Answers #886. One self-contained file: open it from `file://`, no build, no
dependency, no engine. The perspective view is a placeholder where the WebGPU
canvas goes; the ortho views are drawn SVG; every number in it is a plausible
lie.

**Open it and drive it.** The buttons across the top switch the editor between
a 390px phone, a 900px tablet and a 1440px desktop without resizing the window
— the layout is written in *container* queries, so it answers to the frame it
is mounted in rather than to the browser. The same rules fire off the real
viewport when the file is opened on a real phone, where those buttons hide
themselves.

What it settles:

- **The panel vocabulary** — command bar, toolbar, rail, viewport deck,
  outliner, brush palette, inspector (brush / surface / entity), assets,
  console, build, graph canvas, toolbelt, sheet, status strip. The table at
  the bottom of the page is the definition of each.
- **The breakpoints** — desktop ≥ 1100px (four views, three docks), tablet
  720–1099px (one view, drawer for the outliner), phone < 720px (one view,
  sheets, thumb-reachable toolbelt).
- **Where the phone stops** — view, navigate, tweak params, place and move
  brushes. Per-vertex dragging, node wiring and multi-view are desktop-only,
  and the page says so rather than implying otherwise.

One structural idea in it is worth keeping whatever the real editor is built
from: **a panel is mounted, not duplicated.** The same DOM node moves between
dock, drawer and sheet as the layout changes, so there is one Inspector, not
three that drift apart.

Whether the real editor is DOM chrome or kruddgui is not decided here — that
is #885's call.
