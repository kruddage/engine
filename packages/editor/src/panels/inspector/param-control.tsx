// SPDX-License-Identifier: GPL-2.0-or-later
//
// One parameter, rendered from its declaration.
//
// Nothing in this file knows what an asset is, what a slot is, or what any
// parameter is called. It is handed a `ControlSpec` — which controls.ts derived
// from the authored `(edit ...)` hint — and a value, and it renders. That is
// what makes #951's criterion true rather than merely intended: there is no
// place here where a per-asset special case could be written.
//
// ## Editing, and why a local draft exists
//
// The engine owns the value and this panel does not (#944, Q1). But a text
// field the reader is mid-way through typing into cannot be re-rendered from
// the engine on every frame — "0." would become "0" the instant it round-
// tripped, and a negative number could never be typed at all.
//
// So a draft is held for exactly as long as a field has focus, and it is a
// draft of the *text*, not of the value. The engine is still the only store;
// this is the same reason a browser does not rewrite a URL bar while you type
// in it. #951 states the requirement directly: engine-side changes update
// displayed values "without stealing focus or overwriting in-progress typing".

import { useEffect, useRef, useState } from "react";

import {
	constrain,
	fromHex,
	toHex,
	type ControlSpec,
} from "./controls.js";

export interface ParamControlProps {
	spec: ControlSpec;
	label: string;
	value: readonly number[];
	/** Called with the full component vector, already constrained. */
	onChange: (value: number[]) => void;
	/** Opens a gesture, so a drag is one undo step. Returns its end. */
	onGestureStart: () => () => void;
}

export function ParamControl({
	spec,
	label,
	value,
	onChange,
	onGestureStart,
}: ParamControlProps): React.JSX.Element {
	if (spec.kind === "color") {
		return (
			<ColorControl
				label={label}
				value={value}
				onChange={onChange}
				onGestureStart={onGestureStart}
			/>
		);
	}

	return (
		<div className="param" data-testid={`param-${label}`}>
			<span className="param__label" id={`param-label-${label}`}>
				{label}
			</span>
			<div className="param__inputs">
				{Array.from({ length: spec.components }, (_, i) => (
					<Scalar
						key={i}
						spec={spec}
						label={label}
						index={i}
						value={value[i] ?? 0}
						onChange={(next) => {
							const merged = [...value];
							merged[i] = next;
							onChange(merged);
						}}
						onGestureStart={onGestureStart}
					/>
				))}
			</div>
		</div>
	);
}

/**
 * One component: a slider when the declaration gave bounds, a number otherwise.
 *
 * Both are the same control with a different input type, deliberately — the
 * draft handling, the constraining and the gesture bracketing are identical,
 * and two components would be two places for them to drift.
 */
function Scalar({
	spec,
	label,
	index,
	value,
	onChange,
	onGestureStart,
}: {
	spec: ControlSpec;
	label: string;
	index: number;
	value: number;
	onChange: (value: number) => void;
	onGestureStart: () => () => void;
}): React.JSX.Element {
	const [draft, setDraft] = useState<string | null>(null);
	const gesture = useRef<(() => void) | null>(null);

	/* An abandoned gesture must not stay open — a component can unmount
	 * mid-drag when the selection changes under it. */
	useEffect(() => () => gesture.current?.(), []);

	const begin = (): void => {
		gesture.current ??= onGestureStart();
	};
	const end = (): void => {
		gesture.current?.();
		gesture.current = null;
	};

	const commit = (text: string): void => {
		const parsed = Number.parseFloat(text);
		/*
		 * Rejected at the control, with the field left showing what the
		 * reader typed so they can see what was wrong with it. #951 asks
		 * for the reason to be visible; the invalid state is the reason.
		 */
		if (!Number.isFinite(parsed)) return;
		onChange(constrain(spec, parsed));
	};

	const shown = draft ?? String(round(value));
	const invalid = draft !== null && !Number.isFinite(Number.parseFloat(draft));
	const name = spec.components > 1 ? `${label} ${AXES[index] ?? index}` : label;

	if (spec.kind === "slider") {
		return (
			<input
				className="param__slider"
				data-testid={`param-input-${label}-${index}`}
				type="range"
				aria-label={name}
				min={spec.min}
				max={spec.max}
				step={spec.integral ? 1 : "any"}
				value={value}
				onPointerDown={begin}
				onPointerUp={end}
				onKeyDown={begin}
				onKeyUp={end}
				onChange={(event) =>
					onChange(constrain(spec, Number.parseFloat(event.target.value)))
				}
			/>
		);
	}

	return (
		<input
			className={`param__number${invalid ? " is-invalid" : ""}`}
			data-testid={`param-input-${label}-${index}`}
			type="text"
			inputMode="decimal"
			aria-label={name}
			aria-invalid={invalid || undefined}
			value={shown}
			onFocus={begin}
			onChange={(event) => {
				setDraft(event.target.value);
				commit(event.target.value);
			}}
			onBlur={() => {
				setDraft(null);
				end();
			}}
		/>
	);
}

function ColorControl({
	label,
	value,
	onChange,
	onGestureStart,
}: {
	label: string;
	value: readonly number[];
	onChange: (value: number[]) => void;
	onGestureStart: () => () => void;
}): React.JSX.Element {
	const gesture = useRef<(() => void) | null>(null);

	useEffect(() => () => gesture.current?.(), []);

	return (
		<div className="param" data-testid={`param-${label}`}>
			<span className="param__label">{label}</span>
			<input
				className="param__color"
				data-testid={`param-input-${label}-0`}
				type="color"
				aria-label={label}
				value={toHex(value)}
				onFocus={() => {
					gesture.current ??= onGestureStart();
				}}
				onBlur={() => {
					gesture.current?.();
					gesture.current = null;
				}}
				onChange={(event) => {
					const rgb = fromHex(event.target.value);
					if (!rgb) return;
					/*
					 * A fourth component is preserved rather than dropped: the
					 * picker edits three channels and alpha is not its business,
					 * but writing the field means writing all of it.
					 */
					const merged = [...value];
					merged[0] = rgb[0];
					merged[1] = rgb[1];
					merged[2] = rgb[2];
					onChange(merged);
				}}
			/>
		</div>
	);
}

const AXES = ["x", "y", "z", "w"];

/* Enough digits to be honest, few enough to read. The engine keeps the float. */
function round(value: number): number {
	return Number.isFinite(value) ? Math.round(value * 1e6) / 1e6 : 0;
}
