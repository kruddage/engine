// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * Reading a `color` param as three numbers.
 *
 * A `color` param holds text — `#e2574c` — because a param is a number or a
 * name and nothing else (see `ParamValue`), and text is what a person types
 * and what survives a JSON round trip. The `colour` column holds three floats
 * per slot. This is the one place that turns the first into the second, so a
 * kind that tints something reads the same hex the settings sheet shows.
 *
 * ## Why a bad value is white rather than a throw
 *
 * `validate` checks that a `color` param is text and stops there — `#e2574`
 * is a perfectly good string. It is also what `#e2574c` looks like for one
 * keystroke while somebody is typing it into the settings sheet, and the
 * document is live: the runner walks it on the very next frame. A parse that
 * threw would take the whole board down mid-keystroke, and `index.ts` reports
 * a frame failure once and stops the loop — a typo would end the session.
 *
 * So an unreadable value reads as white, which is the identity the colour
 * column multiplies by (see `WHITE_FILL` in `crates/shell/web/src/lib.rs`).
 * The cell keeps its shader gradient and nothing is tinted, which is visible
 * without being fatal.
 *
 * ## Channels are taken as they are written
 *
 * `#808080` is 0.5 in the column, not the 0.216 an sRGB-to-linear conversion
 * would give. The three vertex colours the fragment shader multiplies by are
 * written the same way, so this matches what the renderer already does rather
 * than introducing a second convention that only these params obey.
 */

/** Three channels in `[0, 1]`, in the order the `colour` column stores them. */
export type Rgb = readonly [number, number, number];

/** What an unreadable colour reads as: the identity the column multiplies by. */
export const WHITE: Rgb = [1, 1, 1];

/**
 * A `#rgb` or `#rrggbb` string as three channels in `[0, 1]`.
 *
 * Anything else — a name, a half-typed hex, an empty string — is [`WHITE`].
 * See the module docs for why that is not a throw.
 */
export function rgbOf(value: string): Rgb {
	const hex = value.trim();
	if (!hex.startsWith("#")) {
		return WHITE;
	}
	const digits = hex.slice(1);
	// `#rgb` is the shorthand every stylesheet takes, and doubling each digit
	// is what it means: `#f00` is `#ff0000`, not `#f00000`.
	const wide =
		digits.length === 3
			? digits.replace(/./g, (digit) => digit + digit)
			: digits;
	if (wide.length !== 6 || !/^[0-9a-fA-F]{6}$/.test(wide)) {
		return WHITE;
	}
	return [
		Number.parseInt(wide.slice(0, 2), 16) / 255,
		Number.parseInt(wide.slice(2, 4), 16) / 255,
		Number.parseInt(wide.slice(4, 6), 16) / 255,
	];
}
