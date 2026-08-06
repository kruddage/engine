; SPDX-License-Identifier: GPL-2.0-or-later

;;! modelgen — a model viewer: one turntable, one plinth, and a dropdown that
;;! says which model is on it. Pick a piece, watch it turn under the scene's
;;! light, pick the next one.
;;!
;;! This is the first slice of a model-generation project, and it is
;;! deliberately the *viewing* half. Everything downstream of "generate a mesh"
;;! — getting the geometry into the catalog, onto an entity, in front of a
;;! camera, lit, turning, and swappable while the engine runs — is plumbing that
;;! has to work before a generator is worth writing, and it is all here and all
;;! checked by modelgen_test.c. What is NOT here is any generation: the six
;;! models below are the chess set's pieces, copied across from
;;! projects/chess/chess.scm as they stand, at project://mesh/modelgen-* paths.
;;! They are a stand-in for generated output, and the day something here
;;! generates a mesh it takes their place with nothing else in this file moving
;;! — the viewer names a list of (label . path) pairs and knows nothing else
;;! about where a model came from.
;;!
;;! Copied rather than reached for on purpose. A project is a single .scm the
;;! engine loads at runtime and there is no import mechanism (projects/
;;! README.md), so "share the chess meshes" would mean either a project reaching
;;! into another project — the one rule in that file nothing has come close to
;;! breaking — or lifting six meshes back into the engine as built-ins, which is
;;! the move #1013 made in the other direction and for good reason: they are a
;;! game's geometry, not the engine's. Copies it is, until there is a generator
;;! whose output makes the question moot.
;;!
;;! Distances are METRES, the convention stated in
;;! krudd/engine/base/math/include/math/camera.h.

;;! --- the stage --------------------------------------------------------------
;;!
;;! scene_renderer aims a FIXED look-at at the world origin and reads only the
;;! eye from the entity a scene names "Camera", so framing a model is a matter
;;! of putting the model where the camera is already looking. The plinth's top
;;! is therefore BELOW the origin by a little over half a model's height: a
;;! piece standing on it straddles y = 0, which is the point the view is centred
;;! on, so the tall king and the short pawn are both framed by the same still
;;! camera without either of them needing a camera move.
(define modelgen-stage-y -0.6)

;;! --- the model list ---------------------------------------------------------
;;!
;;! What the dropdown offers, in the order it offers it: a display label and the
;;! catalog path its geometry is at. The list is the whole of what the viewer
;;! knows about the models — nothing below reads a piece's name, its height or
;;! its shape — which is what makes it the seam a generator plugs into later.
(define modelgen-models
  (list (cons "Pawn"   "project://mesh/modelgen-pawn")
        (cons "Knight" "project://mesh/modelgen-knight")
        (cons "Rook"   "project://mesh/modelgen-rook")
        (cons "Bishop" "project://mesh/modelgen-bishop")
        (cons "Queen"  "project://mesh/modelgen-queen")
        (cons "King"   "project://mesh/modelgen-king")))

;;! (modelgen-count) -> how many models the list offers.
(define (modelgen-count) (length modelgen-models))

;;! (modelgen-clamp i) -> I folded into a valid index. Clamped rather than
;;! wrapped: the dropdown can only ever hand back a row that exists, so this is
;;! the guard on everything else — a reload, a test, a later generator — and a
;;! clamp keeps the ends of the list as the ends of the list.
(define (modelgen-clamp i)
  (max 0 (min (- (modelgen-count) 1) i)))

;;! (modelgen-label i) / (modelgen-path i) -> the I'th model's display label and
;;! its catalog path.
(define (modelgen-label i)
  (car (list-ref modelgen-models (modelgen-clamp i))))

(define (modelgen-path i)
  (cdr (list-ref modelgen-models (modelgen-clamp i))))

;;! (modelgen-labels) -> the labels alone, in order: what the dropdown draws.
(define (modelgen-labels)
  (map car modelgen-models))

;;! --- what is on the turntable -----------------------------------------------

;;! Which model is showing, as an index into the list above. A fresh load opens
;;! on the first one.
(define *modelgen-index* 0)

;;! The display entity's id, or -1 before a scene has been built. The entity is
;;! SPAWNED (modelgen-spawn!) rather than authored in the (scene ...) clause
;;! below, and that is not a stylistic choice: a scene clause hands nothing
;;! back, and there is no find-an-entity-by-name primitive, so a rule that has
;;! to re-bind a mesh every time the dropdown moves has no way to name the
;;! entity it must re-bind. Spawning it is how the rules come to hold its id.
;;! Everything that never changes — the ground, the plinth, the camera — stays
;;! authored, where it reads as the scene it is.
(define *modelgen-entity* -1)

;;! (modelgen-entity ignored) / (modelgen-index ignored) / (modelgen-models-n
;;! ignored) — read hooks the host (or a test) polls through dispatch_scm, which
;;! calls with one integer argument and hands back an integer.
(define (modelgen-entity ignored) *modelgen-entity*)
(define (modelgen-index ignored) *modelgen-index*)
(define (modelgen-models-n ignored) (modelgen-count))

;;! (modelgen-spawn!) -> the display entity's id. One entity at the plinth's
;;! top, wearing this project's display material and its turntable script; the
;;! mesh is left to modelgen-show!, which is the one thing about this entity
;;! that ever changes. Run from the load hook, so the world it spawns into is
;;! the one project-open has just built.
(define (modelgen-spawn!)
  (let ((id (scene-spawn)))
    (set! *modelgen-entity* id)
    (when (>= id 0)
      (scene-name! id "model")
      (scene-material! id "project://material/modelgen-clay")
      (scene-script! id "project://script/modelgen-turntable")
      (scene-xform! id 0 modelgen-stage-y 0  0 0 0  1 1 1))
    id))

;;! (modelgen-show! i) -> the index actually shown: record the pick and bind
;;! that model's geometry onto the display entity, which is the whole of what
;;! choosing a model does. Takes an index and returns one, so it is also the
;;! shape dispatch_scm calls — a test drives the real rule rather than a
;;! test-only shim. Before a scene exists there is nothing to bind and the pick
;;! is simply remembered, which is what makes the load hook's order below safe
;;! either way round.
(define (modelgen-show! i)
  (let ((k (modelgen-clamp i)))
    (set! *modelgen-index* k)
    (when (>= *modelgen-entity* 0)
      (scene-mesh! *modelgen-entity* (modelgen-path k)))
    k))

;;! --- the turntable ----------------------------------------------------------
;;!
;;! Degrees per second. Slow: a turntable is for reading a silhouette, not for
;;! showing off. The engine's builtin://script/spinner would do the same job at
;;! its own 90 deg/s — a full turn every four seconds — and the (scene ...) DSL
;;! has no clause for overriding a script's params, so bringing the rate down to
;;! something you can actually look at means bringing the script with it.
(define modelgen-spin-rate 32)

;;! (modelgen-spin-deg t) -> the model's yaw in degrees at clock T. Pure, and a
;;! function of the clock alone rather than of an accumulated angle, so a frame
;;! hitch cannot make the turntable drift and a test can ask where it should be
;;! at any time without stepping it there.
(define (modelgen-spin-deg t)
  (* t modelgen-spin-rate))

;;! (modelgen-turntable-tick! self t) — the turntable script's on-tick: one euler
;;! set per frame, Y only. The model's position comes from its authored local
;;! transform, recomposed every frame, so writing the rotation alone leaves it
;;! standing exactly where it was put. A fault is caught so a bad frame never
;;! takes the viewer down, mirroring chess-camera-tick!.
(define (modelgen-turntable-tick! self t)
  (catch #t
         (lambda ()
           (entity-set-euler! self 0 (modelgen-spin-deg t) 0))
         (lambda args (krudd-log 2 "modelgen: turntable fault") #f)))

;;! The (script ...) source itself — one on-tick clause dispatching to the
;;! procedure above, written a line per string the way the engine's own built-in
;;! script sources are (world/asset/include/asset/builtin_scripts.h).
(define modelgen-turntable-script
  (string-append "(script modelgen-turntable\n"
                 "  (on-tick (self t)\n"
                 "    (modelgen-turntable-tick! self t)))\n"))

;;! Register it as this source is evaluated, which is before any scene is built
;;! and so before modelgen-spawn! binds the path. A reload re-runs this and
;;! replaces the source in place (script-define! keeps the id), so a model
;;! already turning keeps turning.
(script-define! "project://script/modelgen-turntable" modelgen-turntable-script)

;;! --- the dropdown -----------------------------------------------------------
;;!
;;! The gui host draws the loaded project's own panel every tick, through one
;;! seam: a procedure installed with project-set-panel! (the note above
;;! project-panel in krudd/engine/game/project/project.scm). This project's
;;! panel is one dropdown, and the dropdown owns no state — it takes the current
;;! index and hands back the chosen one — so the pick lives here, in
;;! *modelgen-index*, where modelgen_test.c can read it without a gui.
;;!
;;! What the panel draws with is gui vocabulary (kruddgui-dropdown and the
;;! layout constants around it), and none of it is reached for until the gui
;;! host calls this procedure — which is also what loads the image defining it,
;;! so there is nothing here to guard.

;;! How wide the panel is, in CSS px: enough for the longest label and the
;;! caret, narrow enough to leave the model unobscured on a phone.
(define modelgen-panel-w 190)

;;! (modelgen-panel-draw) — the panel, top-left inside the safe area, drawn once
;;! per tick by the gui host. Everything it touches is gui vocabulary; the one
;;! line of game in it is the last: a pick that differs from what is showing
;;! puts the new model on the turntable.
(define (modelgen-panel-draw)
  (let* ((ins  (kruddgui-insets))
         (x    (+ (cadddr ins) kruddgui-margin))
         (y    (+ (car ins) kruddgui-margin))
         (pick (kruddgui-dropdown "modelgen" x y modelgen-panel-w "MODEL"
                                  (modelgen-labels) *modelgen-index*)))
    (when (not (= pick *modelgen-index*))
      (modelgen-show! pick))))

;;! (modelgen-install-panel!) — hand the panel over. Run from the load hook,
;;! because the slot is emptied on every load: a project installs its panel as
;;! part of starting up, the way it builds its scene.
(define (modelgen-install-panel!)
  (project-set-panel! modelgen-panel-draw))

;;! --- loading ----------------------------------------------------------------

;;! (modelgen-reset) — a fresh viewer: a display entity in the world the load
;;! just built, the first model on it, and the panel installed. Variadic because
;;! dispatch_scm always calls with one throwaway argument, yet a test may call
;;! it with none, exactly like chess-reset.
(define (modelgen-reset . ignored)
  (modelgen-spawn!)
  (modelgen-show! 0)
  (modelgen-install-panel!))

;;! --- the models this project brings with it ---------------------------------
;;!
;;! The six pieces, copied from projects/chess/chess.scm and renamed. Each is
;;! authored base-at-origin (its foot sits on y = 0, so the plinth's top is the
;;! only height the viewer has to know) and sized against a one-unit square: a
;;! body no wider than ~0.6 units, heights rising pawn < knight < rook < bishop
;;! < queen < king. Five are pure surfaces of revolution — a single (r y)
;;! silhouette swept around Y, mesh-lathe deriving the smooth normals — so the
;;! set reads as one turned family; the knight is not, and says so where it is.
;;! Written a line per string the way the engine's own built-in mesh sources are
;;! (world/asset/include/asset/builtin_mesh_scripts.h), so the text reads as the
;;! Scheme it is.

;;! pawn — the simplest turning: a flared foot, a slim waist under a collar, and
;;! a round ball head. 15 silhouette points, 24 sectors: 375 verts / 2016
;;! indices.
(define modelgen-pawn-mesh
  (string-append "(mesh modelgen-pawn\n"
                 "  (generate ()\n"
                 "    (mesh-lathe\n"
                 "      (list\n"
                 "        (list 0 0) (list 0.26 0) (list 0.27 0.05)\n"
                 "        (list 0.2 0.08) (list 0.13 0.16) (list 0.115 0.28)\n"
                 "        (list 0.175 0.34) (list 0.2 0.375) (list 0.11 0.4)\n"
                 "        (list 0.105 0.45) (list 0.165 0.52) (list 0.185 0.6)\n"
                 "        (list 0.155 0.7) (list 0.085 0.775) (list 0 0.8)\n"
                 "      )\n"
                 "      24)))\n"))

;;! rook — a castle turret: a heavy foot, a straight shaft, and a crown that
;;! flares to a raised rim over a recessed top. A lathe cannot cut the
;;! crenellation notches, so this revolved rook keeps the ring silhouette and
;;! leaves the battlements smooth. 15 points: 375 verts / 2016 indices.
(define modelgen-rook-mesh
  (string-append "(mesh modelgen-rook\n"
                 "  (generate ()\n"
                 "    (mesh-lathe\n"
                 "      (list\n"
                 "        (list 0 0) (list 0.29 0) (list 0.3 0.05)\n"
                 "        (list 0.22 0.1) (list 0.175 0.18) (list 0.165 0.4)\n"
                 "        (list 0.205 0.46) (list 0.15 0.52) (list 0.165 0.6)\n"
                 "        (list 0.255 0.66) (list 0.27 0.74) (list 0.27 0.82)\n"
                 "        (list 0.2 0.82) (list 0.19 0.76) (list 0 0.76)\n"
                 "      )\n"
                 "      24)))\n"))

;;! bishop — a tall slim body under a bulbous mitre tapering to a finial ball.
;;! The mitre's diagonal slit is not a surface of revolution and is omitted. 17
;;! points: 425 verts / 2304 indices.
(define modelgen-bishop-mesh
  (string-append "(mesh modelgen-bishop\n"
                 "  (generate ()\n"
                 "    (mesh-lathe\n"
                 "      (list\n"
                 "        (list 0 0) (list 0.25 0) (list 0.26 0.05)\n"
                 "        (list 0.185 0.1) (list 0.12 0.2) (list 0.11 0.4)\n"
                 "        (list 0.175 0.46) (list 0.115 0.52) (list 0.1 0.6)\n"
                 "        (list 0.155 0.72) (list 0.185 0.82) (list 0.16 0.92)\n"
                 "        (list 0.1 1) (list 0.075 1.04) (list 0.09 1.09)\n"
                 "        (list 0.055 1.14) (list 0 1.17)\n"
                 "      )\n"
                 "      24)))\n"))

;;! queen — the bishop's body grown taller with a coronet: the head flares wide
;;! to a rim (a real crown's points are not revolvable, so the rim stays a
;;! smooth ring) and a small neck and ball finish it. 19 points: 475 verts /
;;! 2592 indices.
(define modelgen-queen-mesh
  (string-append "(mesh modelgen-queen\n"
                 "  (generate ()\n"
                 "    (mesh-lathe\n"
                 "      (list\n"
                 "        (list 0 0) (list 0.29 0) (list 0.3 0.05)\n"
                 "        (list 0.21 0.11) (list 0.14 0.22) (list 0.125 0.48)\n"
                 "        (list 0.195 0.55) (list 0.12 0.61) (list 0.11 0.7)\n"
                 "        (list 0.16 0.8) (list 0.225 0.92) (list 0.265 1.02)\n"
                 "        (list 0.27 1.08) (list 0.21 1.1) (list 0.17 1.06)\n"
                 "        (list 0.115 1.14) (list 0.155 1.24) (list 0.085 1.3)\n"
                 "        (list 0 1.34)\n"
                 "      )\n"
                 "      24)))\n"))

;;! king — the tallest piece: the queen's proportions with a taller crown and a
;;! knobbed finial. The chess set gives its king a cross built from two small
;;! crossed boxes parented to it; a model on a turntable is one mesh and one
;;! entity, so this one ends at the finial — the lathe body is the model, and
;;! the cross was scene furniture rather than geometry. 19 points: 475 verts /
;;! 2592 indices.
(define modelgen-king-mesh
  (string-append "(mesh modelgen-king\n"
                 "  (generate ()\n"
                 "    (mesh-lathe\n"
                 "      (list\n"
                 "        (list 0 0) (list 0.3 0) (list 0.31 0.05)\n"
                 "        (list 0.22 0.11) (list 0.15 0.24) (list 0.135 0.52)\n"
                 "        (list 0.205 0.59) (list 0.13 0.65) (list 0.12 0.76)\n"
                 "        (list 0.17 0.86) (list 0.235 0.98) (list 0.275 1.1)\n"
                 "        (list 0.28 1.16) (list 0.22 1.18) (list 0.18 1.14)\n"
                 "        (list 0.135 1.22) (list 0.155 1.32) (list 0.12 1.4)\n"
                 "        (list 0 1.44)\n"
                 "      )\n"
                 "      24)))\n"))

;;! knight — the one model that is NOT a surface of revolution, so it is a
;;! deliberately pragmatic, blocky approximation rather than a sculpted horse: a
;;! revolved pedestal (mesh-lathe, capped flat on top) fused with two tilted
;;! boxes — a head block and a shorter snout — forward-leaning about the X axis.
;;! The three sub-meshes are welded index-offset by modelgen-fuse; modelgen-box
;;! builds an axis-aligned box from the six quad faces and modelgen-tilt rotates
;;! and translates a sub-mesh into place. Faces the +Z ("forward") side, which
;;! the turntable carries past the camera once a turn. 273 verts / 1224 indices.
(define modelgen-knight-mesh
  (string-append "(mesh modelgen-knight\n"
                 "  (generate ()\n"
                 "    (define (modelgen-box hx hy hz)\n"
                 "      (let ((faces (list\n"
                 "              (list (list  1.0 0.0 0.0) (list 0.0 0.0 1.0) (list 0.0 1.0 0.0))\n"
                 "              (list (list -1.0 0.0 0.0) (list 0.0 0.0 1.0) (list 0.0 1.0 0.0))\n"
                 "              (list (list 0.0  1.0 0.0) (list 1.0 0.0 0.0) (list 0.0 0.0 1.0))\n"
                 "              (list (list 0.0 -1.0 0.0) (list 1.0 0.0 0.0) (list 0.0 0.0 1.0))\n"
                 "              (list (list 0.0 0.0  1.0) (list 1.0 0.0 0.0) (list 0.0 1.0 0.0))\n"
                 "              (list (list 0.0 0.0 -1.0) (list 1.0 0.0 0.0) (list 0.0 1.0 0.0)))))\n"
                 "        (let loop ((f 0) (verts '()) (indices '()))\n"
                 "          (if (= f 6)\n"
                 "              (cons (map (lambda (vx)\n"
                 "                           (list (* (list-ref vx 0) (* 2.0 hx))\n"
                 "                                 (* (list-ref vx 1) (* 2.0 hy))\n"
                 "                                 (* (list-ref vx 2) (* 2.0 hz))\n"
                 "                                 (list-ref vx 3) (list-ref vx 4) (list-ref vx 5)\n"
                 "                                 (list-ref vx 6) (list-ref vx 7)))\n"
                 "                         verts)\n"
                 "                    indices)\n"
                 "              (let ((fc (list-ref faces f)))\n"
                 "                (loop (+ f 1)\n"
                 "                      (append verts (mesh-quad-verts (car fc) (cadr fc) (caddr fc) 0.5))\n"
                 "                      (append indices (mesh-quad-indices (* f 4)))))))))\n"
                 "    (define (modelgen-tilt blob ang tx ty tz)\n"
                 "      (let ((c (cos ang)) (s (sin ang)))\n"
                 "        (cons (map (lambda (vx)\n"
                 "                     (let ((px (list-ref vx 0)) (py (list-ref vx 1)) (pz (list-ref vx 2))\n"
                 "                           (nx (list-ref vx 3)) (ny (list-ref vx 4)) (nz (list-ref vx 5)))\n"
                 "                       (list (+ px tx) (+ (- (* py c) (* pz s)) ty)\n"
                 "                             (+ (+ (* py s) (* pz c)) tz)\n"
                 "                             nx (- (* ny c) (* nz s)) (+ (* ny s) (* nz c))\n"
                 "                             (list-ref vx 6) (list-ref vx 7))))\n"
                 "                   (car blob))\n"
                 "              (cdr blob))))\n"
                 "    (define (modelgen-fuse a b)\n"
                 "      (let ((na (length (car a))))\n"
                 "        (cons (append (car a) (car b))\n"
                 "              (append (cdr a) (map (lambda (i) (+ i na)) (cdr b))))))\n"
                 "    (let ((base (mesh-lathe\n"
                 "                  (list (list 0.0 0.0) (list 0.28 0.0) (list 0.29 0.05)\n"
                 "                        (list 0.21 0.1) (list 0.16 0.2) (list 0.155 0.34)\n"
                 "                        (list 0.185 0.42) (list 0.15 0.47) (list 0.0 0.47))\n"
                 "                  24))\n"
                 "          (head (modelgen-tilt (modelgen-box 0.12 0.23 0.28) -0.42 0.0 0.6 0.02))\n"
                 "          (snout (modelgen-tilt (modelgen-box 0.1 0.09 0.16) -0.15 0.0 0.55 0.24)))\n"
                 "      (modelgen-fuse (modelgen-fuse base head) snout))))\n"))

;;! Register the six while this source is evaluated, which is before any scene
;;! is built — so by the time modelgen-show! resolves a path, the asset is
;;! there. A reload re-runs this and replaces each source in place (mesh-define!
;;! keeps the id), so a model already on the turntable keeps its geometry rather
;;! than losing it to a stale id. Returns 0 apiece with no asset catalog under
;;! it, which is what the lean half of a project test boots.
(mesh-define! "project://mesh/modelgen-pawn" modelgen-pawn-mesh)
(mesh-define! "project://mesh/modelgen-knight" modelgen-knight-mesh)
(mesh-define! "project://mesh/modelgen-rook" modelgen-rook-mesh)
(mesh-define! "project://mesh/modelgen-bishop" modelgen-bishop-mesh)
(mesh-define! "project://mesh/modelgen-queen" modelgen-queen-mesh)
(mesh-define! "project://mesh/modelgen-king" modelgen-king-mesh)

;;! --- the display material ---------------------------------------------------
;;!
;;! One material for whatever is on the turntable: a pale matte clay, the
;;! neutral a model is judged against. Metallic 0 and a middling roughness, so
;;! the analytic highlight rolls slowly across a turning silhouette and reads
;;! the form rather than flaring off it — which is the whole reason a viewer
;;! turns a model instead of showing a still of it.
(define modelgen-clay-material
  (string-append "(material modelgen-clay\n"
                 "  (shader \"builtin://shader/pbr\")\n"
                 "  (base_color 0.82 0.80 0.76 1.0)\n"
                 "  (metallic 0.0)\n"
                 "  (roughness 0.45)\n"
                 "  (subsurface 0.0))\n"))

;;! Registered alongside the meshes and for the same reasons: before any scene
;;! is built, so the spawn's material path resolves, and replaced in place on a
;;! reload (material-define! keeps the id) so a model already showing keeps its
;;! surface.
(material-define! "project://material/modelgen-clay" modelgen-clay-material)

;;! --- the project ------------------------------------------------------------
;;!
;;! The form game/project's host reads. The scene is the stage and nothing else:
;;! the camera's fixed eye, a wide matte ground, and the plinth the model stands
;;! on — the model itself arrives from the load hook (see *modelgen-entity*).
;;! There is no (on-tick ...): the turntable is an entity script, ticked by the
;;! engine like any other, and the dropdown is drawn by the gui host, so this
;;! project has no per-frame work of its own. There is no (on-selected ...)
;;! either — nothing here is clickable in the world; the one thing you can pick
;;! is the model, and you pick it in the panel.
(project "Modelgen"
         (scene modelgen
                ;;! The eye: a little above the model's waist and far enough
                ;;! back that the tallest model clears the frame at the
                ;;! renderer's 0.8 rad vertical fov — about 2.6 m of visible
                ;;! height at this distance, against a 1.44 m king. No mesh and
                ;;! no script: a fixed position is the whole camera rig,
                ;;! because here the model turns instead of the view.
                (entity (name "Camera") (at 0 0.55 3.1))

                ;;! A wide matte ground under the whole thing, and the plinth
                ;;! standing on it — a squat dark box whose TOP is
                ;;! modelgen-stage-y, which is where a model's foot goes.
                (entity (name "ground")
                        (mesh     "builtin://mesh/plane")
                        (material "builtin://material/pbr-stone")
                        (at 0 -0.92 0) (scale 14 1 14))
                (entity (name "plinth")
                        (mesh     "builtin://mesh/box")
                        (material "builtin://material/board-dark")
                        (at 0 -0.75 0) (scale 1.6 0.3 1.6)))
         (on-load modelgen-reset))
