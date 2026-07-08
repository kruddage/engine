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

(c-enum gpu-cap
	(gpu-cap-draw-direct  (<< 1 0))
	(gpu-cap-draw-indexed (<< 1 1))
	(gpu-cap-compute      (<< 1 2))
	(gpu-cap-bindless     (<< 1 3)))

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

(c-enum gpu-shader-dialect
	(gpu-shader-dialect-glsl-es-300 0))

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

(c-struct gpu-pipeline-desc
	(color-formats      (array gpu-format gpu-max-color-attachments))
	(color-format-count u32)
	(depth-format       gpu-format)
	(topology           gpu-topology)
	(strip-index-format gpu-index-format)
	(sample-count       u32)
	(vertex-layout      gpu-vertex-layout)
	(vert               gpu-shader-source)
	(frag               gpu-shader-source))

(c-section "Render passes")

(c-struct gpu-color-attachment
	(texture  gpu-texture)
	(load-op  gpu-load-op)
	(store-op gpu-store-op)
	(clear    (array f32 4)))

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

(c-struct gpu-texture-desc
	(format       gpu-format)
	(width        u32)
	(height       u32)
	(mip-levels   u32)
	(sample-count u32))

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
	(cmd-dispatch
	  (fn void ((cmd gpu-cmd-buf) (x u32) (y u32) (z u32))))

	(gpu-malloc (fn (ptr void) ((size size_t))))
	(gpu-free (fn void ((ptr (ptr void)))))
	(gpu-host-to-device-ptr (fn (ptr void) ((host-ptr (ptr void)))))

	(texture-create (fn gpu-texture ((desc (ptr (const gpu-texture-desc))))))
	(texture-destroy (fn void ((texture gpu-texture)))))
