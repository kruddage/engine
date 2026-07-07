# Backend Abstraction Seam — Local Provider + IndexedDB Persistence

Design note for issue #189 (part of epic #23).

This document resolves the central question of #23 **narrowly**: what is the
minimum abstraction shape that makes persistent project assets real in Local
mode, leaves an honest seam for a future Remote provider, and does not build
what it defers?

---

## Background and motivation

Every asset in the current catalog enters via `asset_request`, which always
kicks off an `emscripten_fetch` (see `krudd/build/ninja/plugins/asset/asset_plugin.c`, the
`start_fetch` / `on_fetch_success` / `on_fetch_error` pattern). There is no
path for assets that the user authors: bytes that come from a text editor, an
import drop, or a file upload. Issue #188 adds that path — a "born-loaded"
authored entry with `origin = authored`. Issue #187 gives each catalog entry a
stable `uint32_t id` so selection, mutation, and persistence never key on the
swap-removed array index.

With #187 and #188 in place, authored project assets can live for the session.
The remaining gap is that they vanish on reload. Issue #189 closes that gap
with a backend abstraction whose first — and for now only — concrete capability
is project-asset persistence via IndexedDB in Local mode.

#23's broader vision also includes a Remote provider (REST calls to a real
server), auth, messaging, and multiplayer. **None of those are in scope here.**
The seam is shaped to receive them without making them a prerequisite.

---

## 1. The abstraction seam — `backend_api`

### Placement in the subsystem graph

The backend is registered as an `async_subsystem` (see
`krudd/build/ninja/modules/core/include/async_subsystem.h`) because its init — opening the
IndexedDB database and reading persisted records — is asynchronous. It signals
readiness via the manager's `done` callback, after which the asset plugin
(or any other consumer) can query it by name:

```c
/* resolve at setup time, once; valid for the session */
const struct backend_api *be =
        subsystem_manager_get_api(mgr, "backend");
```

`subsystem_manager_get_api` (in `krudd/build/ninja/modules/core/subsystem_manager.c`) already
walks the `async_dynamic` table and returns the api pointer regardless of
readiness — readiness gating is the caller's concern, not the lookup's. The
Local provider is live after its IndexedDB open succeeds (or after it logs the
fallback and accepts in-memory-only mode).

### `backend_api` struct (as implemented in `krudd/build/ninja/plugins/include/backend_api.h`)

The design was simplified during implementation. The `struct
backend_project_record` wrapper and the separate `enumerate` / `login` pointers
were dropped in favour of a flat, minimal vtable:

```c
/*
 * Capability flags.  get_caps() returns a live OR-set of these.
 * The Local provider sets BACKEND_CAP_PROJECT_PERSIST at startup and
 * clears it if IndexedDB is unavailable (private-browsing, open error).
 * AUTH and MESSAGING are reserved for a future Remote provider.
 */
#define BACKEND_CAP_PROJECT_PERSIST  (1u << 0)
#define BACKEND_CAP_AUTH             (1u << 1)
#define BACKEND_CAP_MESSAGING        (1u << 2)

struct backend_api {
        /*
         * Return the live capability bitmask.  BACKEND_CAP_PROJECT_PERSIST
         * is cleared at runtime if IndexedDB is unavailable.
         */
        uint32_t (*get_caps)(void);

        /*
         * Insert-or-update a persisted record for an authored asset.
         * id must be non-zero.  size must be <= BACKEND_RECORD_MAX.
         * Returns 0 on success, -1 on failure (no capability, oversize,
         * null args, or IndexedDB error).
         */
        int32_t (*persist_asset)(uint32_t id, const char *path,
                                 int32_t type, const void *bytes,
                                 uint32_t size);

        /*
         * Remove a persisted record by id.
         * Returns 0 on success, -1 on failure (no capability or bad id).
         */
        int32_t (*delete_asset)(uint32_t id);
};
```

Key differences from the original design sketch:
- `get_caps()` is a function rather than a static `uint32_t caps` field — this
  allows the Local provider to reflect the live IDB state (cleared on failure)
  without callers caching a stale field.
- `persist_asset` takes flat parameters instead of a `struct
  backend_project_record` pointer; this matches the EM_JS boundary naturally.
- `enumerate` is handled inside the backend plugin's `async_init` rather than
  exposed on the vtable — consumers never call it directly.
- Auth/messaging stubs (`login`, etc.) are omitted; they will be added when the
  Remote provider track lands.

### Why a separate subsystem rather than embedding in the asset plugin

The asset plugin (`krudd/build/ninja/plugins/asset/asset_plugin.c`) is deliberately narrow: it
owns the in-memory catalog, read/write semantics for entries, and the fetch
plumbing. Backend persistence is a cross-cutting concern that will eventually
encompass auth and messaging — topics the asset plugin should not know about.
Registering `"backend"` as a separate async subsystem keeps the asset plugin's
scope stable and lets the backend be replaced or mocked independently.

---

## 2. The Local provider — IndexedDB rehydration

### Init path (async)

The Local provider's `async_init` function opens (or creates) an IndexedDB
database named `"krudd-project"` with an object store named `"assets"`, keyed
on the `uint32_t id` from #187. The open is asynchronous; the JS-side
completion handler calls back into WASM to signal done. In pseudo-sequence:

```
plugin_entry(mgr)
  subsystem_manager_register_async(mgr, &local_backend_desc)
    local_backend_async_init(done, ctx)     /* called by manager */
      idb_open_db(done, ctx)               /* EM_JS — opens IndexedDB */
        [ 1-2 frames later ]
        onsuccess → enumerate persisted records
          for each record: call asset_mut->inject(id, path, type, data, size)
          call done(ctx)                   /* signals manager: ready = 1 */
```

The `done` callback is the same `async_done` the manager sets up internally
(see `subsystem_manager_register_async` in
`krudd/build/ninja/modules/core/subsystem_manager.c`). Once called, the manager sets
`async_subsystem_slot.ready = 1` and fires any `on_ready` callbacks registered
for `"backend"`. Tick starts running from the next frame.

Rehydrated assets arrive a few frames after init. This is not a special case
— the catalog already tolerates `ASSET_PENDING` entries from in-flight
`emscripten_fetch` calls (see `asset_state_of`, `asset_get` in
`asset_plugin.c`). Authored assets injected during rehydration start as
`ASSET_LOADED` immediately (they arrive with their bytes); consumers that
poll `asset_state_of` see them as soon as the inject call completes. The
kruddboard's per-frame enumeration loop picks them up on the next frame draw.

### Mutation write-through

When the editor calls `asset_mut->create(name, type, data, size)` or
`asset_mut->set_data(id, data, size)` from #188, the asset plugin (or a thin
hook registered at init) calls `backend->persist_asset(rec)`. The Local
provider serializes the record and calls an `EM_JS` function that writes it
into IndexedDB. The write is fire-and-forget — no completion callback is
required before the editor unblocks — matching the async pattern already used
by `emscripten_fetch`.

Similarly, `asset_mut->delete(id)` triggers `backend->delete_asset(id)`, which
calls an `EM_JS` function that removes the key from the object store.

### JS↔WASM seam via `EM_JS`

The project's EM_ASM ban (`scripts/check-plugin-no-em-asm.sh`) applies. All
JS↔WASM calls use `EM_JS`. A sketch of the three entry points:

```c
/* Open the database and enumerate existing records into the catalog.
 * Calls the WASM-side rehydrate callback once per record,
 * then calls done_cb when the cursor is exhausted. */
EM_JS(void, idb_open_and_enumerate,
      (void (*rehydrate)(uint32_t id, const char *path,
                         int32_t type,
                         const uint8_t *data, uint32_t size,
                         void *ctx),
       void *rehydrate_ctx,
       void (*done_cb)(void *ctx),
       void *done_ctx),
{
        /* ... JS: indexedDB.open, onsuccess → IDBObjectStore.openCursor ... */
})

/* Write or overwrite one asset record. */
EM_JS(void, idb_persist,
      (uint32_t id, const char *path, int32_t type,
       const uint8_t *data, uint32_t size),
{
        /* ... JS: IDBObjectStore.put({ id, path, type, data: Uint8Array }) */
})

/* Delete a record by id. */
EM_JS(void, idb_delete, (uint32_t id),
{
        /* ... JS: IDBObjectStore.delete(id) */
})
```

Strings passed across the boundary (`path`) are marshalled with
`UTF8ToString` on the JS side, following the same pattern as the rest of the
engine's WASM glue. Byte arrays become `Uint8Array` views of WASM heap memory,
copied into IndexedDB as ArrayBuffer values so IndexedDB owns the storage.

---

## 3. Serialization format

Each record stored in IndexedDB is a structured JS object. The format is
**versioned** so future fields can be appended without breaking existing
stored data.

### Record shape (JS / IndexedDB side)

```json
{
  "version": 1,
  "id":      42,
  "path":    "my-notes.md",
  "type":    7,
  "data":    "<ArrayBuffer — raw asset bytes>"
}
```

| Field     | Type        | Notes                                              |
|-----------|-------------|----------------------------------------------------|
| `version` | uint32      | Schema version; currently `1`. Increment on breaking changes. |
| `id`      | uint32      | Stable catalog id from #187; also the IDBObjectStore key. |
| `path`    | string      | Asset path (up to `ASSET_PATH_MAX = 256` chars).   |
| `type`    | int32       | `ASSET_TYPE_*` constant (e.g. `ASSET_TYPE_TEXT = 7` from #188). |
| `data`    | ArrayBuffer | Raw asset bytes. Text assets store UTF-8.          |

### Forward compatibility

- **Unknown fields** in a stored record are ignored on read. New fields can
  be appended in a later version without a migration.
- **Version mismatch**: on open, records with `version > CURRENT_VERSION` are
  skipped and logged via `log_api`. Records with `version < CURRENT_VERSION`
  follow a migration path defined when that version bump happens. For now, with
  only version 1, the rule is: unknown version → skip and log.
- **Schema changes** that require restructuring (e.g. adding a new IDB index)
  bump the IDB database version passed to `indexedDB.open`, which triggers the
  browser's `onupgradeneeded` handler for existing users.

### What is deliberately not stored

- **Built-in primitives** (`ASSET_KIND_PRIMITIVE`, `read_only = 1`) — seeded
  from `seed_builtins()` at every startup; never need persistence.
- **Fetched-from-URL assets** (`ASSET_KIND_NORMAL`, origin = fetched) — these
  re-fetch from their URL on next startup via `asset_request`. Duplicating them
  in IndexedDB wastes storage and diverges from the authoritative source.
- **Error / pending entries** — only `ASSET_LOADED` authored entries are
  persisted; a persist call on a non-loaded authored entry is a no-op with a
  warning log.

The guard is the `origin = authored` flag added in #188. The persist hook
checks this before writing:

```c
/* Illustrative — in the persist hook wired up at asset plugin init */
if (info.origin != ASSET_ORIGIN_AUTHORED)
        return; /* built-in or fetched — skip */
backend->persist_asset(&rec);
```

---

## 4. Degradation — IndexedDB unavailable

Private browsing windows and some hardened browser configurations make IndexedDB
unavailable (the `indexedDB.open` call fails or the database object is absent).
The Local provider handles this gracefully:

1. During `idb_open_and_enumerate`, if the open fails, the JS side calls the
   `done_cb` directly without calling the rehydrate callback.
2. The C side detects the failure (e.g. a flag set via an `EM_JS` error path)
   and logs via `g_log->write(LOG_LEVEL_WARN, "backend: IndexedDB unavailable "
   "— running in-memory only (assets will not persist)")`.
3. `BACKEND_CAP_PROJECT_PERSIST` is cleared from the live bitmask. Callers
   that check `get_caps()` will see the capability as absent and can hide or
   disable persistence affordances accordingly (e.g. the Save button in
   kruddboard). All subsequent calls to `persist_asset` and `delete_asset`
   return -1 immediately.
4. The engine continues to run. In-memory authored assets work for the session;
   they just do not survive a reload.

No crash, no assertion, no silent data loss — the user's authored work for the
current session is preserved in memory, and the log makes the situation clear.

---

## 5. What stays natively unit-testable

The IndexedDB path is intrinsically browser-only — `EM_JS` functions do not
compile or run in the native test harness. The split:

### Browser-only (not unit-testable natively)
- `idb_open_and_enumerate`, `idb_persist`, `idb_delete` — the `EM_JS` wrappers
  themselves.
- The `async_init` open sequence and its callbacks.

### Natively testable (no browser required)
- **Record serialization helpers**: functions that pack a
  `struct backend_project_record` into the on-wire fields and unpack them back.
  These are pure C, have no WASM dependencies, and can be exercised in the
  native test harness the same way `asset_plugin_test.c` tests the catalog
  (using `asset_plugin_entry` under the `#else` native branch).
- **The capability bitmask logic**: checking `caps & BACKEND_CAP_*` before
  calling a function pointer.
- **The rehydrate loop itself**: if `enumerate` is factored to call the
  `rehydrate_cb` synchronously in a loop (with the actual IDB cursor replaced
  by a stub in tests), the loop logic is testable.
- **The persist guard**: the `origin == ASSET_ORIGIN_AUTHORED` check before
  calling `persist_asset` — a simple condition, but worth a test given that
  silently persisting built-ins would be a bug.

The implementation should put the serialization helpers in a separate
translation unit (e.g. `krudd/build/ninja/plugins/backend/backend_record.c`) with no `#ifdef
__EMSCRIPTEN__` body, so native tests can link it without dragging in the IDB
wrappers.

---

## 6. Scope boundary — what this issue defers

The following are explicitly deferred to later #23 tracks:

- **Remote provider** — no REST calls, no server URLs, no provider-selection
  logic. The `backend_api` vtable shape accommodates a Remote provider; it does
  not build one.
- **Auth / login screen** — `backend_api.login` is listed as a stub above to
  show where it belongs, but it is NULL on the Local provider and will not be
  called until the auth track lands. The ImGui login screen from #23's original
  vision is a later track.
- **Messaging / multiplayer** — `BACKEND_CAP_MESSAGING` exists in the capability
  table; no implementation is written here.
- **Export (ZIP)** — listed in #23's capability table; deferred.
- **Provider selection at runtime** — today there is one provider (Local). The
  mechanism for switching to Remote (reading a server URL from localStorage,
  connecting, swapping the vtable) is a later track.
- **Persistence of fetched-from-URL assets** — these re-fetch; caching them
  locally is a separate optimization, not part of this issue.

---

## Open questions for the maintainer

The following points should be resolved before or during implementation:

1. **`persist_asset` call site** — should the mutation hook be wired up inside
   `asset_plugin.c` (the asset plugin calls `backend->persist_asset` directly
   after modifying the entry), or via a `subsystem_manager_on_ready("backend",
   ...)` callback that the backend plugin itself registers to receive asset
   mutation events? The first is simpler but creates a dependency from the asset
   plugin on `"backend"`. The second is more decoupled but requires an event or
   observer mechanism not currently in the subsystem layer.

2. **Clearing `BACKEND_CAP_PROJECT_PERSIST` on IndexedDB failure** — resolved.
   The Local provider clears `BACKEND_CAP_PROJECT_PERSIST` from the live
   bitmask when IDB is unavailable, so callers can gate on `get_caps()` and
   hide or grey out the Save affordance. See section 4.

3. **Debounce vs. write-through** — `persist_asset` is described above as
   write-through (every edit triggers an IDB `put`). For a text editor with
   keystroke-level edits this may cause frequent IDB writes. A short debounce
   (e.g. 500 ms idle, or explicit Save action from #190) may be preferable.
   This is a UX/performance call for the editor sub-issue (#190) to make, but
   the backend API needs to know whether to expose an explicit `flush` or just
   write on every call.

4. **Max stored-record size** — IndexedDB has no hard per-record limit in the
   spec, but browsers differ. Is there a practical cap the engine should enforce
   (e.g. reject persist calls for assets over N MB) rather than silently failing
   a large IDB write?

5. **`id` wrap-around** — the stable id in #187 is a monotonic `uint32_t`.
   At `UINT32_MAX` it wraps. This is effectively impossible in practice but
   worth a comment in the implementation noting that id 0 is reserved (or
   `UINT32_MAX` is the sentinel) to avoid ambiguity with an uninitialized field.

6. **IDB database versioning strategy** — when `CURRENT_SCHEMA_VERSION` bumps
   in a future release, the `onupgradeneeded` handler must migrate or discard
   existing records. Should migration be best-effort (discard unknown-version
   records and continue) or hard-fail (clear the store and start fresh)? The
   answer affects how aggressively the format can change between releases.
