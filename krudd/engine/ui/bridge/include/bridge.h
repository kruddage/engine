/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BRIDGE_H
#define BRIDGE_H

#include "edit_api.h"
#include "entity_api.h"

#include <stdint.h>

/*
 * bridge — the editor boundary (#945).
 *
 * One crossing per frame, in both directions. The editor writes a tape of
 * commands and queries into a buffer this module owns, calls exchange() once,
 * and gets one JSON document back carrying the generation vector, any discrete
 * events, and the answer to every query in the tape. Nothing else crosses.
 *
 * ## Why the two directions are shaped differently
 *
 * They are asymmetric on purpose, each in the direction that is cheap.
 *
 * Inbound is a **binary tape**. It is written by one encoder in TypeScript and
 * read by one decoder here, so there is no JSON parser in this tree and no
 * allocation on the hot path. The format belongs to this boundary — it is not
 * a view of any C struct — so it couples the editor to a wire format we own
 * rather than to the layout of `struct world`, which is the coupling #944's Q1
 * rejected when it rejected direct heap views.
 *
 * Outbound is **JSON**. It is written once per frame, read by a runtime with a
 * native parser, and is the half a human has to be able to read in a devtools
 * pane at 2am. script_layout_json already established that a JSON document is
 * how this engine talks outward, and this is the same act with a reply channel.
 *
 * ## Generations, and why the reads are cheap anyway
 *
 * The editor is a view over this document (#944, Q1) and holds no copy of it.
 * That would mean re-serializing the scene every frame, so every query carries
 * the generation the caller already has: when it still matches, the result
 * comes back `"fresh": true` with no value attached and the caller reuses what
 * it cached. It is an ETag, and it is what makes a JSON read model affordable.
 *
 * A generation moves when the domain's fingerprint changes — see
 * bridge_refresh(). Fingerprinting rather than notification is a deliberate
 * trade: it costs O(live entities) once per frame, and in exchange **every**
 * change is seen, including the ones the editor did not make (a C-side gizmo
 * drag, a script moving an entity, a game loading a scene). #944 requires
 * exactly that, and a notify-on-mutate design would have to reach into every
 * mutator in world/ to get it. If the hash ever shows up in a profile, the fix
 * is a touch() hook on this subsystem and the fingerprint becomes the fallback
 * for the paths that do not call it.
 *
 * ## What this module is not
 *
 * It is not a second history. `world/edit`'s ring is the only undo stack in the
 * engine (#944, Q2), and undo/redo are ordinary commands on the tape. It is not
 * a scene mutator either: every command lands on the `scene` and `edit` service
 * vtables, so an edit made from the editor is indistinguishable from one made
 * in C, and gets the same undo entry.
 */

/* Wire version. Bumped when the tape or the reply document changes shape. */
#define BRIDGE_PROTOCOL 1

/* 'K' 'B' 'R' 'G' little-endian — the tape's first four bytes. */
#define BRIDGE_TAPE_MAGIC 0x47524248u

/*
 * Domains a generation is kept for. One counter each rather than one global
 * counter, so a selection change does not invalidate a cached scene tree.
 */
enum bridge_domain {
	BRIDGE_SCENE = 0,
	BRIDGE_SELECTION,
	BRIDGE_HISTORY,
	BRIDGE_DOMAIN_COUNT
};

/*
 * Tape opcodes. Commands mutate and answer nothing; queries answer and mutate
 * nothing. They share one tape, and therefore one decoder, because they share
 * one crossing — splitting them into two calls would double the boundary's
 * only real cost to buy a distinction the reply document already draws.
 *
 * The numbering is grouped, sparse and permanent: an opcode is never reused
 * for a different meaning, because a stale editor bundle in someone's cache
 * would then drive the wrong mutator rather than fail.
 */
enum bridge_op {
	/* Gestures — one user action becomes one undo entry. */
	BRIDGE_OP_GESTURE_BEGIN		= 0x0001,
	BRIDGE_OP_GESTURE_COMMIT	= 0x0002,
	BRIDGE_OP_GESTURE_ABORT		= 0x0003,

	/* History. Ordinary commands: the stack they drive is C's. */
	BRIDGE_OP_UNDO			= 0x0010,
	BRIDGE_OP_REDO			= 0x0011,

	BRIDGE_OP_SELECT		= 0x0020,

	BRIDGE_OP_ENTITY_CREATE		= 0x0030,
	BRIDGE_OP_ENTITY_DESTROY	= 0x0031,
	BRIDGE_OP_ENTITY_TRANSFORM	= 0x0032,
	BRIDGE_OP_ENTITY_NAME		= 0x0033,
	BRIDGE_OP_ENTITY_RENDER_REF	= 0x0034,
	BRIDGE_OP_ENTITY_MATERIAL_REF	= 0x0035,
	BRIDGE_OP_ENTITY_SCRIPT_REF	= 0x0036,

	BRIDGE_OP_SET_PAUSED		= 0x0040,

	/*
	 * Replace the world with the (scene ...) form on the tape. The document
	 * half of open-a-project, and not an undoable edit: loading a scene is
	 * arriving at a new document rather than changing the current one, so
	 * it clears the history instead of landing on it.
	 */
	BRIDGE_OP_SCENE_LOAD		= 0x0050,

	/* Queries. Each carries the generation the caller already holds. */
	BRIDGE_OP_QUERY_TREE		= 0x0100,
	BRIDGE_OP_QUERY_ENTITY		= 0x0101,
	BRIDGE_OP_QUERY_SELECTION	= 0x0102,
	BRIDGE_OP_QUERY_HISTORY		= 0x0103,
	/*
	 * The world as (scene ...) text — the answer to Save.
	 *
	 * A query rather than a command with a reply channel, because it is a
	 * read of engine state and the generation machinery already makes reads
	 * cheap. It is nonetheless the one query no panel should watch: the
	 * answer is the whole scene serialized, and re-serializing it on every
	 * scene-generation bump to feed a cache nobody reads would turn a cold
	 * path into a per-edit one. The client asks for it once, when the
	 * reader saves.
	 */
	BRIDGE_OP_QUERY_SCENE_TEXT	= 0x0104
};

/*
 * A transform on the tape: ten little-endian float32s, in the order position
 * xyz, rotation xyzw, scale xyz. It is the wire's shape, not `struct
 * transform`'s — they agree today and are not required to keep agreeing.
 */
#define BRIDGE_TAPE_TRANSFORM_BYTES 40

/* Why a tape was rejected whole. Reported as "error" in the reply. */
enum bridge_status {
	BRIDGE_OK		= 0,
	BRIDGE_ERR_MAGIC	= -1,	/* not a tape, or byte order is wrong */
	BRIDGE_ERR_TRUNCATED	= -2,	/* a record runs past the tape's end */
	BRIDGE_ERR_OPCODE	= -3,	/* an opcode this build does not know */
	BRIDGE_ERR_PAYLOAD	= -4,	/* a record's payload is the wrong size */
	BRIDGE_ERR_OVERFLOW	= -5	/* the reply would not fit the buffer */
};

/*
 * The engine services a bridge drives. Injected rather than looked up so the
 * native test can drive the whole boundary against a hand-built world and a
 * fake history, with no subsystem manager and no plugins in the picture.
 */
struct bridge_host {
	const struct entity_api	*entity;
	const struct edit_api	*edit;
};

/* Discrete notices — the part of the outbound stream a generation cannot say. */
#define BRIDGE_MAX_EVENTS	64
#define BRIDGE_EVENT_BYTES	192

struct bridge_event {
	/* Interned literal, e.g. "log" or "command.rejected". */
	const char	*type;
	int32_t		code;
	char		text[BRIDGE_EVENT_BYTES];
};

/*
 * Reply buffer. Fixed, and sized for a full scene tree at
 * WORLD_MAX_ENTITIES — a reply that does not fit is an error rather than a
 * truncation, because a silently short JSON document is a parse failure in the
 * editor with no clue attached.
 */
#define BRIDGE_REPLY_BYTES	(768 * 1024)

/*
 * Inbound tape buffer. The editor writes here through the wasm heap; there is
 * no allocator on this path, by design.
 */
#define BRIDGE_TAPE_BYTES	(64 * 1024)

/*
 * Interned gesture labels.
 *
 * edit_api's contract is that a label outlives the history entry that holds it
 * — every C caller passes a string literal. A label arriving on the tape is
 * transient, so it is copied into a slot here and the slot is what the history
 * keeps. The ring is as deep as the history is (128 entries), so a label cannot
 * be recycled while an entry still points at it: the entry that would have
 * outlived it was dropped from the ring first.
 */
#define BRIDGE_LABEL_SLOTS	128
#define BRIDGE_LABEL_BYTES	48

/*
 * Scratch for scene text, in both directions.
 *
 * One buffer rather than two, because the two uses cannot overlap: a load
 * decodes into it during the command pass, and a save serializes into it
 * during the query pass, which always runs afterwards. A batch carrying both
 * loads the scene and then saves the world it produced, which is the only
 * meaning that ordering could have.
 *
 * Sized for a full scene rather than for the tape: a load is bounded by
 * BRIDGE_TAPE_BYTES on the way in, but a save has to hold every entity in a
 * world at WORLD_MAX_ENTITIES. Undersizing it would make Save fail on exactly
 * the large levels that most need saving.
 *
 * A load is therefore capped by the tape rather than by this: a scene form
 * larger than BRIDGE_TAPE_BYTES cannot be sent at all, and the client refuses
 * the flush and says so rather than delivering a scene cut in half.
 */
#define BRIDGE_SCENE_TEXT_BYTES	(512 * 1024)

struct bridge {
	struct bridge_host	host;

	uint32_t	generation[BRIDGE_DOMAIN_COUNT];
	uint64_t	fingerprint[BRIDGE_DOMAIN_COUNT];
	int32_t		primed;		/* fingerprints seeded yet? */

	uint32_t	serial;		/* exchanges served, for ordering */
	int32_t		gesture_depth;	/* open begin() without a commit */

	char		labels[BRIDGE_LABEL_SLOTS][BRIDGE_LABEL_BYTES];
	int32_t		label_next;

	struct bridge_event	events[BRIDGE_MAX_EVENTS];
	int32_t			event_count;
	int32_t			events_dropped;
	/*
	 * How many of `events` the reply being built has already written.
	 *
	 * The envelope — events included — is written before the query pass
	 * runs, so a notice a query emits arrives after its own reply has been
	 * serialized. Draining the whole list at the end of the exchange would
	 * throw those away unread; draining exactly this many carries them to
	 * the next reply, which is what bridge_emit promises.
	 */
	int32_t			events_sent;

	char		scene_text[BRIDGE_SCENE_TEXT_BYTES];

	uint8_t		tape[BRIDGE_TAPE_BYTES];
	char		reply[BRIDGE_REPLY_BYTES];
	int32_t		reply_len;
	int32_t		reply_overflow;
};

/* Zero the bridge and bind the services it drives. Safe to call again. */
void bridge_init(struct bridge *b, const struct bridge_host *host);

/*
 * Serve one crossing.
 *
 * Applies every command in `tape` in order, refreshes the generation vector,
 * answers every query against the refreshed state, and writes the reply
 * document. Returns a NUL-terminated JSON string owned by `b`, valid until the
 * next exchange.
 *
 * A malformed tape is rejected whole and reported in the reply's "error" —
 * partially applying a batch would leave the editor's view and the engine's
 * document disagreeing with no way to tell that they do.
 */
const char *bridge_exchange(struct bridge *b, const uint8_t *tape,
			    int32_t len);

/*
 * Queue a notice for the next reply. Overflow is counted, not dropped
 * silently: the reply carries "eventsDropped" so a console that missed lines
 * can say so rather than quietly showing fewer.
 */
void bridge_emit(struct bridge *b, const char *type, int32_t code,
		 const char *text);

/*
 * Recompute the fingerprints and bump the generations that moved. Called by
 * exchange(); exposed for the test, which asserts that a change made behind
 * the bridge's back is still seen.
 */
void bridge_refresh(struct bridge *b);

/* The current generation for one domain. */
uint32_t bridge_generation(const struct bridge *b, enum bridge_domain d);

#endif /* BRIDGE_H */
