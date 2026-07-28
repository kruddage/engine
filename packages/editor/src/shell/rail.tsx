// SPDX-License-Identifier: GPL-2.0-or-later
//
// The rail: the narrow strip down the left edge that shows and hides panels.
//
// It is the View menu's toggles, one click away instead of three. That is its
// whole job — it does not launch anything the menu cannot, on purpose, because
// two paths to one action that drift apart is exactly what the vocabulary and
// the single command table exist to prevent.
//
// Not built on a package: it is a list of toggle buttons, and `aria-pressed` on
// a `<button>` is the whole accessibility story. Reaching for a component
// library here would be the kind of dependency #944's rule is meant to allow,
// not the kind it is meant to encourage.

import type { PanelDefinition, PanelId } from "./panels.js";

export interface RailProps {
	panels: PanelDefinition[];
	hidden: PanelId[];
	onToggle: (id: PanelId) => void;
}

export function Rail({ panels, hidden, onToggle }: RailProps): React.JSX.Element {
	return (
		<nav className="rail" aria-label="Panels" data-testid="rail">
			{panels
				.filter((definition) => definition.hideable !== false)
				.map((definition) => {
					const shown = !hidden.includes(definition.id);
					return (
						<button
							key={definition.id}
							type="button"
							className="rail__button"
							data-testid={`rail-${definition.id}`}
							aria-pressed={shown}
							/* The full name, because the rail shows an
							 * abbreviation and an icon-only control with no
							 * accessible name is a control nobody can use. */
							aria-label={definition.title}
							title={definition.title}
							onClick={() => onToggle(definition.id)}
						>
							{definition.title.slice(0, 2)}
						</button>
					);
				})}
		</nav>
	);
}
