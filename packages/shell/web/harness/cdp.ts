// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * A minimal DevTools Protocol client over Node's built-in `WebSocket`.
 *
 * Node has shipped a spec `WebSocket` since 22, with no flag and no package —
 * so this *is* the transport: no `chrome-remote-interface`, no
 * `puppeteer-core`. See `docs/toolchain.md`'s "no dependencies" rule and
 * `render-test.ts`'s header for what that constrains and why the pieces this
 * file does not implement (target discovery, session flattening) are not
 * needed here: Chromium is launched pointed at exactly one page, so there is
 * exactly one target and its own `webSocketDebuggerUrl` talks Page/Runtime
 * commands directly, with no `sessionId` bookkeeping.
 *
 * Requests and responses are correlated by `id`. Anything else — a console
 * message, an uncaught exception — arrives with a `method` and no matching
 * `id`, and is dispatched to whoever subscribed with `on`.
 */

/** One pending `send()`, waiting for its response frame. */
interface PendingCall {
	resolve(value: unknown): void;
	reject(reason: Error): void;
}

/** A CDP wire frame, however it turns out to be shaped. */
interface CdpFrame {
	id?: number;
	method?: string;
	params?: unknown;
	result?: unknown;
	error?: { message: string };
}

export class CdpClient {
	readonly #socket: WebSocket;
	readonly #opened: Promise<void>;
	#nextId = 1;
	readonly #pending = new Map<number, PendingCall>();
	readonly #listeners = new Map<string, Set<(params: unknown) => void>>();

	constructor(webSocketDebuggerUrl: string) {
		this.#socket = new WebSocket(webSocketDebuggerUrl);
		this.#opened = new Promise((resolve, reject) => {
			this.#socket.addEventListener("open", () => resolve(), { once: true });
			this.#socket.addEventListener(
				"error",
				() => reject(new Error(`could not connect to ${webSocketDebuggerUrl}`)),
				{ once: true },
			);
		});
		this.#socket.addEventListener("message", (event) => {
			// Cast rather than import `MessageEvent`: this file is typechecked
			// with both `dom` (inherited) and `@types/node` in scope, and the
			// two disagree about several overlapping globals `WebSocket` pulls
			// in. Narrowing structurally to the one field this needs sidesteps
			// the disagreement entirely rather than picking a side.
			const data = (event as { data: unknown }).data;
			if (typeof data === "string") {
				this.#dispatch(JSON.parse(data) as CdpFrame);
			}
		});
	}

	/** Resolves once the transport is open, or rejects if it never connects. */
	async ready(): Promise<void> {
		await this.#opened;
	}

	/** Sends one CDP command and resolves with its `result`. */
	async send<T = unknown>(
		method: string,
		params: Record<string, unknown> = {},
	): Promise<T> {
		await this.#opened;
		const id = this.#nextId++;
		const promise = new Promise<T>((resolve, reject) => {
			this.#pending.set(id, {
				resolve: resolve as (value: unknown) => void,
				reject,
			});
		});
		this.#socket.send(JSON.stringify({ id, method, params }));
		return promise;
	}

	/** Subscribes to a CDP event by method name, e.g. `Runtime.exceptionThrown`. */
	on(method: string, handler: (params: unknown) => void): void {
		let handlers = this.#listeners.get(method);
		if (handlers === undefined) {
			handlers = new Set();
			this.#listeners.set(method, handlers);
		}
		handlers.add(handler);
	}

	close(): void {
		this.#socket.close();
	}

	#dispatch(frame: CdpFrame): void {
		if (frame.id !== undefined) {
			const pending = this.#pending.get(frame.id);
			if (pending === undefined) {
				return;
			}
			this.#pending.delete(frame.id);
			if (frame.error !== undefined) {
				pending.reject(new Error(frame.error.message));
			} else {
				pending.resolve(frame.result);
			}
			return;
		}
		if (frame.method === undefined) {
			return;
		}
		const handlers = this.#listeners.get(frame.method);
		if (handlers === undefined) {
			return;
		}
		for (const handler of handlers) {
			handler(frame.params);
		}
	}
}
