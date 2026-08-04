; SPDX-License-Identifier: GPL-2.0-or-later

;;! default — what the page opens on: a checkered floor with a box spinning on
;;! it, a sphere hopping, a pyramid rocking and a turned rook, all under a
;;! camera that orbits the lot. Not a game and not pretending to be one. It is
;;! the engine saying hello — one scene that puts every mesh shape engine, both
;;! scene shaders and three behaviour scripts on screen at once, which is what
;;! you want behind a project picker and exactly what you want to be looking at
;;! when you are asking "is the renderer alive".
;;!
;;! --- why this file exists (#1034) ------------------------------------------
;;!
;;! This scene is not new. It is `seed_demo_scene`, which lived in
;;! render/scene_renderer/scene_renderer.c and ran from that plugin's init —
;;! five entities, a camera and a light, built in C the moment the renderer came
;;! up and before the boot had read the URL. It has been deleted from there and
;;! written out here, unchanged apart from the two notes below.
;;!
;;! The bug it caused: the seed ran no matter what the page had been asked for.
;;! Every project but the staged one arrives by fetch (krudd_boot_unresolved in
;;! core/engine.c hands the name back to the shell, which fetches the source and
;;! brings it in through window.kruddRunProject), and until that source lands
;;! there was a scene already on screen — this one. So opening ducks showed a
;;! spinning box first, and the picker came down over the demo rather than over
;;! the game. A scene nobody asked for, in front of the one they did.
;;!
;;! Deleting it outright was the other option and it is the worse one: the demo
;;! is the only thing in the tree that draws the built-ins, and the page would
;;! then open on a clear colour. A scene that is worth showing is worth being a
;;! project — so it is one, it holds the staged slot (see build.scm), and the
;;! engine boots it exactly when the URL named nothing. Nothing is seeded from
;;! C any more, so a `?game=` that has to fetch now waits on an empty world
;;! rather than on this one.
;;!
;;! --- what the move cost, exactly -------------------------------------------
;;!
;;! Two details of the C seed have no spelling in the (scene ...) DSL, and both
;;! are named here rather than quietly dropped:
;;!
;;!   THE SUN. The seed made a COMPONENT_LIGHT entity called "Sun" at identity
;;!   rotation. scene_renderer_tick turns its base sun vector by the first live
;;!   light entity's world rotation — and at identity that leaves the vector
;;!   exactly as it found it, which is also what happens when the scene carries
;;!   no light entity at all. So the two are pixel-identical today, and the
;;!   entity is gone rather than faked. When the DSL grows a light clause this
;;!   scene should get one back, because "you can rotate the sun" is the thing
;;!   that entity was proof of.
;;!
;;!   THE FLOOR'S CHECK SIZE. The seed pushed a per-entity texture-param
;;!   override onto the floor (scale 12, against the checker texture's own
;;!   default of 8) so the tiles read smaller than the plane. Per-entity texture
;;!   params are a world column with no scene clause behind them, so the floor
;;!   below wears builtin://material/checker as the catalog bakes it: the same
;;!   checkerboard, in tiles a half again bigger. Visible, harmless, and a
;;!   (texture-params ...) clause is the fix whenever something needs one for a
;;!   reason better than this.
;;!
;;! --- no rules, and no hooks ------------------------------------------------
;;!
;;! There is no (rules ...) clause, no (on-load ...) and no (on-tick ...),
;;! because there is nothing here to run: everything that moves is a built-in
;;! entity script the asset catalog already carries, ticked by the entity-script
;;! driver, and every mesh and material is a built-in too. This project defines
;;! no asset of its own and holds no state — which makes it the smallest
;;! complete project in the tree, and a fair reading of what the (project ...)
;;! form costs when a scene is all you want.

;;! --- the scene -------------------------------------------------------------
;;!
;;! The same five props at the same coordinates the C seed spawned them at, in
;;! the same order, so the picture is the one that was there before. The mix is
;;! deliberate and worth keeping deliberate: the box, sphere and rook wear the
;;! physically based metal/plastic materials while the floor and pyramid wear
;;! the textured checker, so both scene shaders are on screen together; and the
;;! rook is the marching-cubes/SDF mesh, so the three mesh shape engines (lathe,
;;! parametric grid, implicit surface) are all represented.
;;!
;;! Entities with no (scale ...) take the DSL's default of 1 1 1, which is what
;;! the seed set them to by hand; only the floor is scaled, out to a 6-unit
;;! square half a unit below the origin.
(project "Default"
         (scene default
                ;;! The orbiting eye. scene_renderer resolves the camera by
                ;;! NAME — the first live entity called "Camera" — and reads its
                ;;! world position each frame, so the built-in orbit-camera
                ;;! script driving this entity is the whole camera rig. No mesh
                ;;! and no material: it is a position with a script on it.
                (entity (name "Camera")
                        (script "builtin://script/orbit-camera"))

                (entity (name "Floor")
                        (mesh     "builtin://mesh/plane")
                        (material "builtin://material/checker")
                        (at 0 -0.5 0) (scale 6 1 6))

                (entity (name "Box")
                        (mesh     "builtin://mesh/box")
                        (material "builtin://material/pbr-plastic")
                        (script   "builtin://script/spinner")
                        (at -1.5 0 0))

                (entity (name "Sphere")
                        (mesh     "builtin://mesh/sphere")
                        (material "builtin://material/pbr-metal")
                        (script   "builtin://script/bounce")
                        (at 0 0 -1))

                (entity (name "Pyramid")
                        (mesh     "builtin://mesh/pyramid")
                        (material "builtin://material/checker")
                        (script   "builtin://script/wobble")
                        (at 1.5 0 0.5))

                (entity (name "Rook")
                        (mesh     "builtin://mesh/sdf-rook")
                        (material "builtin://material/pbr-metal")
                        (script   "builtin://script/spinner")
                        (at 0 0 1.4))))
