// SPDX-License-Identifier: GPL-2.0-or-later
//
// What an unimplemented panel says.
//
// A heading, a line about what will be here, and the issue that will put it
// there. **It says so plainly rather than rendering an empty box** — an
// acceptance criterion of #954's, and the difference between "this is not built
// yet" and "the editor is broken", which are indistinguishable to a reader
// looking at a blank frame.
//
// The headings and blurbs are `editor_layout.scm`'s, carried over verbatim.
// They were written to describe these panels and they still do; rewriting them
// would be churn dressed as work.

export interface PlaceholderProps {
	heading: string;
	blurb: string;
	/** The issue that fills this panel in. */
	issue: number;
	children?: React.ReactNode;
}

export function Placeholder({
	heading,
	blurb,
	issue,
	children,
}: PlaceholderProps): React.JSX.Element {
	return (
		<section className="placeholder" data-testid={`placeholder-${issue}`}>
			<h2 className="placeholder__heading">{heading}</h2>
			<p className="placeholder__blurb">{blurb}</p>
			{children}
			<p className="placeholder__issue">
				Not built yet —{" "}
				<a
					href={`https://github.com/kruddage/engine/issues/${issue}`}
					rel="noreferrer"
					target="_blank"
				>
					#{issue}
				</a>
			</p>
		</section>
	);
}
