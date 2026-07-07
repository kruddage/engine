# Frame Graph — `krudd/ninja/plugins/frame_graph`

The frame graph is a Frostbite/Unreal-style render graph
([O'Donnell, GDC 2017](https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in);
Unreal's RDG). Each frame a consumer *declares* passes and the resources they
read and write; the graph compiles that declaration into an ordered, culled DAG,
then executes it — allocating transient textures, inserting barriers, and
setting up the render pass around each pass. Inside a pass the consumer records
real GPU commands through a command context the graph **lends** it for the
duration of the execute callback.

This boundary is the whole point of the plugin, so it is worth stating exactly.

## What the graph owns vs. what a pass owns

| Concern                              | Owner | Notes                                                              |
|--------------------------------------|-------|--------------------------------------------------------------------|
| Resource lifetimes (transients)      | graph | `texture_create` on first write, `texture_destroy` after last read |
| The pass DAG + dead-pass culling     | graph | reference-counted cull, then a topological sort                    |
| Automatic barriers                   | graph | WRITE→READ hazard barrier on cross-pass reads                      |
| Render-pass setup                    | graph | `cmd_begin_render_pass` / `cmd_end_render_pass` around each pass    |
| Attachment selection (color + depth) | graph | built from the pass's declared `writes[]`                          |
| Per-pass load/clear config           | pass  | `fg_pass_set_color_clear` / `fg_pass_set_depth_clear`              |
| Recording GPU commands               | pass  | set pipeline, draw, dispatch on the **lent** command buffer        |
| Persistent GPU resources (PSO, mesh) | consumer | created once against the device — see the seam below            |

The graph manages *resources and synchronization*. It does **not** wrap draw
calls: a pass issues normal `gpu->cmd_*` commands. This is deliberately less
strict than the engine's first `fg.h` contract (which made the graph the "sole
owner of `gpu_api`" and funnelled every draw through an `fg_cmd_*` wrapper) —
that approach forced every GPU capability to be re-plumbed through fg and does
not match how Frostbite or Unreal actually split responsibility.

## The lend-at-execute rule (borrowed, not kept)

The graph stays the owner of the GPU API; at execute time it **lends** it into
each pass's execute callback via `struct fg_pass_ctx`:

```c
static void my_pass(struct fg_pass_ctx *ctx, void *userdata)
{
        const struct gpu_api *gpu = fg_ctx_gpu(ctx);
        gpu_cmd_buf_t         cmd = fg_ctx_cmd(ctx);
        gpu_texture_t         tex = fg_ctx_resource(ctx, some_resource);

        gpu->cmd_set_pipeline(cmd, my_pso);
        gpu->cmd_draw_indexed(cmd, &args, root_data);
}
```

These accessors — and the handles they return — are **valid only for the
duration of the callback**. The contract is *borrowed, not kept*:

- Do **not** cache `gpu`, `cmd`, or any resolved texture handle across frames or
  across passes.
- Do **not** resolve `"renderer"` from inside a pass. The graph already holds
  the device and lends exactly what the pass needs.
- The callback runs **inside an already-begun render pass**. Do not begin or end
  the render pass yourself; the graph does that around you from your declared
  writes.

## Render-pass setup

Before invoking a pass's callback, `fg_execute` builds a `gpu_render_pass_desc`
from that pass's declared `writes[]`:

- A write whose format is `GPU_FORMAT_DEPTH32_FLOAT` becomes the **depth**
  attachment.
- Every other write becomes a **color** attachment, in declaration order.

Each attachment defaults to `GPU_LOAD_OP_LOAD` / `GPU_STORE_OP_STORE`. A pass
opts into clearing with:

```c
fg_pass_set_color_clear(pass, /* color index */ 0, (float[4]){0, 0, 0, 1});
fg_pass_set_depth_clear(pass, 1.0f);
```

The color index counts color-format writes in declaration order (the same order
they become color attachments).

## Resources: transient vs. imported

- **Transient textures** (`fg_declare_transient`) are owned by the graph. It
  `texture_create`s them on first write and `texture_destroy`s them after their
  last read, within a single frame.
- **Imported resources** (`fg_import_backbuffer`) are external — the canvas /
  default framebuffer. A pass can name the backbuffer as a write target, but the
  graph only **binds** it; it never creates or destroys its storage. The pass
  that writes the backbuffer carries an implicit external reference, so it is
  never culled even though no in-graph pass reads it.

> Transient resource **aliasing** (reusing one physical allocation for several
> non-overlapping transients) is intentionally out of scope here — that is later
> "Rendering II" work. This boundary is about *who records what*, not memory
> reuse.

## The persistent-creation seam

Some GPU resources outlive every frame: the compiled **pipeline state object**
(PSO) and **static mesh** vertex/index buffers. The execute-time lend only hands
out the GPU *inside* a pass callback, so it does not — and should not — cover
one-time persistent creation.

The decided path (epic #167, issue #168 — "option a"):

> **The consumer creates persistent GPU resources by resolving the `"renderer"`
> device directly, once, outside any frame.**

```c
/* At setup — exactly once, never per frame. */
const struct gpu_api *gpu = subsystem_manager_get_api(mgr, "renderer");
gpu_pipeline_t pso = gpu->pipeline_create(&pso_desc);
/* upload static mesh buffers via gpu_malloc / gpu_host_to_device_ptr ... */
```

This is a **narrow, documented exception**, scoped to one-time creation, and it
matches Frostbite/Unreal (persistent resources are created against the device).
It does **not** apply to:

- per-frame command recording — that goes through the lent context; or
- transient textures such as a depth buffer — the graph owns and creates those.

Keeping the strict *borrowed, not kept* rule on the per-frame path is what
matters; the persistent seam is the deliberate, single place a consumer touches
the device directly.
