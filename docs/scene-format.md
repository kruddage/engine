# Scene File Format — `.scene` v1

Binary, little-endian. Designed for WASM and native little-endian targets.
The on-disk entity records map directly onto the runtime transfer struct on
any LE platform — the decoder is a header read plus a bounded copy loop,
with no tokenizer or endianness conversion needed.

## File layout

```
[ scene_header      ]  16 bytes
[ scene_entity × N ]  N × 56 bytes  (N = scene_header.entity_count)
[ name blob        ]  scene_header.string_bytes bytes
```

## Structs

### `struct scene_header` (16 bytes)

| Offset | Type       | Field          | Notes                           |
|--------|------------|----------------|---------------------------------|
| 0      | uint8_t[4] | magic          | `"KSCN"` (0x4B 0x53 0x43 0x4E) |
| 4      | uint32_t   | version        | `1`                             |
| 8      | uint32_t   | entity_count   | number of `scene_entity` records |
| 12     | uint32_t   | string_bytes   | size of trailing name blob      |

### `struct scene_entity` (56 bytes)

| Offset | Type       | Field      | Notes                                      |
|--------|------------|------------|--------------------------------------------|
| 0      | uint32_t   | mask       | `enum component_bit` bitmask               |
| 4      | int32_t    | parent     | index of parent entity; `-1` = root        |
| 8      | float[3]   | position   | world-space translation XYZ                |
| 20     | float[4]   | rotation   | unit quaternion XYZW                       |
| 36     | float[3]   | scale      | per-axis scale XYZ                         |
| 48     | uint32_t   | name_off   | byte offset into name blob; `0xFFFFFFFF` = none |
| 52     | uint32_t   | render_ref | valid when `COMPONENT_RENDER` is set       |

### Name blob

A flat array of NUL-terminated strings packed end-to-end. `name_off` in
each entity is a byte offset from the start of the blob. The blob is
present only when `string_bytes > 0`.

## Component bits (`enum component_bit`)

| Bit | Name             | Effect                                     |
|-----|------------------|--------------------------------------------|
| 0   | COMPONENT_NAME   | `name_off` is valid; entity has a name     |
| 1   | COMPONENT_RENDER | `render_ref` is valid; entity is renderable |

Transform fields (`position`, `rotation`, `scale`) are always present.

## Constraints

- **Topological order** — every entity must appear after its parent in the
  record array (`parent_index < own_index`). The decoder rejects files that
  violate this. Root entities use `parent = -1`.
- **Magic and version** — the decoder rejects files with a wrong magic or
  an unsupported version number.
- **Truncation** — the decoder rejects files shorter than
  `sizeof(scene_header) + entity_count × 56 + string_bytes`.

## Decoder output (`struct scene`)

```c
struct scene {
    uint32_t             count;
    struct scene_entity *entities;   /* allocated; NULL when count == 0 */
    char                *names;      /* allocated; NULL when string_bytes == 0 */
};
```

The caller owns all three allocations and must free them individually:
`entities`, `names`, then the `struct scene` itself.

## Decoder plugin

The scene decoder is registered as the `.scene` codec via `asset_codec_api`
(`codec->register_codec("scene", scene_decode)`). Retrieve a decoded scene
through the asset system:

```c
asset_request("levels/intro.scene");
/* ... wait for ASSET_LOADED ... */
struct scene *s = asset_codec_get_typed("levels/intro.scene");
```

## Future

- New component types → bump `version` to 2 and add a migration path.
- Human-authored scenes → a host-side text-to-`.scene` compiler (not in
  the runtime).
- Runtime ingestion of the transfer struct into `struct world` is handled
  by the entity system (issue #153).
