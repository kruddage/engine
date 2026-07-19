; SPDX-License-Identifier: GPL-2.0-or-later

;;! Renderer interface spec — the source of truth for renderer.h. krudd emits
;;! the header during codegen (krudd-emit-interface-header in introspect.scm);
;;! there is no Scheme runtime side, this file is read as data, never evaluated.
;;!
;;! Low-level GPU API shaped by Aaltonen's "No Graphics API" philosophy: GPU
;;! primitives directly, no game-level drawing concepts. Backends register a
;;! struct gpu_api * as the "renderer" subsystem api.
;;!
;;! Forms render in file order, so every type is declared before it is used.

(c-include "stddef.h")
(c-include "stdint.h")

(c-section "Opaque handle types")

(c-handle gpu-cmd-buf)
(c-handle gpu-pipeline)
(c-handle gpu-texture)
(c-handle gpu-buffer)

(c-section "Capability flags")

;;! clip-z-zero-to-one advertises the backend's clip-space depth convention:
;;! set when NDC z runs [0, 1] (WebGPU, D3D, Metal), clear when it runs [-1, 1]
;;! (OpenGL / WebGL). A GL-convention projection puts the near half of its depth
;;! range at clip z < 0, which a [0,1] backend clips away, so a projection built
;;! for GL must be adapted (mat4-clip-z01) before such a backend rasterizes it.
;;! It is a convention, not a feature, but rides the caps word since that is the
;;! one per-backend flag the scene layer already queries.
;;! msaa-resolve advertises that the backend can render a scene pass into a
;;! multisampled colour target and resolve it to a single-sample texture in the
;;! same pass (via gpu-color-attachment's resolve-target). WebGPU sets it;
;;! WebGL (whose MSAA needs a separate multisampled-renderbuffer + blit path)
;;! and the null renderer leave it clear, so the scene renderer keeps its
;;! single-sample path on those and asks for no resolve.
(c-enum gpu-cap
        (gpu-cap-draw-direct       (<< 1 0))
        (gpu-cap-draw-indexed      (<< 1 1))
        (gpu-cap-compute           (<< 1 2))
        (gpu-cap-bindless          (<< 1 3))
        (gpu-cap-clip-z-zero-to-one (<< 1 4))
        (gpu-cap-msaa-resolve      (<< 1 5)))

(c-section "Enumerations")

(c-enum gpu-format
        (gpu-format-unknown 0)
        gpu-format-rgba8-unorm
        gpu-format-bgra8-unorm
        gpu-format-depth32-float
        gpu-format-rg32-float
        gpu-format-rgb32-float
        gpu-format-rgba32-float)

(c-enum gpu-topology
        (gpu-topology-triangle-list 0)
        gpu-topology-triangle-strip
        gpu-topology-line-list
        gpu-topology-point-list)

(c-enum gpu-index-format
        (gpu-index-format-uint16 0)
        gpu-index-format-uint32)

(c-enum gpu-load-op
        (gpu-load-op-load 0)
        gpu-load-op-clear
        gpu-load-op-dont-care)

(c-enum gpu-store-op
        (gpu-store-op-store 0)
        gpu-store-op-dont-care)

(c-section "Buffers")

(c-enum gpu-buffer-usage
        (gpu-buffer-usage-vertex  (<< 1 0))
        (gpu-buffer-usage-index   (<< 1 1))
        (gpu-buffer-usage-uniform (<< 1 2))
        (gpu-buffer-usage-storage (<< 1 3)))

(c-struct gpu-buffer-desc
          (size         size_t)
          (usage        u32)
          (initial-data (ptr (const void))))

(c-section "Shaders")

(c-enum gpu-shader-stage
        (gpu-shader-stage-vertex 0)
        gpu-shader-stage-fragment)

;;! The source language of a gpu-shader-source. glsl-es-300 is handed to the
;;! GL backend as-is; krudd is the shader DSL, which the backend lowers to its
;;! own target (GLSL today, WGSL/MSL later) at pipeline-create.
(c-enum gpu-shader-dialect
        (gpu-shader-dialect-glsl-es-300 0)
        gpu-shader-dialect-krudd)

(c-struct gpu-shader-source
          (src     (ptr (const char)))
          (stage   gpu-shader-stage)
          (dialect gpu-shader-dialect))

(c-section "Vertex input layout")

(c-define gpu-max-vertex-attrs 8)

(c-struct gpu-vertex-attr
          (location u32)
          (offset   u32)
          (format   gpu-format))

(c-struct gpu-vertex-layout
          (attrs      (array gpu-vertex-attr gpu-max-vertex-attrs))
          (attr-count u32)
          (stride     u32))

(c-section "Pipeline state object")

(c-define gpu-max-color-attachments 8)

;;! blend-enable / disable-depth-test are pipeline state (as in WebGPU), applied
;;! at cmd-set-pipeline. Both default to 0, which reproduces the opaque forward
;;! draw every scene pipeline already relies on: no blending, depth test on. A 2D
;;! overlay (kruddgui) sets blend-enable 1 for straight-alpha compositing
;;! (src-alpha, one-minus-src-alpha) and disable-depth-test 1 so later quads draw
;;! over earlier ones instead of being rejected by a same-depth test.
(c-struct gpu-pipeline-desc
          (color-formats      (array gpu-format gpu-max-color-attachments))
          (color-format-count u32)
          (depth-format       gpu-format)
          (topology           gpu-topology)
          (strip-index-format gpu-index-format)
          (sample-count       u32)
          (vertex-layout      gpu-vertex-layout)
          (vert               gpu-shader-source)
          (frag               gpu-shader-source)
          (blend-enable       u32)
          (disable-depth-test u32))

(c-section "Render passes")

;;! resolve-target (may be NULL) is the single-sample texture a multisampled
;;! `texture` resolves into at the end of the pass. It is set only when the
;;! colour target is multisampled and the backend advertises
;;! gpu-cap-msaa-resolve; a single-sample pass leaves it NULL and the attachment
;;! behaves exactly as before this field existed. A backend without the cap
;;! ignores it (the scene renderer never sets it there).
(c-struct gpu-color-attachment
          (texture        gpu-texture)
          (resolve-target gpu-texture)
          (load-op        gpu-load-op)
          (store-op       gpu-store-op)
          (clear          (array f32 4)))

(c-struct gpu-render-pass-desc
          (color          (array gpu-color-attachment gpu-max-color-attachments))
          (color-count    u32)
          (depth          gpu-texture)
          (depth-load-op  gpu-load-op)
          (depth-store-op gpu-store-op)
          (clear-depth    f32))

(c-section "Barriers")

(c-typedef gpu-stage-mask u32)

(c-define gpu-stage-top      (<< 1 0))
(c-define gpu-stage-vertex   (<< 1 1))
(c-define gpu-stage-fragment (<< 1 2))
(c-define gpu-stage-compute  (<< 1 3))
(c-define gpu-stage-transfer (<< 1 4))
(c-define gpu-stage-bottom   (<< 1 5))

(c-typedef gpu-access-mask u32)

(c-define gpu-access-shader-read    (<< 1 0))
(c-define gpu-access-shader-write   (<< 1 1))
(c-define gpu-access-color-write    (<< 1 2))
(c-define gpu-access-depth-write    (<< 1 3))
(c-define gpu-access-transfer-read  (<< 1 4))
(c-define gpu-access-transfer-write (<< 1 5))

(c-struct gpu-barrier
          (src-stage    gpu-stage-mask)
          (dst-stage    gpu-stage-mask)
          (src-access   gpu-access-mask)
          (dst-access   gpu-access-mask)
          (hazard-flags u32))

(c-section "Texture descriptors")

;;! initial-data (may be NULL) is a tightly-packed pixel buffer uploaded to mip
;;! level 0 at create time — the seam a baked procedural texture rides in on.
;;! generate-mips (0/1) asks the backend to build the full mip chain from level 0
;;! after upload (glGenerateMipmap on GL; a render/compute blit on a future
;;! WebGPU backend). A render-target texture leaves both zero, exactly as before
;;! these fields existed, so the frame graph is unaffected.
(c-struct gpu-texture-desc
          (format        gpu-format)
          (width         u32)
          (height        u32)
          (mip-levels    u32)
          (sample-count  u32)
          (initial-data  (ptr (const void)))
          (generate-mips u32))

(c-section "Draw / dispatch")

(c-struct gpu-draw-indexed-args
          (index-count    u32)
          (instance-count u32)
          (first-index    u32)
          (vertex-offset  i32)
          (first-instance u32))

(c-section "GPU API vtable")

(c-struct gpu-api
          (caps u32)

          (cmd-buf-begin  (fn gpu-cmd-buf ()))
          (cmd-buf-submit (fn void ((cmd gpu-cmd-buf))))

          ;;! Called once per frame, after every subsystem has drawn and submitted.
          ;;! The engine tick is the only caller.
          ;;!
          ;;! It exists because a frame is not one command buffer. The frame graph
          ;;! submits, kruddgui submits its overlay, and an open preview panel
          ;;! submits again — three in a frame, and nothing stops a fourth. A
          ;;! backend holding a per-frame resource therefore cannot release it at
          ;;! submit, because submit is not the end of anything.
          ;;!
          ;;! The WebGPU backend is the one that cares: it holds the canvas surface
          ;;! texture, which is the frame's. Releasing at submit meant the next
          ;;! subsystem to draw acquired a second, blank one — so the earlier
          ;;! subsystem's work sat in a texture that was never presented, and the
          ;;! canvas showed only whatever drew last. It releases here instead.
          ;;!
          ;;! There is deliberately no matching frame-begin: acquisition is already
          ;;! lazy, done by the first pass that names the backbuffer, and a backend
          ;;! with nothing to release at a frame boundary (WebGL, null) no-ops.
          (frame-end (fn void ()))

          (pipeline-create
           (fn gpu-pipeline ((desc (ptr (const gpu-pipeline-desc))))))
          (pipeline-destroy (fn void ((pipeline gpu-pipeline))))
          (cmd-set-pipeline
           (fn void ((cmd gpu-cmd-buf) (pipeline gpu-pipeline))))

          (buffer-create (fn gpu-buffer ((desc (ptr (const gpu-buffer-desc))))))
          (buffer-destroy (fn void ((buf gpu-buffer))))
          (buffer-update
           (fn void ((buf gpu-buffer) (offset u32)
                     (data (ptr (const void))) (size u32))))
          (cmd-bind-vertex-buffer
           (fn void ((cmd gpu-cmd-buf) (slot u32)
                     (buf gpu-buffer) (offset u32))))
          (cmd-bind-index-buffer
           (fn void ((cmd gpu-cmd-buf) (buf gpu-buffer)
                     (offset u32) (fmt gpu-index-format))))
          (cmd-bind-uniform-buffer
           (fn void ((cmd gpu-cmd-buf) (slot u32) (buf gpu-buffer)
                     (offset u32) (size u32))))

          (cmd-begin-render-pass
           (fn void ((cmd gpu-cmd-buf) (desc (ptr (const gpu-render-pass-desc))))))
          (cmd-end-render-pass (fn void ((cmd gpu-cmd-buf))))

          (cmd-barrier
           (fn void ((cmd gpu-cmd-buf) (barriers (ptr (const gpu-barrier)))
                     (count u32))))

          (cmd-draw-indexed
           (fn void ((cmd gpu-cmd-buf) (args (ptr (const gpu-draw-indexed-args))))))

          ;;! Non-indexed draw (the gpu-cap-draw-direct path): pull vertex-count
          ;;! vertices from the bound vertex buffer starting at first-vertex, no index
          ;;! buffer. instance-count 1 / first-instance 0 is the ordinary case. Used by
          ;;! a 2D batch (kruddgui) that streams triangles with no shared vertices.
          (cmd-draw
           (fn void ((cmd gpu-cmd-buf) (vertex-count u32) (instance-count u32)
                     (first-vertex u32) (first-instance u32))))

          ;;! Restrict subsequent draws in this pass to a rectangle, in framebuffer
          ;;! pixels with the origin at the bottom-left (GL / WebGPU scissor space).
          ;;! The caller passes the full target rect to clear the restriction; a pass's
          ;;! draws are otherwise unscissored.
          (cmd-set-scissor
           (fn void ((cmd gpu-cmd-buf) (x i32) (y i32) (width u32) (height u32))))

          (cmd-dispatch
           (fn void ((cmd gpu-cmd-buf) (x u32) (y u32) (z u32))))

          (gpu-malloc (fn (ptr void) ((size size_t))))
          (gpu-free (fn void ((ptr (ptr void)))))
          (gpu-host-to-device-ptr (fn (ptr void) ((host-ptr (ptr void)))))

          (texture-create (fn gpu-texture ((desc (ptr (const gpu-texture-desc))))))
          (texture-destroy (fn void ((texture gpu-texture))))

          ;;! Bind a sampled texture to a texture unit for the next draw. The shader's
          ;;! sampler2D uniform reads from the matching unit (unit 0 is the GLSL
          ;;! default, which the scene-textured shader's single albedo sampler uses).
          (cmd-bind-texture
           (fn void ((cmd gpu-cmd-buf) (unit u32) (texture gpu-texture))))

          ;;! Return an opaque id naming a texture, or 0 when there is none. The id is
          ;;! meaningful only to the backend that issued it: cmd-bind-texture-handle
          ;;! resolves it back, and nothing else may interpret it. The WebGL backend
          ;;! hands back what is effectively its own texture name; the WebGPU backend
          ;;! hands back an index into a table it keeps. Neither caller can tell, and
          ;;! that is the point.
          ;;!
          ;;! It exists because a UI layer has to composite a render-target texture
          ;;! through its own quad batch — kruddgui's kgui-image draws a scene preview
          ;;! or a kruddboard bake — and the handle crosses into Scheme on the way
          ;;! (kgui-image takes it as an s7 number). It therefore has to stay an
          ;;! integer, which is why this is an id and not a gpu-texture.
          ;;!
          ;;! Calling it twice for the same live texture returns the same id. An id
          ;;! whose texture has since been destroyed resolves to nothing rather than to
          ;;! whatever took its place — backends must not let a recycled id name an
          ;;! unrelated texture. The null renderer owns no textures and returns 0.
          (texture-handle (fn u32 ((texture gpu-texture))))

          ;;! Bind a texture to a unit by the id texture-handle returned. The symmetric
          ;;! partner to it, and the only thing that may interpret an id.
          ;;!
          ;;! For a UI layer that holds an id rather than a gpu-texture: kruddgui's
          ;;! image quads carry one across Scheme. Otherwise identical to
          ;;! cmd-bind-texture. An id of 0, or one whose texture has been destroyed,
          ;;! unbinds the unit. The null renderer no-ops.
          (cmd-bind-texture-handle
           (fn void ((cmd gpu-cmd-buf) (unit u32) (handle u32)))))
