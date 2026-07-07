# Scene renderer

The scene renderer draws the live entity world in-browser, **entirely through the
frame graph**, with a camera. It is the payoff of the rendering epic (#167): a
built-in primitive placed as a `COMPONENT_RENDER` entity shows up on screen, and
multiple entities at different transforms draw at their correct, depth-sorted
world positions.

It lives in `krudd/ninja/plugins/scene_renderer/` and registers as the `scene_renderer`
subsystem. It replaced the throwaway hardcoded triangle that `renderer_webgl`
used to draw each frame.

## A pure frame-graph consumer

The renderer never owns the GPU. It resolves `frame_graph`, `scene` (the
`entity_api`) and `asset`, and holds **no persistent `gpu_api` pointer**:

- **Persistent setup (once, against the device).** The pipeline state object and
  the primitive vertex/index buffers outlive a frame, so they are created via the
  seam documented in `docs/frame-graph.md`: the device is resolved directly
  (`subsystem_manager_get_api(mgr, "renderer")`) only inside `init`/`shutdown`,
  never held across frames.
- **Per frame.** A forward pass is declared writing the imported backbuffer. Its
  execute callback receives the command context the graph *lends* at execute time
  (`fg_ctx_gpu` / `fg_ctx_cmd`), records real draws, and lets it go — it never
  caches the device or resolves `renderer`. The graph begins and ends the render
  pass around the callback.

## Walking the world

The forward pass walks the world the documented way (see `docs/entity-system.md`):
iterate `w->count`, skip tombstones (`alive[i] == 0`), require
`mask[i] & COMPONENT_RENDER`, and draw from `world_xform[i]` and `render_ref[i]`.

`render_ref` is an **asset catalog stable id**. The renderer fetches that asset's
geometry once via `asset->get_data(render_ref, …)`, parses the `mesh_blob`, uploads
a vertex + index buffer, and caches it keyed by `render_ref`. Only the built-in
primitives carry geometry today, so a `render_ref` without an uploaded mesh is
skipped safely.

Per entity, `model = mat4_from_transform(world_xform)` and the camera's `view_proj`
are packed into a UBO (`std140 Camera { mat4 view_proj; mat4 model; }`) via
`buffer_update`, then the mesh is drawn. The `builtin://shader/scene.{vert,frag}`
program shades from the world normal — readable 3D without a lighting subsystem.

## Depth

The epic scopes off-screen passes out (a later "Rendering II"), which rules out a
separate off-screen depth pre-pass with a blit to the canvas. So the single
forward pass writes the imported backbuffer and relies on the **canvas's own depth
buffer**: the WebGL backend enables depth testing and clears depth each pass. This
is depth-correct — nearer entities occlude farther ones — without an off-screen
target.

## Mesh blob format

`krudd/ninja/plugins/include/mesh.h` defines the delivery format primitives ride over the
byte-oriented asset seam:

```
struct mesh_blob { u32 magic; u32 vertex_count; u32 index_count; u32 index_format; }
    followed by vertex_count * struct mesh_vertex   (position vec3, normal vec3, uv0 vec2)
    followed by index_count  * uint16_t
```

`krudd/ninja/plugins/asset/primitives.c` generates cube/sphere/plane/pyramid blobs, which
`asset_plugin` seeds as the `data` of the `builtin://…` primitive assets.

## Demo seed

On init, if the world holds no renderable entity, the renderer seeds a few
primitives at distinct transforms so the page shows depth-sorted content on load.
This is a temporary convenience — it never runs when a scene is already present —
and should be removed once scenes are authored/loaded routinely.
