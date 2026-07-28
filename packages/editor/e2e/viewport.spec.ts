// SPDX-License-Identifier: GPL-2.0-or-later
//
// The viewport, in a real browser, against a real engine (#949).
//
// Everything here is a thing jsdom would answer wrongly, which is the whole
// admission criterion (#944, Q6). jsdom reports every element as zero-sized, it
// stubs the pointer-capture API into a no-op, it implements no ResizeObserver
// and no GL context — so a component test of any of the below would pass on
// code a browser rejects, which is worse than not testing it.
//
// The component suite (`test/viewport.test.tsx`, `test/viewport-pointer.test.ts`)
// covers what can be answered honestly there: the state machine's every branch,
// the bar reading the engine rather than a local state, and the canvas element
// surviving a re-render. This is the rest.
//
// WebGL is pinned for the same reason boot.spec.ts pins it: the runner is
// headless Chromium with no GPU, `?renderer=webgl` is the shell's own documented
// opt-out, and a live renderer is what the claims below need.

import { expect, test } from "@playwright/test";
import type { Locator, Page } from "@playwright/test";

/** The engine up and drawing, which every test below needs first. */
async function booted(page: Page): Promise<void> {
	await page.goto("/?renderer=webgl");
	await expect(page.getByTestId("status-phase")).toHaveText(/running|ready/, {
		timeout: 45_000,
	});
}

/** Drag across the pointer surface, in steps, so moves actually fire. */
async function drag(
	surface: Locator,
	from: { x: number; y: number },
	to: { x: number; y: number },
	options: { button?: "left" | "middle"; steps?: number } = {}
): Promise<void> {
	const box = await surface.boundingBox();
	if (!box) throw new Error("the viewport surface has no box");

	const page = surface.page();
	await page.mouse.move(box.x + from.x, box.y + from.y);
	await page.mouse.down({ button: options.button ?? "left" });
	/*
	 * Stepped, not teleported. A single move would be one pointermove, and the
	 * thing under test is a gesture that resolves over several frames — the
	 * engine's answer about what the press grabbed arrives on a flush.
	 */
	await page.mouse.move(box.x + to.x, box.y + to.y, {
		steps: options.steps ?? 12,
	});
	await page.mouse.up({ button: options.button ?? "left" });
}

test.describe("the canvas handover", () => {
	test("the engine's canvas fills the deck and has a GL context", async ({
		page,
	}) => {
		await booted(page);

		/*
		 * The two halves of the contract in boot.ts: the element the engine
		 * finds by `#canvas` is the one the panel rendered, and it actually
		 * carries a context. jsdom can answer neither — it has no layout and no
		 * WebGL — and getting either wrong fails at runtime and nowhere near
		 * the cause.
		 */
		const measured = await page.evaluate(() => {
			const canvas = document.querySelector("canvas#canvas");
			if (!(canvas instanceof HTMLCanvasElement)) return null;
			const rect = canvas.getBoundingClientRect();
			return {
				cssWidth: rect.width,
				cssHeight: rect.height,
				backingWidth: canvas.width,
				backingHeight: canvas.height,
				dpr: window.devicePixelRatio,
				/* `false` would mean the context was never created; asking for
				 * it again returns the existing one rather than a second. */
				hasContext: canvas.getContext("webgl2") !== null,
			};
		});

		expect(measured).not.toBeNull();
		expect(measured!.cssWidth).toBeGreaterThan(100);
		expect(measured!.cssHeight).toBeGreaterThan(100);
		expect(measured!.hasContext).toBe(true);

		/* The backing store follows the device pixel ratio, which is the half
		 * of "survives a DPI change" that can be asserted without moving a
		 * window between monitors. */
		expect(measured!.backingWidth).toBe(
			Math.round(measured!.cssWidth * measured!.dpr)
		);
		expect(measured!.backingHeight).toBe(
			Math.round(measured!.cssHeight * measured!.dpr)
		);
	});

	test("the canvas keeps its context across a resize", async ({ page }) => {
		await booted(page);

		const before = await page.evaluate(() => {
			const canvas = document.querySelector<HTMLCanvasElement>("canvas#canvas");
			/* Stamped onto the element so the identity can be compared after
			 * the resize — a remount would produce a node without it. */
			canvas?.setAttribute("data-witness", "1");
			return canvas?.width ?? 0;
		});

		await page.setViewportSize({ width: 900, height: 700 });
		/* ResizeObserver fires on a frame, so wait for the effect rather than
		 * for a fixed delay. */
		await expect
			.poll(async () =>
				page.evaluate(
					() =>
						document.querySelector<HTMLCanvasElement>("canvas#canvas")?.width ?? 0
				)
			)
			.not.toBe(before);

		const after = await page.evaluate(() => {
			const canvas = document.querySelector<HTMLCanvasElement>("canvas#canvas");
			return {
				witness: canvas?.getAttribute("data-witness"),
				hasContext: canvas?.getContext("webgl2") !== null,
			};
		});

		/*
		 * The same element, still holding a context. A React tree that
		 * remounted the canvas on a resize would leave the engine drawing into
		 * a detached node, and the symptom would be a viewport that goes black
		 * rather than an error.
		 */
		expect(after.witness).toBe("1");
		expect(after.hasContext).toBe(true);
	});

	test("the engine is picking against the size the editor gave it", async ({
		page,
	}) => {
		await booted(page);

		/*
		 * The number the pick is answered against, read back out of the engine
		 * through the viewport query — which is the point of it being in the
		 * reply at all. A size the editor reported and the engine did not adopt
		 * is a whole class of bug that shows up only as clicks landing
		 * somewhere other than where they were aimed.
		 */
		const measured = await page.evaluate(() => {
			const rect = document
				.querySelector<HTMLCanvasElement>("canvas#canvas")
				?.getBoundingClientRect();
			return `${Math.round(rect?.width ?? 0)}x${Math.round(rect?.height ?? 0)}`;
		});

		await expect(page.getByTestId("viewport-surface")).toHaveAttribute(
			"data-engine-size",
			measured
		);
	});
});

test.describe("the pointer over the canvas", () => {
	test("a drag orbits the camera rather than scrolling the page", async ({
		page,
	}) => {
		await booted(page);
		const surface = page.getByTestId("viewport-surface");

		const scrollBefore = await page.evaluate(() => window.scrollY);
		await drag(surface, { x: 200, y: 200 }, { x: 340, y: 240 });

		/*
		 * `touch-action: none` and the captured pointer are what keep a drag
		 * over the viewport from scrolling the page. jsdom has neither, so this
		 * is exactly the assertion that would lie there.
		 */
		expect(await page.evaluate(() => window.scrollY)).toBe(scrollBefore);
		await expect(surface).toHaveAttribute("data-phase", "idle");
	});

	test("a drag that leaves the viewport still delivers its moves", async ({
		page,
	}) => {
		await booted(page);
		const surface = page.getByTestId("viewport-surface");
		const box = await surface.boundingBox();

		/*
		 * Pointer capture, which jsdom stubs to a no-op. Without it a gizmo
		 * drag stops the instant the pointer crosses the viewport's edge —
		 * which is precisely when a reader is dragging something to the far
		 * side of the scene.
		 */
		await page.mouse.move(box!.x + 100, box!.y + 100);
		await page.mouse.down();
		await page.mouse.move(box!.x + box!.width + 200, box!.y + 100, {
			steps: 10,
		});
		await expect(surface).not.toHaveAttribute("data-phase", "idle");
		await page.mouse.up();
		await expect(surface).toHaveAttribute("data-phase", "idle");
	});

	test("chrome floating over the canvas eats its own press", async ({
		page,
	}) => {
		await booted(page);

		/*
		 * The pointer-capture arbitration #944's Q4 comment added to #945: a
		 * React control over the viewport must not let a camera drag start
		 * underneath it. The bar floats over the canvas, so pressing a button
		 * on it is the smallest honest case of that.
		 */
		const surface = page.getByTestId("viewport-surface");
		await page.getByTestId("gizmo-mode-rotate").click();

		await expect(page.getByTestId("gizmo-mode-rotate")).toHaveAttribute(
			"aria-checked",
			"true"
		);
		await expect(surface).toHaveAttribute("data-phase", "idle");
	});
});

test.describe("picking and the selection", () => {
	test("clicking empty space clears the selection", async ({ page }) => {
		await booted(page);

		/*
		 * The engine's own answer, not the editor's: the pick raycasts, calls
		 * set_selected, and the inspector renders whatever the selection query
		 * comes back with. A corner of the viewport is the one place a scene
		 * reliably has nothing in it.
		 */
		const surface = page.getByTestId("viewport-surface");
		const box = await surface.boundingBox();
		await page.mouse.click(box!.x + 8, box!.y + box!.height - 8);

		await expect(page.getByTestId("inspector-empty")).toBeVisible({
			timeout: 5_000,
		});
	});
});
