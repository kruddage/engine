// SPDX-License-Identifier: GPL-2.0-or-later
//
// The size of the box a panel was given, in CSS pixels.
//
// react-arborist virtualizes, and virtualizing means knowing how tall the
// viewport is — it cannot be `height: 100%` because the library computes which
// rows exist from a number. So the one panel that needs a measurement gets one.
//
// This is not `useCanvasSize` (src/viewport/use-viewport.ts) and is deliberately
// smaller than it. That hook is about the *backing store*: device pixels, the
// DPI change that fires no resize, and a size it has to report across the
// boundary. None of that applies to a list of DOM rows, and copying it here
// would be carrying three problems this panel does not have.

import { useEffect, useState } from "react";

export interface PanelSize {
	width: number;
	height: number;
}

/**
 * Observe an element and report its content box.
 *
 * Starts at zero, which is what a panel renders before layout has happened and
 * also what a collapsed dock measures. The caller draws nothing at zero rather
 * than drawing a tree one row tall — a `ResizeObserver` fires again the moment
 * the dock opens, and a spurious empty state for one frame is invisible.
 */
export function usePanelSize(element: HTMLElement | null): PanelSize {
	const [size, setSize] = useState<PanelSize>({ width: 0, height: 0 });

	useEffect(() => {
		if (element === null) return;

		const measure = (): void => {
			const rect = element.getBoundingClientRect();
			setSize((previous) =>
				/*
				 * Rounded, and compared before it is set. An observer that
				 * published a new object every callback would re-render the
				 * whole tree on every sub-pixel of a splitter drag.
				 */
				previous.width === Math.round(rect.width) &&
				previous.height === Math.round(rect.height)
					? previous
					: {
							width: Math.round(rect.width),
							height: Math.round(rect.height),
						}
			);
		};

		measure();
		const observer = new ResizeObserver(measure);
		observer.observe(element);
		return () => observer.disconnect();
	}, [element]);

	return size;
}
