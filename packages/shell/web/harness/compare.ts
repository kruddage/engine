// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The pixel comparison, and the policy for what counts as a pass.
 *
 * Decoding happens inside the page — `buildCompareExpression`'s returned
 * source runs there, over CDP — rather than in Node, because the page is
 * already a PNG codec (`createImageBitmap`) and Node's standard library is
 * not. Writing a PNG decoder here to avoid a dependency would be choosing a
 * different, larger unnecessary thing over the dependency it was trying to
 * avoid.
 *
 * `judge` is the opposite way round on purpose: what counts as a failure is
 * policy, meant to be read and argued about in review, so it is ordinary
 * typechecked TypeScript — not a string shipped into a browser tab.
 */

/**
 * Per-channel (0-255) difference above which two pixels count as different.
 *
 * Not zero, even though the rendering path (SwiftShader, pinned in
 * `chromium.ts`) is expected to be bit-for-bit deterministic given identical
 * inputs. This is insurance against a SwiftShader or Chromium point-release
 * changing rounding by a shade, not against the demo's animation — the
 * animation's own nondeterminism (real time, real drift) is removed
 * entirely by the virtual clock in `inject.ts`, so this tolerance is never
 * asked to paper over that. A tolerance chosen to make a flaky test pass is
 * how an instrument stops failing loudly; this one exists only for rounding
 * noise, and 8/255 is generous enough to absorb that while still catching
 * anything that would actually look different to a person.
 */
export const CHANNEL_TOLERANCE = 8;

/**
 * The fraction of pixels allowed to exceed `CHANNEL_TOLERANCE` before a run
 * fails.
 *
 * 1% of a 400x300 capture is 1,200 pixels — nowhere near enough to hide a
 * missing triangle, a wrong colour, or a background that took over the
 * frame, all of which move a large, contiguous fraction of the image. It
 * exists for anti-aliased edge pixels, which are the one place a
 * sub-channel-tolerance rounding difference is expected to cluster.
 */
export const MAX_DIFF_FRACTION = 0.01;

/**
 * The band (max - min per channel, across the whole image) below which a
 * capture counts as a uniform colour.
 *
 * Deliberately tighter than `CHANNEL_TOLERANCE`: this question is "did
 * anything at all get drawn", not "did the right thing get drawn", and it
 * must stay strict even while the diff check above allows a little
 * rasteriser noise.
 */
const BLANK_BAND = 2;

/** What the in-page comparison reports back. */
export interface CompareStats {
	width: number;
	height: number;
	refWidth: number;
	refHeight: number;
	sameSize: boolean;
	capturedIsBlank: boolean;
	referenceIsBlank: boolean;
	diffPixels: number;
	totalPixels: number;
	diffFraction: number;
}

/**
 * Builds the `Runtime.evaluate` expression that decodes both PNGs and diffs
 * them, entirely inside the page.
 *
 * The two payloads are base64 — safe to splice directly into a JS string
 * literal, because the base64 alphabet contains no quote, backslash, or
 * newline that could break out of it.
 */
export function buildCompareExpression(
	capturedBase64: string,
	referenceBase64: string,
): string {
	return `
(async () => {
	async function decode(base64) {
		const binary = atob(base64);
		const bytes = new Uint8Array(binary.length);
		for (let i = 0; i < binary.length; i++) {
			bytes[i] = binary.charCodeAt(i);
		}
		const bitmap = await createImageBitmap(new Blob([bytes], { type: "image/png" }));
		const canvas = document.createElement("canvas");
		canvas.width = bitmap.width;
		canvas.height = bitmap.height;
		const context = canvas.getContext("2d");
		context.drawImage(bitmap, 0, 0);
		return context.getImageData(0, 0, bitmap.width, bitmap.height);
	}

	function isBlank(imageData) {
		const data = imageData.data;
		let minR = 255;
		let minG = 255;
		let minB = 255;
		let maxR = 0;
		let maxG = 0;
		let maxB = 0;
		for (let i = 0; i < data.length; i += 4) {
			minR = Math.min(minR, data[i]);
			maxR = Math.max(maxR, data[i]);
			minG = Math.min(minG, data[i + 1]);
			maxG = Math.max(maxG, data[i + 1]);
			minB = Math.min(minB, data[i + 2]);
			maxB = Math.max(maxB, data[i + 2]);
		}
		return maxR - minR <= ${BLANK_BAND} && maxG - minG <= ${BLANK_BAND} && maxB - minB <= ${BLANK_BAND};
	}

	const captured = await decode("${capturedBase64}");
	const reference = await decode("${referenceBase64}");

	const sameSize = captured.width === reference.width && captured.height === reference.height;
	let diffPixels = 0;
	if (sameSize) {
		const a = captured.data;
		const b = reference.data;
		for (let i = 0; i < a.length; i += 4) {
			const dr = Math.abs(a[i] - b[i]);
			const dg = Math.abs(a[i + 1] - b[i + 1]);
			const db = Math.abs(a[i + 2] - b[i + 2]);
			if (dr > ${CHANNEL_TOLERANCE} || dg > ${CHANNEL_TOLERANCE} || db > ${CHANNEL_TOLERANCE}) {
				diffPixels++;
			}
		}
	}
	const totalPixels = captured.width * captured.height;

	return {
		width: captured.width,
		height: captured.height,
		refWidth: reference.width,
		refHeight: reference.height,
		sameSize,
		capturedIsBlank: isBlank(captured),
		referenceIsBlank: isBlank(reference),
		diffPixels,
		totalPixels,
		diffFraction: totalPixels > 0 ? diffPixels / totalPixels : 1,
	};
})()
`;
}

/** Every reason `stats` should fail the run — empty if it should pass. */
export function judge(stats: CompareStats, referencePath: string): string[] {
	const problems: string[] = [];

	// Checked unconditionally, never inferred from the diff below: two
	// uniform images diff as identical, which is exactly the scenario #827
	// exists to catch — "nothing rendered" must never read the same as
	// "rendered correctly" merely because the reference happens to agree.
	if (stats.capturedIsBlank) {
		problems.push(
			"the capture is a uniform colour — nothing was drawn, or the " +
				"WebGL2 backend silently failed. This is the exact failure this " +
				"harness exists to catch; it is not a diff-tolerance problem.",
		);
	}
	if (stats.referenceIsBlank) {
		problems.push(
			`${referencePath} is itself a uniform colour — the checked-in ` +
				"reference is broken and needs regenerating " +
				"(KRUDD_UPDATE_REFERENCE=1 cargo xtask render-test), not the harness.",
		);
	}
	if (!stats.sameSize) {
		problems.push(
			`captured ${stats.width}x${stats.height} but the reference is ` +
				`${stats.refWidth}x${stats.refHeight} — the two cannot be diffed pixel for pixel`,
		);
	} else if (stats.diffFraction > MAX_DIFF_FRACTION) {
		problems.push(
			`${stats.diffPixels}/${stats.totalPixels} pixels ` +
				`(${(stats.diffFraction * 100).toFixed(2)}%) differ by more than ` +
				`${CHANNEL_TOLERANCE}/255 per channel, over the ` +
				`${(MAX_DIFF_FRACTION * 100).toFixed(0)}% budget`,
		);
	}
	return problems;
}
