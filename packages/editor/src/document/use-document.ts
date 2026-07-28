// SPDX-License-Identifier: GPL-2.0-or-later
//
// Standing a document up over a booted engine module, and running its loop.
//
// Split from use-engine.ts because the two answer different questions and fail
// differently. useEngine asks whether the page has an engine at all — a wasm
// module that loaded, a canvas it owns, a renderer it picked. This asks whether
// that engine speaks a protocol we understand, which is a question that only
// arises once the first one is answered yes.
//
// ## The protocol check is not a formality
//
// The editor bundle and the wasm artifact are built, hashed and cached
// separately, so a browser can hold a bundle from one commit and a module from
// another — a reload after a deploy is enough. Decoding a tape the other side
// writes differently produces garbage that looks like data, so the mismatch is
// caught here and reported as a mismatch, which is the one form of it a reader
// can act on.

import { useEffect, useMemo, useState } from "react";

import {
	type BridgeModule,
	type BridgeReply,
	createBridge,
	protocolMatches,
} from "@kruddage/engine/bridge";

import { createDocument, type KruddDocument, type Scheduler } from "./document.js";

export interface DocumentRuntime {
	/** Null until the wasm runtime is up, or if its protocol is not ours. */
	document: KruddDocument | null;
	/** Why there is no document, when there is a reason worth showing. */
	error: string | null;
}

export interface UseDocumentOptions {
	module: BridgeModule | null;
	scheduler?: Scheduler | undefined;
	/** Called for every reply carrying an engine-side or client-side error. */
	onError?: ((reply: BridgeReply) => void) | undefined;
}

export function useDocumentRuntime(
	options: UseDocumentOptions
): DocumentRuntime {
	const { module, scheduler, onError } = options;
	const [error, setError] = useState<string | null>(null);

	const document = useMemo(() => {
		if (!module) return null;
		const bridge = createBridge(module, onError ? { onError } : {});
		if (!protocolMatches(bridge)) {
			setError(
				`the engine speaks bridge protocol ${bridge.protocol} and this ` +
					`editor speaks a different one — reload to pick up a matching build`
			);
			return null;
		}
		setError(null);
		return createDocument(scheduler ? { bridge, scheduler } : { bridge });
		/*
		 * onError is deliberately not a dependency. It is a reporting sink, it
		 * changes identity on every render of whatever owns it, and rebuilding
		 * the bridge for it would throw away the generation cache — every panel
		 * would re-read the whole scene, once per render, forever.
		 */
		// eslint-disable-next-line react-hooks/exhaustive-deps
	}, [module, scheduler]);

	useEffect(() => document?.run(), [document]);

	/*
	 * Enter editor mode as soon as there is a document (#949).
	 *
	 * The flag starts at 0 in the engine, matching the shipped shell's default
	 * of no `body.editor-mode` class — the page boots straight into the scene
	 * and the reader taps into the editor. This application *is* the editor, so
	 * it says so once at boot, and everything that hangs off the flag follows:
	 * the simulation pauses, the selection outline lights, and kruddgui's
	 * click-to-pick stands down so the pointer has one owner.
	 *
	 * Here rather than in the viewport panel because it is a property of the
	 * application rather than of a panel — a reader who hides every dock is
	 * still in an editor — and because a panel that set it on mount would unset
	 * it on unmount, which is not what closing a dock should mean.
	 */
	useEffect(() => {
		document?.dispatch("engine.editing", { editing: true });
	}, [document]);

	return { document, error };
}
