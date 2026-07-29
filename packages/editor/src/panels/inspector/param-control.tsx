// SPDX-License-Identifier: GPL-2.0-or-later
//
// One parameter, rendered from its declaration.
//
// Nothing in this file knows what an asset is, what a slot is, or what any
// parameter is called. It is handed a `ControlSpec` — which controls.ts derived
// from the authored `(edit ...)` hint — and a value, and it renders. That is
// what makes #951's criterion true rather than merely intended: there is no
// place here where a per-asset special case could be written. It does not know
// how many entities are selected either: one value arrives, one value goes
// back, and merge.ts fans the write out.
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
//
// ## Why a refusal is a sentence and not a red border
//
// #951 asks that a bad value be "rejected at the control, with the reason
// visible". A border colour is not a reason, and it is invisible to a reader
// using a screen reader or one of the several common forms of colour blindness.
// So every refusal renders as text, tied to its input by aria-describedby, and
// the colour is decoration on top of it.
//
// ## Drag-to-scrub
//
// The issue calls it "most of what makes a property grid feel like an editor",
// and the whole of it is here: a press on an *unfocused* number field that
// travels far enough becomes a drag on the value. Focused is left alone
// deliberately — a field being typed in is a text field, and taking away
// select-by-dragging inside it would cost more than the scrub gains. A press
// that never travels focuses the field, so click-to-type still works.

import { useEffect, useId, useRef, useState } from "react";

import { constrain, fromHex, toHex, type ControlSpec } from "./controls.js";

export interface ParamControlProps {
	spec: ControlSpec;
	label: string;
	value: readonly number[];
	/**
	 * True when the selection does not agree on this value.
	 *
	 * Required rather than optional: a control that silently defaults to
	 * "they all agree" is exactly the failure #951 names — differing values
	 * shown as the first one.
	 */
	mixed: boolean;
	/** Called with the full component vector, already constrained. */
	onChange: (value: number[]) => void;
	/** Opens a gesture, so a drag is one undo step. Returns its end. */
	onGestureStart: () => () => void;
}

export function ParamControl({
	spec,
	label,
	value,
	mixed,
	onChange,
	onGestureStart,
}: ParamControlProps): React.JSX.Element {
	if (spec.kind === "color") {
		return (
			<ColorControl
				label={label}
				value={value}
				mixed={mixed}
				onChange={onChange}
				onGestureStart={onGestureStart}
			/>
		);
	}

	return (
		<div className="param" data-testid={`param-${label}`} data-mixed={mixed || undefined}>
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
						mixed={mixed}
						onChange={(next) => {
							/*
							 * A write carries the whole vector, so the
							 * components the reader did not touch have to be
							 * the ones already there — the boundary rebuilds
							 * the block from what it is handed.
							 */
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
 * One component: a number entry, and a slider in front of it when the
 * declaration gave bounds.
 *
 * The entry is always there, and that is not decoration. A range input has no
 * way to show a value outside its bounds — the thumb sits at the bound and the
 * reader is told 5 is 1 — so a slider alone would make the engine's own answer
 * a lie the moment a script or an older project put a value out of range. It is
 * also the only place an out-of-range value can be *typed*, which is what #951
 * asks be rejected with a visible reason.
 *
 * Both inputs share the draft, the gesture and the constraining rather than
 * carrying their own, because two copies of those are two places for them to
 * drift.
 */
function Scalar({
	spec,
	label,
	index,
	value,
	mixed,
	onChange,
	onGestureStart,
}: {
	spec: ControlSpec;
	label: string;
	index: number;
	value: number;
	mixed: boolean;
	onChange: (value: number) => void;
	onGestureStart: () => () => void;
}): React.JSX.Element {
	const [draft, setDraft] = useState<string | null>(null);
	const [refusal, setRefusal] = useState<string | null>(null);
	const gesture = useRef<(() => void) | null>(null);
	const focused = useRef(false);
	const scrub = useRef<Scrub | null>(null);
	const noteId = useId();

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

	/**
	 * Take a typed value, or say why not.
	 *
	 * Three outcomes and they are not the same: text that is not a number
	 * cannot be applied at all, a number outside the authored bounds is
	 * applied at the bound, and a fraction typed into an int is applied
	 * rounded. Saying which happened is the difference between a control that
	 * looks broken and one that explains itself. The rules themselves stay in
	 * `constrain` — this reports on them rather than repeating them.
	 */
	const commit = (text: string): void => {
		const parsed = Number.parseFloat(text);
		if (!Number.isFinite(parsed)) {
			/* An empty field is on the way to a value, not a refusal. */
			setRefusal(text.trim() === "" ? null : "not a number");
			return;
		}
		setRefusal(refusalFor(spec, parsed));
		onChange(constrain(spec, parsed));
	};

	const name = spec.components > 1 ? `${label} ${AXES[index] ?? index}` : label;
	/*
	 * What the row has to say about itself, most urgent first: what the reader
	 * just typed and why it did not take, then what is wrong with the value the
	 * engine holds, then that the selection does not agree on it. Only one is
	 * shown — a stack of notes under every field is a panel nobody reads.
	 */
	const standing = draft === null && !mixed ? outsideBounds(spec, value) : null;
	const note =
		refusal ?? standing ?? (mixed && draft === null ? "mixed" : null);

	const endScrub = (event: React.PointerEvent<HTMLInputElement>): void => {
		const state = scrub.current;
		scrub.current = null;
		if (state === null) return;
		if (state.active) {
			release(event.currentTarget, event.pointerId);
			end();
			return;
		}
		/* Never travelled: this was a click, and a click on a number field
		 * means "let me type in it". The default focus was suppressed at
		 * pointerdown so the drag would not select text, so it happens here. */
		event.currentTarget.focus();
	};

	const slider =
		spec.kind === "slider" ? (
			<input
				className="param__slider"
				data-testid={`param-input-${label}-${index}`}
				data-mixed={mixed || undefined}
				type="range"
				/*
				 * The slider is the same control as the entry beside it, so it
				 * is labelled once and hidden from the reading order — a screen
				 * reader that announced both would announce this parameter
				 * twice. The entry is the labelled one, because it is the one
				 * that can hold every value.
				 */
				aria-hidden="true"
				tabIndex={-1}
				min={spec.min}
				max={spec.max}
				step={spec.integral ? 1 : "any"}
				/*
				 * Clamped for the thumb's sake, and it is the one place the
				 * display is not the engine's answer: a range input cannot
				 * render a value outside its bounds. The entry beside it shows
				 * the true one, with the note saying so.
				 */
				value={constrain(spec, value)}
				onPointerDown={begin}
				onPointerUp={end}
				onKeyDown={begin}
				onKeyUp={end}
				onChange={(event) => {
					begin();
					onChange(constrain(spec, Number.parseFloat(event.target.value)));
				}}
			/>
		) : null;

	const entry = (
		<input
			className={`param__number${refusal === null ? "" : " is-invalid"}`}
			data-testid={
				spec.kind === "slider"
					? `param-entry-${label}-${index}`
					: `param-input-${label}-${index}`
			}
			data-mixed={mixed || undefined}
			type="text"
			inputMode="decimal"
			aria-label={name}
			aria-invalid={refusal !== null || undefined}
			aria-describedby={note === null ? undefined : noteId}
			/*
			 * Mixed shows nothing rather than one of the values. An em
			 * dash as the placeholder says the field is empty on purpose;
			 * typing into it sets every selected entity.
			 */
			value={draft ?? (mixed ? "" : String(round(value)))}
			placeholder={mixed ? "—" : undefined}
			onFocus={() => {
				focused.current = true;
				begin();
			}}
			onChange={(event) => {
				begin();
				setDraft(event.target.value);
				commit(event.target.value);
			}}
			onBlur={() => {
				focused.current = false;
				setDraft(null);
				setRefusal(null);
				end();
			}}
			onPointerDown={(event) => {
				/*
				 * Touch is left entirely alone: a scrub and a scroll begin
				 * with the same gesture, and a panel the reader cannot
				 * scroll is a worse trade than no scrubbing on a phone.
				 */
				if (event.pointerType === "touch") return;
				if (event.button > 0) return;
				if (focused.current) return;
				event.preventDefault();
				scrub.current = {
					pointer: event.pointerId,
					x: event.clientX,
					from: value,
					active: false,
				};
			}}
			onPointerMove={(event) => {
				const state = scrub.current;
				if (state === null || state.pointer !== event.pointerId) return;
				const dx = event.clientX - state.x;
				if (!state.active) {
					/* A press that has not travelled is still a click. */
					if (Math.abs(dx) < SCRUB_THRESHOLD) return;
					state.active = true;
					capture(event.currentTarget, event.pointerId);
					begin();
				}
				onChange(constrain(spec, state.from + dx * scrubStep(spec)));
			}}
			onPointerUp={endScrub}
			onPointerCancel={endScrub}
		/>
	);

	return (
		<span className="param__field">
			<span className="param__pair">
				{slider}
				{entry}
			</span>
			<Note id={noteId} note={note} refused={refusal !== null} />
		</span>
	);
}

function Note({
	id,
	note,
	refused,
}: {
	id: string;
	note: string | null;
	refused: boolean;
}): React.JSX.Element | null {
	if (note === null) return null;
	return (
		<span
			className={`param__note${refused ? " is-refused" : ""}`}
			id={id}
			/* A refusal is news and is announced; "mixed" is a standing fact
			 * about what is selected and would be noise read out on every
			 * arrow-key move through the panel. */
			role={refused ? "status" : undefined}
		>
			{note}
		</span>
	);
}

function ColorControl({
	label,
	value,
	mixed,
	onChange,
	onGestureStart,
}: {
	label: string;
	value: readonly number[];
	mixed: boolean;
	onChange: (value: number[]) => void;
	onGestureStart: () => () => void;
}): React.JSX.Element {
	const gesture = useRef<(() => void) | null>(null);
	const noteId = useId();

	useEffect(() => () => gesture.current?.(), []);

	return (
		<div className="param" data-testid={`param-${label}`} data-mixed={mixed || undefined}>
			<span className="param__label">{label}</span>
			<span className="param__field">
				<input
					className="param__color"
					data-testid={`param-input-${label}-0`}
					data-mixed={mixed || undefined}
					type="color"
					aria-label={label}
					aria-describedby={mixed ? noteId : undefined}
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
				<Note id={noteId} note={mixed ? "mixed" : null} refused={false} />
			</span>
		</div>
	);
}

interface Scrub {
	pointer: number;
	/** Where the press started, and the value it started from. */
	x: number;
	from: number;
	/** False until the press has travelled far enough to be a drag. */
	active: boolean;
}

/* Pixels of travel before a press stops being a click and becomes a scrub. */
const SCRUB_THRESHOLD = 3;

/**
 * How much one pixel of travel is worth.
 *
 * A slider gets its sensitivity from its declared bounds; a number field has
 * none to get it from, so these are the two honest defaults — a hundredth for a
 * float, and four pixels per step for an int, which is slow enough that a count
 * of segments can be landed on exactly.
 */
function scrubStep(spec: ControlSpec): number {
	return spec.integral ? 0.25 : 0.01;
}

/*
 * Pointer capture, where it exists. jsdom implements neither call, and a
 * browser can refuse a capture for a pointer that has already been released —
 * neither is a reason to lose the drag, which works through the element's own
 * handlers regardless. Capture only makes it survive the pointer leaving.
 */
function capture(node: HTMLInputElement, pointer: number): void {
	try {
		node.setPointerCapture(pointer);
	} catch {
		/* no capture, and the scrub carries on without it */
	}
}

function release(node: HTMLInputElement, pointer: number): void {
	try {
		node.releasePointerCapture(pointer);
	} catch {
		/* already released, by the browser or by never having been taken */
	}
}

/**
 * What to say about a value the control had to change on its way through.
 *
 * Null when it passed unaltered. The conditions are asked of the declaration
 * rather than of `constrain`'s answer, because "3.7 became 4" and "5 became 1"
 * are indistinguishable after the fact and the reader needs to know which
 * happened.
 */
function refusalFor(spec: ControlSpec, value: number): string | null {
	const outside = outsideBounds(spec, value);
	if (outside !== null) return `${outside}, clamped`;
	if (spec.integral && !Number.isInteger(value)) return "rounded to a whole number";
	return null;
}

/**
 * Whether a value sits outside its declared range, said as a phrase.
 *
 * Asked of the engine's own value too, not only of typed text. Nothing stops a
 * script or a project written against an older asset from holding a value the
 * declaration no longer allows, and a panel that quietly showed it at the bound
 * would be reporting a number the engine does not have.
 */
function outsideBounds(spec: ControlSpec, value: number): string | null {
	const { min, max } = spec;
	if (min === undefined || max === undefined) return null;
	if (value >= min && value <= max) return null;
	return `outside ${round(min)}–${round(max)}`;
}

const AXES = ["x", "y", "z", "w"];

/* Enough digits to be honest, few enough to read. The engine keeps the float. */
function round(value: number): number {
	return Number.isFinite(value) ? Math.round(value * 1e6) / 1e6 : 0;
}
