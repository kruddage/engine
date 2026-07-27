// SPDX-License-Identifier: GPL-2.0-or-later

/** What a consumer is allowed to do with an artifact. See src/artifacts.mjs. */
export type ArtifactRole = "entry" | "code" | "static";

export interface EngineArtifact {
	/** Build-output filename, e.g. "index.wasm". */
	name: string;
	role: ArtifactRole;
	/**
	 * Whether the file may be renamed with a cache-busting stem at deploy
	 * time. False for anything referenced by literal name.
	 */
	cacheBusting: boolean;
}

export interface BuiltArtifact extends EngineArtifact {
	/** Absolute path to the staged file. */
	path: string;
	bytes: number;
	sha256: string;
}

export interface EngineManifest {
	name: "@kruddage/engine";
	/** The version stamped into this build (KRUDD_VERSION, or version.txt). */
	version: string;
	/**
	 * The stem index.wasm must be renamed to at deploy time — read back out of
	 * the built index.html's Module.locateFile hook, not re-derived.
	 */
	cacheStem: string;
	/** The C entry points the engine intends to offer (mirrors -sEXPORTED_FUNCTIONS). */
	declaredExports: string[];
	/** The function exports actually present in index.wasm. */
	wasmExports: string[];
	/** Runtime asset directory name, or null when the build produced none. */
	assetDir: string | null;
	files: Omit<BuiltArtifact, "path">[];
}

export declare const ENGINE_ARTIFACTS: readonly (EngineArtifact & {
	required: boolean;
})[];
export declare const ENGINE_ASSET_DIR: string;
export declare const ENGINE_EXPORTED_FUNCTIONS: readonly string[];

/** Absolute path to this package's staged artifacts. */
export declare const distDir: string;

/** Insert a cache-busting stem before an artifact's extension. */
export declare function bustedName(artifact: EngineArtifact, stem: string): string;

/** Extract the Module.locateFile cache stem from a built index.html. */
export declare function bakedCacheStem(html: string): string | null;

/** Whether this package has been built. */
export declare function isBuilt(): boolean;

/** Read the built manifest. Throws with the build command if absent. */
export declare function readManifest(): EngineManifest;

/** The built artifacts, with absolute paths resolved. */
export declare function artifacts(): BuiltArtifact[];

/** Absolute path to one artifact, by build-output name. Throws if unknown. */
export declare function artifactPath(name: string): string;

/** Absolute path to the runtime asset directory, or null. */
export declare function assetDir(): string | null;
