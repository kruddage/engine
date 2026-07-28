// SPDX-License-Identifier: GPL-2.0-or-later
//
// The status strip: fps, resolution, driver, and the transient hint.
//
// The three fields are the ones `editor_layout.scm` declares, in its order, and
// they keep the `status-<field>` element ids the generated shell gives them —
// the id is a documented contract with a host that updates them by id, and
// dropping it because React happens not to need it would break that contract
// silently. Where the values come from is frame-stats.ts's business, and the
// note there explains why they are measured rather than pushed.
//
// The hint is the other half of this strip and it is the part that makes the
// shell honest: an action the shell has not wired says so here, transiently,
// exactly as `shell.html.in:1548` does today.

import { driverLabel, type FrameStats } from "../engine/frame-stats.js";
import type { EnginePhase } from "../engine/types.js";

/*
 * How far the engine got, in words. The generated shell carries the same pill
 * beside its status fields — outside `editor_layout.scm`'s (statusbar ...),
 * because the spec describes fields a host updates and this one is the host's
 * own state.
 */
const PHASE_LABEL: Record<EnginePhase, string> = {
	unbuilt: "not built",
	loading: "loading…",
	running: "running",
	ready: "ready",
	failed: "failed",
};

export interface StatusStripProps {
	stats: FrameStats;
	phase: EnginePhase;
	renderer: string | null;
	hint: string | null;
}

export function StatusStrip({
	stats,
	phase,
	renderer,
	hint,
}: StatusStripProps): React.JSX.Element {
	return (
		<footer className="status-strip" data-testid="status-strip">
			<span
				className={`status-strip__phase status-strip__phase--${phase}`}
				data-testid="status-phase"
				data-phase={phase}
			>
				{PHASE_LABEL[phase]}
			</span>
			<Field id="fps" label="fps">
				{stats.fps === null ? "fps —" : `${stats.fps} fps`}
			</Field>
			<Field id="resolution" label="resolution">
				{stats.resolution === null
					? "—×—"
					: `${stats.resolution.width}×${stats.resolution.height}`}
			</Field>
			<Field id="driver" label="driver">
				{driverLabel(renderer)}
			</Field>

			<span className="status-strip__spacer" />

			{/*
			 * A live region, so the hint reaches a reader who is not looking at
			 * the bottom of the screen. "polite" rather than "assertive": it is
			 * a note about an unwired button, not an error.
			 */}
			<span
				className={`status-strip__hint${hint === null ? "" : " is-visible"}`}
				data-testid="status-hint"
				role="status"
				aria-live="polite"
			>
				{hint ?? ""}
			</span>
		</footer>
	);
}

function Field({
	id,
	label,
	children,
}: {
	id: string;
	label: string;
	children: React.ReactNode;
}): React.JSX.Element {
	return (
		<span
			className="status-strip__field"
			/* The by-id contract from `editor_layout.scm`'s (field ...) — see
			 * the note at the top of this file. */
			id={`status-${id}`}
			data-testid={`status-${id}`}
			aria-label={label}
		>
			{children}
		</span>
	);
}
