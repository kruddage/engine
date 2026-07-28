// SPDX-License-Identifier: GPL-2.0-or-later
//
// The toolbar: the renderer badge, an elastic gap, and the build identity.
//
// `editor_layout.scm` declares exactly this — `(badge "renderer" …)`,
// `(spacer)`, `(badge "version" …)` — and the shape is carried over rather than
// improved on. The spacer is what puts the version against the trailing edge,
// and it is a flex grow here for the same reason it was an elastic gap there.
//
// The renderer badge is the shell's only genuinely live chrome today, and the
// race it has to survive is real and already documented in the generated shell:
// a backend can report before the chrome exists. Here it cannot — `boot.ts`
// holds the last status it saw and replays it into whatever mounts next, so a
// badge that mounts late still shows the backend that went live rather than
// sitting on "booting…" forever.

import type { EngineStatus } from "../engine/types.js";

export interface ToolbarProps {
	status: EngineStatus;
	version: string | null;
}

export function Toolbar({ status, version }: ToolbarProps): React.JSX.Element {
	return (
		<div className="toolbar" role="toolbar" aria-label="Toolbar">
			<RendererBadge status={status} />
			<span className="toolbar__spacer" />
			<span className="badge badge--version" data-testid="badge-version">
				KRUDD Editor
				{version !== null && (
					<>
						{" "}
						{/* The bare version, on its own element: the browser
						  * suite asserts it looks like a semver, and it should
						  * not have to strip a product name off the front to
						  * do it. */}
						<span data-testid="engine-version">{version}</span>
					</>
				)}
			</span>
		</div>
	);
}

function RendererBadge({ status }: { status: EngineStatus }): React.JSX.Element {
	const failed = status.phase === "failed";
	const label =
		status.renderer === null
			? "renderer — booting…"
			: RENDERER_LABEL[status.renderer] ?? status.renderer;

	return (
		<span
			className={`badge badge--renderer${failed ? " badge--failed" : ""}`}
			data-testid="badge-renderer"
			data-renderer={status.renderer ?? ""}
			/* The failure reason, where the generated shell also puts it. A
			 * badge that says a backend is unavailable without saying why is a
			 * bug report with the useful half removed. */
			title={failed && status.error !== null ? status.error : undefined}
		>
			{label}
			{failed && " — unavailable"}
		</span>
	);
}

/* The same names the generated shell shows. Two pages driving one engine must
 * not disagree about what to call the thing that is drawing. */
const RENDERER_LABEL: Record<string, string> = {
	webgpu: "WebGPU",
	webgl: "WebGL",
};
