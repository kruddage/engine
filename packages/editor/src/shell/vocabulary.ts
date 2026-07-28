// SPDX-License-Identifier: GPL-2.0-or-later
//
// The panel vocabulary.
//
// #948 calls this its load-bearing half and #954 settles it here, in the first
// shell PR, because it costs nothing now and an argument later. #855 fixed the
// board's vocabulary the same way, and it is why board/node/wire/port/lane
// never became a discussion.
//
// **These are the words. Use them in code, in comments, in commit messages and
// in PR bodies, and do not introduce a synonym.** "Sidebar" is not a rail,
// "tree" is not the outliner, "canvas" is not the viewport deck. A second name
// for one thing is how six PRs end up describing four different layouts.
//
// The table is exported rather than written in a comment so the suite can
// assert that every term is defined and every panel id is one of them — a
// vocabulary nothing checks is a vocabulary that drifts on the second PR.

/** Every term, with the one line that defines it. */
export const VOCABULARY = {
	"command bar": "The menu strip across the top: File, Edit, View, Help, and the keyboard shortcuts they declare.",
	toolbar: "The row under the command bar: direct actions, and the live badges for the renderer and the build.",
	rail: "The narrow icon strip down the left edge that shows and hides the docked panels.",
	"viewport deck": "The centre of the shell, where the engine's canvas lives. Panels dock around it; it is not one of them.",
	outliner: "The scene as a tree of entities, docked left.",
	inspector: "One panel with a tab group for the selected entity's properties, docked right. Not three docks.",
	assets: "The project's meshes, textures, sounds and scenes, docked bottom.",
	console: "The engine's output, and eventually a live Scheme REPL into the running image. Docked bottom.",
	build: "A panel rather than a button, because a build is slow, it fails, and it needs somewhere to say so.",
	"status strip": "The bar along the bottom: fps, resolution, driver, and the transient hint an unwired action shows.",
} as const;

export type Term = keyof typeof VOCABULARY;

/**
 * Where a panel sits. Four slots, matching the four areas `editor_layout.scm`
 * declares, plus the deck.
 *
 * The deck is deliberately not somewhere a panel can be moved to. It holds the
 * engine's canvas, and React re-creates a DOM node that changes position in the
 * tree — moving the viewport between slots would hand the engine a detached
 * canvas and take the GL context with it. #949 owns the canvas handover; this
 * shell's job is to not break it in the meantime.
 */
export type PanelSlot = "left" | "right" | "bottom" | "deck";

export const DOCKABLE_SLOTS: readonly PanelSlot[] = ["left", "right", "bottom"];

export const SLOT_LABEL: Record<PanelSlot, string> = {
	left: "Left",
	right: "Right",
	bottom: "Bottom",
	deck: "Deck",
};
