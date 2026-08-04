; SPDX-License-Identifier: GPL-2.0-or-later

;;! training — a room-scale training space: a dark shell ruled by glowing lines,
;;! the whole thing built at human scale so a headset dropped into it later
;;! stands in a room rather than a doll's house or a cathedral.
;;!
;;! THE POINT OF THIS PROJECT IS THE SCALE. There is no game here and there is
;;! deliberately none yet — what it exists to pin down is that one world unit is
;;! one metre, and that every mesh the room is built from is sized in metres
;;! against that. Everything else it will grow (see the initiative this landed
;;! under) is layered on a room that is already the right size, because a room
;;! that is the wrong size is not discovered until someone puts a headset on and
;;! by then every asset in it is wrong together.
;;!
;;! --- one unit is one metre -------------------------------------------------
;;!
;;! The convention itself — why a unit is a metre, what WebXR requires of that,
;;! and what each built-in mesh measures against it, capsule included — is
;;! stated once, in krudd/engine/base/math/include/math/camera.h. This project
;;! is a consumer of that convention, not its definition: it is the first scene
;;! in the tree built to it, and training-metres-per-unit below and
;;! training_test.c are what make that a checked fact rather than a comment.

;;! --- the room, in metres ---------------------------------------------------
;;!
;;! A 6 m x 6 m floor under a 3 m ceiling: a generous domestic room, and roughly
;;! the largest space a room-scale headset boundary is usually drawn in. The
;;! walls stand at x = ±3 and z = ±3, so the origin is the centre of the floor
;;! and the floor is y = 0 — which is also where WebXR puts the floor in a
;;! local-floor reference space, so the room needs no offset to be stood in.
;;! In metres throughout: the floor edge to edge, the floor to the ceiling, and
;;! the origin out to any wall.
(define training-room-size 6.0)
(define training-room-height 3.0)
(define training-room-half 3.0)

;;! Standing eye height. 1.6 m is the conventional default a headset assumes
;;! before it has measured anyone, so the flatscreen camera parks there: what
;;! you see on a monitor today is what you would see standing in it later.
(define training-eye-height 1.6)

;;! The reference figure: its real height head to floor, the capsule scale that
;;! produces that height, and its width shoulder to shoulder — all metres except
;;! the scale. The division is written out rather than folded into a literal
;;! because the whole hazard here is that the second number does not look like
;;! the first.
(define training-human-height 1.75)
(define training-human-scale (/ training-human-height 2.0))
(define training-human-width 0.45)

;;! Grid spacing, and how fat a ruled line is. One metre is the point — the
;;! lines are a ruler you can read while standing in the room, so that a mesh
;;! authored later can be eyeballed against them before anyone measures it.
(define training-grid-step 1.0)
(define training-line-width 0.02)

;;! (training-metres-per-unit ignored) -> 1, a poll hook the way chess-turn is:
;;! dispatch_scm calls with one throwaway integer and takes an integer back, so
;;! this is the convention itself readable from C. It is 1 and the test asserts
;;! it is 1; if the engine ever adopts a different unit this is the line that
;;! has to change and the assert that will notice.
(define (training-metres-per-unit ignored) 1)

;;! (training-reset . ignored) — the (on-load ...) hook. Nothing to reset yet:
;;! the room has no state, and a project with no rules still wants the hook
;;! wired so the first rule that arrives has somewhere to land. Variadic for the
;;! same reason chess-reset is — dispatch_scm always passes one argument, the
;;! tests may pass none.
(define (training-reset . ignored) 0)

;;! --- the materials this room brings with it --------------------------------
;;!
;;! Two, and the contrast between them is the whole look: a shell that swallows
;;! light and lines that emit it.
;;!
;;! The shell is a near-black dielectric at a high roughness, so the floor and
;;! walls take almost nothing from the analytic highlight and read as a void
;;! with edges rather than as surfaces. Not pure black: a little base colour
;;! keeps the room from flattening into a silhouette when a lit object stands in
;;! it.
(define training-shell-material
  (string-append "(material training-shell\n"
                 "  (shader \"builtin://shader/pbr\")\n"
                 "  (base_color 0.035 0.038 0.045 1.0)\n"
                 "  (metallic 0.0)\n"
                 "  (roughness 0.85))\n"))

;;! The lines. `emissive` is the pbr shader's own Material field (asset_plugin's
;;! shader source, `(emissive vec3 (edit color))`), summed into the shaded colour
;;! alongside ambient and the analytic lobe — so a value above 1 makes a surface
;;! brighter than any light in the scene can make it, which is what a glowing
;;! line is. Cyan, and driven well past 1 so the lines stay the brightest thing
;;! in frame and would bloom if a bloom pass is ever hung off the frame graph.
;;! base_color is near-black under it: the line does not want to also be a lit
;;! surface, it wants to be a light.
(define training-line-material
  (string-append "(material training-line\n"
                 "  (shader \"builtin://shader/pbr\")\n"
                 "  (base_color 0.02 0.06 0.08 1.0)\n"
                 "  (metallic 0.0)\n"
                 "  (roughness 0.6)\n"
                 "  (emissive 0.15 1.7 2.1))\n"))

;;! Registered as this source is evaluated — before any scene is built, so the
;;! scene clause's (material ...) paths resolve when the launcher opens the
;;! room. project://, not builtin://, because the engine seeds nothing named
;;! training: these are in the catalog only because the two forms below put them
;;! there. Both return 0 in the headless test, which boots a catalog but the
;;! room's look is not what that test is about.
(material-define! "project://material/training-shell" training-shell-material)
(material-define! "project://material/training-line" training-line-material)

;;! --- the project -----------------------------------------------------------
;;!
;;! The room, written out literally the way chess writes its 64 squares out
;;! literally: a (scene ...) clause is data, and a loop that emitted it would
;;! buy nothing but a layer between the numbers and the room.
;;!
;;! Floor lines are planes — flat, so a plane is exactly the mesh for them —
;;! scaled to 6 m x 2 cm and floated 4 mm off the floor so they win the depth
;;! test against it without being visible as a step. Wall lines are boxes,
;;! because a plane faces +Y and a wall line has to stand up; a 2 cm box is
;;! cheaper than teaching this file about rotations for a shape you cannot see
;;! the thickness of anyway.
;;!
;;! The lines run at every metre from -3 to +3 inclusive, both axes: seven and
;;! seven. The outermost of each pair lands exactly at a wall, which is what the
;;! room's edge should look like, so they are not special-cased away.
(project "Training"
         (scene training
                ;;! Standing at the near edge, eyes at 1.6 m, looking back
                ;;! across the room at the figure. scene_renderer reads this
                ;;! entity's world position as the eye each frame; the view
                ;;! target stays the origin, which here is the floor centre.
                (entity (name "Camera") (at 0 1.6 2.4))

                ;;! The shell: floor, ceiling, and four walls. The floor is a
                ;;! plane at y = 0 — the height WebXR calls the floor — and the
                ;;! ceiling is the same plane 3 m up. The walls are 5 cm boxes
                ;;! so the room has thickness rather than being a set of
                ;;! one-sided quads you can see out through from behind.
                (entity (name "floor")
                        (mesh     "builtin://mesh/plane")
                        (material "project://material/training-shell")
                        (at 0 0 0) (scale 6 1 6))
                (entity (name "ceiling")
                        (mesh     "builtin://mesh/plane")
                        (material "project://material/training-shell")
                        (at 0 3 0) (scale 6 1 6))
                (entity (name "wall-north")
                        (mesh     "builtin://mesh/box")
                        (material "project://material/training-shell")
                        (at 0 1.5 -3) (scale 6 3 0.05))
                (entity (name "wall-south")
                        (mesh     "builtin://mesh/box")
                        (material "project://material/training-shell")
                        (at 0 1.5 3) (scale 6 3 0.05))
                (entity (name "wall-west")
                        (mesh     "builtin://mesh/box")
                        (material "project://material/training-shell")
                        (at -3 1.5 0) (scale 0.05 3 6))
                (entity (name "wall-east")
                        (mesh     "builtin://mesh/box")
                        (material "project://material/training-shell")
                        (at 3 1.5 0) (scale 0.05 3 6))

                ;;! Floor lines running along X, one per metre of Z.
                (entity (name "floor-x-n3") (mesh "builtin://mesh/plane")
                        (material "project://material/training-line")
                        (at 0 0.004 -3) (scale 6 1 0.02))
                (entity (name "floor-x-n2") (mesh "builtin://mesh/plane")
                        (material "project://material/training-line")
                        (at 0 0.004 -2) (scale 6 1 0.02))
                (entity (name "floor-x-n1") (mesh "builtin://mesh/plane")
                        (material "project://material/training-line")
                        (at 0 0.004 -1) (scale 6 1 0.02))
                (entity (name "floor-x-0") (mesh "builtin://mesh/plane")
                        (material "project://material/training-line")
                        (at 0 0.004 0) (scale 6 1 0.02))
                (entity (name "floor-x-p1") (mesh "builtin://mesh/plane")
                        (material "project://material/training-line")
                        (at 0 0.004 1) (scale 6 1 0.02))
                (entity (name "floor-x-p2") (mesh "builtin://mesh/plane")
                        (material "project://material/training-line")
                        (at 0 0.004 2) (scale 6 1 0.02))
                (entity (name "floor-x-p3") (mesh "builtin://mesh/plane")
                        (material "project://material/training-line")
                        (at 0 0.004 3) (scale 6 1 0.02))

                ;;! Floor lines running along Z, one per metre of X.
                (entity (name "floor-z-n3") (mesh "builtin://mesh/plane")
                        (material "project://material/training-line")
                        (at -3 0.004 0) (scale 0.02 1 6))
                (entity (name "floor-z-n2") (mesh "builtin://mesh/plane")
                        (material "project://material/training-line")
                        (at -2 0.004 0) (scale 0.02 1 6))
                (entity (name "floor-z-n1") (mesh "builtin://mesh/plane")
                        (material "project://material/training-line")
                        (at -1 0.004 0) (scale 0.02 1 6))
                (entity (name "floor-z-0") (mesh "builtin://mesh/plane")
                        (material "project://material/training-line")
                        (at 0 0.004 0) (scale 0.02 1 6))
                (entity (name "floor-z-p1") (mesh "builtin://mesh/plane")
                        (material "project://material/training-line")
                        (at 1 0.004 0) (scale 0.02 1 6))
                (entity (name "floor-z-p2") (mesh "builtin://mesh/plane")
                        (material "project://material/training-line")
                        (at 2 0.004 0) (scale 0.02 1 6))
                (entity (name "floor-z-p3") (mesh "builtin://mesh/plane")
                        (material "project://material/training-line")
                        (at 3 0.004 0) (scale 0.02 1 6))

                ;;! Two waist/shoulder-height lines ruled around the walls, at 1
                ;;! and 2 metres. They are what makes the room read as a room
                ;;! from inside it rather than as a floor with a dark surround,
                ;;! and standing next to a line you know is at your chest is the
                ;;! fastest check that the scale is right.
                (entity (name "wall-north-y1") (mesh "builtin://mesh/box")
                        (material "project://material/training-line")
                        (at 0 1 -2.97) (scale 6 0.02 0.02))
                (entity (name "wall-north-y2") (mesh "builtin://mesh/box")
                        (material "project://material/training-line")
                        (at 0 2 -2.97) (scale 6 0.02 0.02))
                (entity (name "wall-south-y1") (mesh "builtin://mesh/box")
                        (material "project://material/training-line")
                        (at 0 1 2.97) (scale 6 0.02 0.02))
                (entity (name "wall-south-y2") (mesh "builtin://mesh/box")
                        (material "project://material/training-line")
                        (at 0 2 2.97) (scale 6 0.02 0.02))
                (entity (name "wall-west-y1") (mesh "builtin://mesh/box")
                        (material "project://material/training-line")
                        (at -2.97 1 0) (scale 0.02 0.02 6))
                (entity (name "wall-west-y2") (mesh "builtin://mesh/box")
                        (material "project://material/training-line")
                        (at -2.97 2 0) (scale 0.02 0.02 6))
                (entity (name "wall-east-y1") (mesh "builtin://mesh/box")
                        (material "project://material/training-line")
                        (at 2.97 1 0) (scale 0.02 0.02 6))
                (entity (name "wall-east-y2") (mesh "builtin://mesh/box")
                        (material "project://material/training-line")
                        (at 2.97 2 0) (scale 0.02 0.02 6))

                ;;! The human reference: 1.75 m tall, standing on the floor.
                ;;! A capsule is 2 units tall at scale 1, so Y is 0.875 and the
                ;;! centre sits at 0.875 for the feet to touch y = 0. This
                ;;! entity is the assertion "the room is the right size" in a
                ;;! form you can see, and training_test.c reads its scale back
                ;;! to make it an assertion you can run.
                (entity (name "human-reference")
                        (mesh     "builtin://mesh/capsule")
                        (material "project://material/training-line")
                        (at 0 0.875 -1) (scale 0.45 0.875 0.45)))

         (on-load training-reset))
