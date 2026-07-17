; SPDX-License-Identifier: GPL-2.0-or-later

;;! The procedural demo — the launcher's "default scene", and a showcase of the
;;! engine's from-code content: every mesh, material and motion here is generated,
;;! not imported. Three primitives animated by the built-in entity scripts (a
;;! spinning box, a bouncing sphere, a wobbling pyramid) sit beside the SDF rook,
;;! which the marching-cubes shape engine builds from a signed-distance field. No
;;! rules and no input — it just runs, so picking it from the menu drops you into
;;! the procedural playground the editor has always shown.

(scene procedural-demo
  (entity (name "spinner")
          (mesh     "builtin://mesh/box")
          (material "builtin://material/pbr-plastic")
          (script   "builtin://script/spinner")
          (at -1.8 0 0))
  (entity (name "bouncer")
          (mesh     "builtin://mesh/sphere")
          (material "builtin://material/pbr-metal")
          (script   "builtin://script/bounce")
          (at 0 0.5 0))
  (entity (name "wobbler")
          (mesh     "builtin://mesh/pyramid")
          (material "builtin://material/pbr-plastic")
          (script   "builtin://script/wobble")
          (at 1.8 0 0))
  (entity (name "rook")
          (mesh     "builtin://mesh/sdf-rook")
          (material "builtin://material/pbr-metal")
          (at 0 0 -2.2) (scale 1.4 1.4 1.4)))
