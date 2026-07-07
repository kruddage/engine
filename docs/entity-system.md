# Entity System — `struct world`

The runtime entity system (issue #153) is a data-oriented store: a flat
struct-of-arrays keyed by a dense entity id, with a parent-index hierarchy. It
is the runtime counterpart of the at-rest [`.scene` format](scene-format.md) —
the decoder produces a flat transfer struct, and the entity system *ingests*
that into `struct world`'s columns.

This was the hybrid model chosen by the #152 spike: cheaper to build than a
full archetype ECS at the v1 component set, with none of a scene graph's
pointer-chasing, and it grows cleanly toward a sparse-set ECS later.

## Storage (`struct world`)

Every column is indexed by entity id. Defined in `krudd/build/ninja/plugins/include/world.h`:

| Column         | Type                | Meaning                                  |
|----------------|---------------------|------------------------------------------|
| `count`        | uint32_t            | high-water mark (not the live count)     |
| `alive[]`      | uint8_t             | `0` = tombstoned slot                    |
| `mask[]`       | uint32_t            | `enum component_bit` OR-set (from scene.h) |
| `parent[]`     | int32_t             | `-1` = root; invariant `parent < child`  |
| `local[]`      | struct transform    | authored, parent-relative                |
| `world_xform[]`| struct transform    | derived each tick                        |
| `name_off[]`   | uint32_t            | offset into `names`; `WORLD_NO_NAME` = none |
| `render_ref[]` | uint32_t            | valid iff `COMPONENT_RENDER` set         |
| `names[]`      | char                | NUL-terminated name blob                 |

A `struct transform` is `position[3]`, `rotation[4]` (quaternion xyzw),
`scale[3]`. A transform is implicit on every entity, so there is no transform
component bit — the mask mirrors the file mask exactly (`COMPONENT_NAME`,
`COMPONENT_RENDER`), which keeps ingest a near-memcpy.

Component access is a mask test plus a column index — there is no query
language at v1:

```c
if (w->mask[i] & COMPONENT_RENDER)
        draw(&w->world_xform[i], w->render_ref[i]);
```

## Lifecycle

- **Create** (`world_create_entity`) appends at `count`, so a new entity's id
  is always greater than its parent's — the topological order is preserved by
  construction. Returns `-1` if the world is full or the parent is neither
  `-1` nor a live existing entity.
- **Destroy** (`world_destroy_entity`) tombstones a slot rather than swap-
  removing it. A naive swap-remove would shift indices and corrupt every
  stored `parent` ref; tombstoning keeps all surviving ids — and their parent
  refs — valid. Destruction cascades to the whole subtree in a single forward
  sweep, again leaning on `parent < child`. Slots are not reused in v1.

## Transform hierarchy

`world_propagate_transforms` resolves every live entity's world transform in a
**single forward pass**. Because the file (and runtime creation) guarantee
`parent < child`, a parent's `world_xform` is always final before its children
are visited:

```c
for (uint32_t i = 0; i < w->count; i++) {
        if (!w->alive[i]) continue;
        int32_t p = w->parent[i];
        w->world_xform[i] = (p < 0) ? w->local[i]
                                    : compose(w->world_xform[p], w->local[i]);
}
```

`compose` does TRS composition assuming shear-free per-axis scale: scale
multiplies component-wise, rotations multiply as quaternions, and the child
origin is scaled, rotated, then translated into the parent frame.

## Subsystem integration

The plugin registers one `"scene"` subsystem. `engine_tick →
subsystem_manager_tick` calls its `tick` every frame, which runs the ordered
system list (`world_tick`); transform propagation is the first system. The
subsystem `tick` is `void (*)(void)`, so the frame delta is read from the
`"stats"` api (`stats_api.last_frame_ms`) rather than a `dt` parameter.

At init the subsystem ingests `main.scene` if the asset is available
(`asset_codec_api.get_typed`); a missing asset just leaves the world empty.
Load order in `engine.c`'s `plugins[]` puts `entity_plugin` after
`scene_plugin` (codec) and before the renderer (consumer).

## Renderer access (`struct entity_api`)

Other plugins reach the world through the `"scene"` subsystem api in
`krudd/build/ninja/plugins/include/entity_api.h`:

```c
const struct entity_api *e = subsystem_manager_get_api(mgr, "scene");
const struct world      *w = e->get_world();
/* walk w->count entities, skip alive[i] == 0, test COMPONENT_RENDER */
```

This is how the renderer (#4) draws entities from their transform and
render-ref components.

## Future

- Slot reuse / compaction (v1 tombstones permanently).
- Generational ids if handles need to outlive a slot.
- More component columns as the v1 set grows.
