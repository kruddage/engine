; SPDX-License-Identifier: GPL-2.0-or-later

;;! ducks — the carnival duck shoot: a rank of ducks riding a conveyor across
;;! the back of a booth, a gun on a post that pivots to follow your aim, a red
;;! crosshair out on the scene, and a ding when one goes down.
;;!
;;! This is the SKELETON of that game. What is here is the whole of the geometry
;;! and the whole of the arithmetic — the conveyor and its wrap, the gun's pivot,
;;! where the crosshair lands for a given pivot, and what counts as a hit — each
;;! written as a pure procedure and each checked by ducks_test.c. What is NOT
;;! here is the input that would drive any of it, and that is not an oversight:
;;!
;;!   THERE IS NO PRESS-AND-HOLD FOR SCHEME TO READ. The only pointer signal a
;;!   project can see today is (scene-selected) — the id of an entity the
;;!   selection CHANGED to, delivered by the host as one call per click. That is
;;!   a click on a thing, and this game needs a press with a position, held over
;;!   time, on nothing in particular. Closing that gap is engine work, it is a
;;!   sub-issue of the initiative this landed under, and until it lands the aim
;;!   below SWEEPS ON A TIMER instead — ducks-sweep-yaw, wired into the gun and
;;!   crosshair scripts, standing in for a hand. Everything downstream of the
;;!   aim is real and takes a yaw, so the day the primitive exists the stand-in
;;!   is deleted and nothing else moves.
;;!
;;! Distances are METRES: one world unit is one metre, the convention stated
;;! in krudd/engine/base/math/include/math/camera.h and the one
;;! projects/training was the first scene built to. A booth you stand at is
;;! a room-scale object, so it is worth this one being right from the start.

;;! --- the booth, in metres ---------------------------------------------------
;;!
;;! The ducks ride a conveyor 4 m back from the origin, at chest height, and the
;;! player stands at the counter near z = +1. The lane runs 7 m wide, which at
;;! the booth's distance fills the view without the end ducks sliding off it.
;;! In metres: how far back the conveyor runs, the left and right ends of the
;;! lane, the span between them, and the height a duck rides at.
(define ducks-lane-z -4.0)
(define ducks-lane-min -3.5)
(define ducks-lane-max 3.5)
(define ducks-lane-span 7.0)
(define ducks-duck-y 1.15)

;;! Five ducks evenly spaced over the span. The spacing is the span divided by
;;! the count rather than a number chosen by eye, which is what makes the wrap
;;! seamless: the duck leaving the right end is exactly one spacing behind the
;;! one that has just left the left end, so the rank stays even forever instead
;;! of bunching up after a lap.
(define ducks-count 5)
(define ducks-spacing (/ ducks-lane-span ducks-count))
;;! Metres per second — a fairground amble.
(define ducks-speed 0.9)

;;! The gun: fixed on its post at the counter, pivoting about Y only. Its
;;! distance BACK from the lane is the one number the aim arithmetic needs — the
;;! pivot and the lane form a right triangle whose far side is the lane and
;;! whose near side is this.
(define ducks-gun-x 0.0)
;;! The pivot height and how far in front of the lane it stands, in metres, and
;;! the 5.2 m reach between them that the aim arithmetic is built on.
(define ducks-gun-y 1.05)
(define ducks-gun-z 1.2)
(define ducks-gun-reach (- ducks-gun-z ducks-lane-z))

;;! How close the crosshair has to be to a duck's centre to knock it down. A
;;! duck is about 0.44 m long, so a little over half that: generous enough to be
;;! a fairground game rather than a marksmanship test.
(define ducks-hit-radius 0.22)

;;! How fast the stand-in aim sweeps, in radians of phase per second. Slow
;;! enough to watch a duck being led. Deleted with the rest of the stand-in when
;;! a real press-and-hold arrives.
(define ducks-sweep-rate 0.6)

;;! --- the conveyor -----------------------------------------------------------

;;! (ducks-wrap x) -> X folded into [lane-min, lane-max). A duck that runs off
;;! the right end reappears at the left carrying its overshoot, so it keeps
;;! exactly the speed it had rather than losing a frame's worth of travel to the
;;! wrap. Written as a modulo of the offset from the left end rather than a
;;! "if past the end, subtract the span" test, which only holds for one lap and
;;! silently stops holding when a frame hitch is longer than the lane.
(define (ducks-wrap x)
  (+ ducks-lane-min
     (let ((off (- x ducks-lane-min)))
       (- off (* ducks-lane-span (floor (/ off ducks-lane-span)))))))

;;! (ducks-advance x dt) -> where a duck at X is DT seconds later. Pure, and
;;! taking its own dt rather than reading a clock, so the test can step it.
(define (ducks-advance x dt)
  (ducks-wrap (+ x (* ducks-speed dt))))

;;! (ducks-x-at i t) -> where duck I is at time T. The whole conveyor is a
;;! function of the clock and the duck's index, with no per-duck state at all:
;;! each duck starts one spacing along from the last and they all travel
;;! together, so the rank is fully determined by T. That is what lets the ducks
;;! be driven by an entity script — which is handed the frame time and nothing
;;! else — without the project having to hold a position per duck and push it.
(define (ducks-x-at i t)
  (ducks-wrap (+ ducks-lane-min (* i ducks-spacing) (* ducks-speed t))))

;;! (ducks-index-from-x x) -> which duck in the rank sits at authored position X.
;;! The inverse of the (* i ducks-spacing) offset in ducks-x-at, so the scene
;;! clause does not have to state a duck's index twice: placing it at the right
;;! spot IS saying which duck it is.
(define (ducks-index-from-x x)
  (round (/ (- x ducks-lane-min) ducks-spacing)))

;;! (ducks-phase-of self) -> which duck in the rank entity SELF is, read out of
;;! where the scene clause authored it. The rank is stateless: no table, no
;;! capture, nothing to invalidate on a reload — the duck's index is a fact
;;! about the scene, and the scene is where it is read from, every frame.
;;!
;;! That works because of a distinction in the entity-script surface worth
;;! naming, since getting it backwards produces a game that runs and does not
;;! move. entity-base-position reports the entity's LOCAL transform — what the
;;! scene clause authored — while entity-set-position! writes its WORLD
;;! transform, which is this frame's render override, recomposed from local at
;;! the start of every frame. A script therefore reads its rest pose and writes
;;! its animated pose, and the two never collide: the authored position stays
;;! authored no matter how many frames have written over the rendered one.
;;!
;;! The other route — read the entity's name and parse the index out of it, the
;;! way chess reads a piece's colour out of its name — is NOT open here:
;;! scene-entity-name is a scene primitive, the world is bound for it only for
;;! the span of a scene call, and from inside an entity script's tick it answers
;;! "" for every id.
(define (ducks-phase-of self)
  (let ((p (entity-base-position self)))
    (if (pair? p) (ducks-index-from-x (car p)) 0)))

;;! --- the aim ----------------------------------------------------------------
;;!
;;! The gun pivots about Y and the ducks ride a straight lane, so aim is one
;;! right triangle: the gun's reach to the lane is the adjacent side, the offset
;;! along the lane is the opposite, and the yaw is the angle between. Both
;;! directions are written out because both are used — the gun needs the yaw for
;;! a target, the crosshair needs the target for a yaw — and because a pair of
;;! inverses is checkable against itself, which is what ducks_test.c does rather
;;! than restating either formula.
;;!
;;! Yaw is DEGREES, positive turning the gun to +X, because that is the unit
;;! entity-set-euler! takes and the unit the scene clause's (rotate ...) is
;;! authored in. The radian conversion stays inside these two procedures.

(define ducks-deg-per-rad (/ 180.0 pi))

;;! (ducks-yaw-for x) -> the yaw, in degrees, that points the gun at X on the
;;! lane.
(define (ducks-yaw-for x)
  (* ducks-deg-per-rad (atan (- x ducks-gun-x) ducks-gun-reach)))

;;! (ducks-aim-x yaw) -> where on the lane a gun at YAW degrees is pointing.
(define (ducks-aim-x yaw)
  (+ ducks-gun-x (* ducks-gun-reach (tan (/ yaw ducks-deg-per-rad)))))

;;! (ducks-sweep-yaw t) -> the stand-in aim: a yaw sweeping smoothly between the
;;! two ends of the lane. THIS IS THE PART A HAND REPLACES. Everything it feeds
;;! takes a yaw and does not care where the yaw came from.
(define (ducks-sweep-yaw t)
  (* (ducks-yaw-for ducks-lane-max) (sin (* ducks-sweep-rate t))))

;;! --- hits -------------------------------------------------------------------

;;! Whose turn it is to be shot at, so to speak: the running score, and the
;;! index of the duck the last shot took down (-1 for a miss). Wrapped in
;;! earmuffs the way chess wraps its turn and selection.
(define *ducks-score* 0)
(define *ducks-last-hit* -1)

;;! (ducks-hit? cross-x duck-x) -> #t when a shot landing at CROSS-X on the lane
;;! knocks down a duck centred at DUCK-X.
(define (ducks-hit? cross-x duck-x)
  (< (abs (- cross-x duck-x)) ducks-hit-radius))

;;! (ducks-duck-under x t) -> the index of the duck the crosshair is over at
;;! time T, or -1. The nearest one wins when two overlap, which they cannot at
;;! this spacing and radius, but a rule that depends on that not happening is a
;;! rule that breaks the day the spacing changes.
(define (ducks-duck-under x t)
  (let loop ((i 0) (best -1) (best-d ducks-hit-radius))
    (if (>= i ducks-count)
        best
        (let ((d (abs (- x (ducks-x-at i t)))))
          (if (< d best-d)
              (loop (+ i 1) i d)
              (loop (+ i 1) best best-d))))))

;;! (ducks-shoot! yaw t) -> 1 if the shot taken at YAW at time T took a duck
;;! down, 0 if it missed. The whole of what a trigger pull will do once there is
;;! a trigger: work out what the crosshair is over, score it, and fire the
;;! feedback. Deliberately takes its yaw and its time rather than reading either
;;! from the world, so a test can pull the trigger at a chosen moment.
(define (ducks-shoot! yaw t)
  (let* ((x (ducks-aim-x yaw))
         (i (ducks-duck-under x t)))
    (set! *ducks-last-hit* i)
    (ducks-muzzle-flash! yaw)
    (if (>= i 0)
        (begin
          (set! *ducks-score* (+ *ducks-score* 1))
          (ducks-ding!)
          1)
        0)))

;;! (ducks-pull-trigger! t-ms) -> 1 on a hit, 0 on a miss. The dispatch-shaped
;;! door onto ducks-shoot!: dispatch_scm hands a named procedure ONE INTEGER and
;;! takes one back, so the moment arrives in milliseconds and the aim is read
;;! off the sweep at that instant. That shape is not a compromise for the test's
;;! benefit — it is the shape the host calls a project's rules in, and it is the
;;! call a trigger will arrive as. When a real press-and-hold lands, the
;;! pointer's yaw replaces the sweep on this one line and ducks-shoot! does not
;;! change at all.
(define (ducks-pull-trigger! t-ms)
  (let ((t (/ t-ms 1000.0)))
    (ducks-shoot! (ducks-sweep-yaw t) t)))

;;! (ducks-score ignored) -> the running score, and (ducks-last-hit ignored) the
;;! duck the last shot took. Poll hooks in the shape dispatch_scm calls: one
;;! throwaway integer in, one integer out. This is how ducks_test.c — and the
;;! score display a later slice will hang off the gui — reads the game.
(define (ducks-score ignored) *ducks-score*)
(define (ducks-last-hit ignored) *ducks-last-hit*)

;;! (ducks-reset . ignored) — a fresh game, run by the (on-load ...) hook.
;;! Variadic for the reason chess-reset is: dispatch_scm always passes one
;;! argument and the tests may pass none.
(define (ducks-reset . ignored)
  (set! *ducks-score* 0)
  (set! *ducks-last-hit* -1))

;;! --- feedback ---------------------------------------------------------------

;;! (ducks-muzzle-flash! yaw) — the puff of particles a shot throws out of the
;;! barrel, in a hot muzzle-white, placed a little way down the barrel from the
;;! pivot so it reads as coming out of the gun rather than out of the post.
;;! Guarded on particle-burst! being bound exactly as chess-spark is: the
;;! renderer registers it, the headless test runs with no renderer, and the
;;! rules never depend on it.
;;!
;;! A single burst per shot for now. The hold-the-button STREAM the game wants —
;;! particles pouring out for as long as the press lasts — needs the press this
;;! file cannot see, so it arrives with it.
;;! Metres from the pivot out to the muzzle.
(define ducks-muzzle-out 0.55)

(define (ducks-muzzle-flash! yaw)
  (when (defined? 'particle-burst!)
    (let* ((rad (/ yaw ducks-deg-per-rad))
           (x (+ ducks-gun-x (* ducks-muzzle-out (sin rad))))
           (z (- ducks-gun-z (* ducks-muzzle-out (cos rad)))))
      (particle-burst! x (+ ducks-gun-y 0.15) z 1.0 0.93 0.7 20))))

;;! (ducks-ding!) — the bell. A duck going down is the one sound this game
;;! really owes you, and it is the engine's own built-in beep pitched up, so
;;! nothing has to be authored here. Guarded like the flash, and for the same
;;! reason: the headless test boots no audio subsystem.
(define (ducks-ding!)
  (when (defined? 'audio-play!)
    (audio-play! "builtin://sound/beep" 0.7 0.0 1.6)))

;;! --- the scripts this game brings with it ------------------------------------
;;!
;;! Three entity scripts, and they exist because an entity script is the one
;;! hook in the engine that is handed the FRAME TIME. A project's own
;;! (on-tick ...) is called with no arguments, so a project cannot animate
;;! anything from it without holding a clock of its own; a script's
;;! (on-tick (self t) ...) gets t, which is exactly what the conveyor and the
;;! sweep are functions of. Chess drives its camera the same way.

;;! The duck script: work out which duck in the rank this entity is, then put it
;;! where the conveyor says that duck is at time T. Faulting is caught so one bad
;;! frame never takes the game down, mirroring chess-camera-tick!.
(define (ducks-duck-tick! self t)
  (catch #t
         (lambda ()
           (entity-set-position! self (ducks-x-at (ducks-phase-of self) t)
                                 ducks-duck-y ducks-lane-z))
         (lambda args (krudd-log 2 "ducks: duck fault") #f)))

;;! The gun script: pivot the barrel to the current aim. One euler set per
;;! frame, Y only — the gun is bolted down and turns, it does not move.
(define (ducks-gun-tick! self t)
  (catch #t
         (lambda ()
           (entity-set-euler! self 0 (ducks-sweep-yaw t) 0))
         (lambda args (krudd-log 2 "ducks: gun fault") #f)))

;;! The crosshair script: park the marker where the gun is pointing, just in
;;! front of the lane so it reads as being over the ducks rather than buried in
;;! them.
;;!
;;! This is a WORLD-SPACE stand-in for what the game wants, which is a crosshair
;;! painted over the scene in screen space — a fixed reticle the world moves
;;! behind. That is gui work (kruddgui already paints in the play view) and it
;;! is its own sub-issue; a marker on the lane is the readable placeholder,
;;! because it puts the aim somewhere you can see it and check it against a
;;! duck.
;;! Metres in front of the lane, so the marker reads as being over the ducks.
(define ducks-crosshair-lift 0.35)

(define (ducks-crosshair-tick! self t)
  (catch #t
         (lambda ()
           (entity-set-position! self (ducks-aim-x (ducks-sweep-yaw t))
                                 ducks-duck-y
                                 (+ ducks-lane-z ducks-crosshair-lift)))
         (lambda args (krudd-log 2 "ducks: crosshair fault") #f)))

;;! The three (script ...) sources, each one on-tick clause dispatching to the
;;! procedure above it, written a line per string the way the engine's own
;;! built-in script sources are. project://, not builtin://: the engine seeds
;;! nothing named ducks, and these are in the catalog only because the
;;! script-define! calls below put them there — before any scene is built, so
;;! the scene clause's (script ...) paths resolve when the launcher opens the
;;! booth.
(define ducks-duck-script
  (string-append "(script ducks-duck\n"
                 "  (on-tick (self t)\n"
                 "    (ducks-duck-tick! self t)))\n"))

(define ducks-gun-script
  (string-append "(script ducks-gun\n"
                 "  (on-tick (self t)\n"
                 "    (ducks-gun-tick! self t)))\n"))

(define ducks-crosshair-script
  (string-append "(script ducks-crosshair\n"
                 "  (on-tick (self t)\n"
                 "    (ducks-crosshair-tick! self t)))\n"))

(script-define! "project://script/ducks-duck" ducks-duck-script)
(script-define! "project://script/ducks-gun" ducks-gun-script)
(script-define! "project://script/ducks-crosshair" ducks-crosshair-script)

;;! --- the materials this game brings with it ----------------------------------
;;!
;;! A fairground palette: painted ducks, a red-and-white booth, and a crosshair
;;! that is a light rather than a surface.
(define ducks-duck-material
  (string-append "(material ducks-duck\n"
                 "  (shader \"builtin://shader/pbr\")\n"
                 "  (base_color 0.95 0.78 0.16 1.0)\n"
                 "  (metallic 0.0)\n"
                 "  (roughness 0.45))\n"))

(define ducks-booth-material
  (string-append "(material ducks-booth\n"
                 "  (shader \"builtin://shader/pbr\")\n"
                 "  (base_color 0.62 0.11 0.13 1.0)\n"
                 "  (metallic 0.0)\n"
                 "  (roughness 0.6))\n"))

;;! The crosshair is emissive and nothing else — it is a sight, so it has to
;;! stay legible against a lit duck at any distance, which a shaded red surface
;;! would not. Driven past 1 for the same reason training's grid lines are.
(define ducks-crosshair-material
  (string-append "(material ducks-crosshair\n"
                 "  (shader \"builtin://shader/pbr\")\n"
                 "  (base_color 0.1 0.01 0.01 1.0)\n"
                 "  (metallic 0.0)\n"
                 "  (roughness 0.5)\n"
                 "  (emissive 2.4 0.12 0.1))\n"))

(material-define! "project://material/ducks-duck" ducks-duck-material)
(material-define! "project://material/ducks-booth" ducks-booth-material)
(material-define! "project://material/ducks-crosshair" ducks-crosshair-material)

;;! --- the project ------------------------------------------------------------
;;!
;;! The booth: a ground plane, a backboard behind the lane, a counter to stand
;;! at, the conveyor the ducks ride, five ducks, the gun on its post, and the
;;! crosshair.
;;!
;;! THE DUCKS ARE SPHERES. A duck wants a lathed silhouette of its own —
;;! mesh-define! and the same turned-profile treatment chess gives its pieces —
;;! and that is a sub-issue rather than something to fake here with a pile of
;;! parented boxes. What a scaled sphere does buy is the right FOOTPRINT: it is
;;! authored the size a duck is, so the hit radius, the spacing and the lane are
;;! all being checked against a real 0.44 m target and the mesh can be swapped
;;! underneath without any of those numbers moving.
;;!
;;! A duck's authored X is where the conveyor puts it at t = 0, so the scene as
;;! authored is the scene at rest — the booth looks right before a single frame
;;! has ticked — and it is also how the duck script learns which duck it is on
;;! (ducks-phase-of). The "duck-N" names are for a reader and for the test; the
;;! game itself reads the rank off the geometry.
(project "Ducks"
         (scene ducks
                ;;! Standing at the counter, a little above it, looking down
                ;;! the booth at the lane.
                (entity (name "Camera") (at 0 1.9 3.4))

                (entity (name "ground")
                        (mesh     "builtin://mesh/plane")
                        (material "builtin://material/pbr-stone")
                        (at 0 0 0) (scale 24 1 24))

                ;;! The backboard the ducks pass in front of, and the counter
                ;;! you lean on. Both are boxes, so their scale is their size in
                ;;! metres directly.
                (entity (name "backboard")
                        (mesh     "builtin://mesh/box")
                        (material "project://material/ducks-booth")
                        (at 0 1.4 -4.6) (scale 8 2.8 0.12))
                (entity (name "counter")
                        (mesh     "builtin://mesh/box")
                        (material "project://material/ducks-booth")
                        (at 0 0.5 1.6) (scale 8 1 0.5))

                ;;! The conveyor: a belt running the width of the lane, just
                ;;! under the ducks.
                (entity (name "conveyor")
                        (mesh     "builtin://mesh/box")
                        (material "builtin://material/pbr-metal")
                        (at 0 0.95 -4) (scale 7.6 0.12 0.5))

                ;;! The rank, at their t = 0 positions: duck I starts one
                ;;! spacing along from duck I-1, spacing being 7.0 / 5 = 1.4.
                (entity (name "duck-0")
                        (mesh     "builtin://mesh/sphere")
                        (material "project://material/ducks-duck")
                        (script   "project://script/ducks-duck")
                        (at -3.5 1.15 -4) (scale 0.3 0.34 0.44))
                (entity (name "duck-1")
                        (mesh     "builtin://mesh/sphere")
                        (material "project://material/ducks-duck")
                        (script   "project://script/ducks-duck")
                        (at -2.1 1.15 -4) (scale 0.3 0.34 0.44))
                (entity (name "duck-2")
                        (mesh     "builtin://mesh/sphere")
                        (material "project://material/ducks-duck")
                        (script   "project://script/ducks-duck")
                        (at -0.7 1.15 -4) (scale 0.3 0.34 0.44))
                (entity (name "duck-3")
                        (mesh     "builtin://mesh/sphere")
                        (material "project://material/ducks-duck")
                        (script   "project://script/ducks-duck")
                        (at 0.7 1.15 -4) (scale 0.3 0.34 0.44))
                (entity (name "duck-4")
                        (mesh     "builtin://mesh/sphere")
                        (material "project://material/ducks-duck")
                        (script   "project://script/ducks-duck")
                        (at 2.1 1.15 -4) (scale 0.3 0.34 0.44))

                ;;! The gun. The post is bolted down; the barrel is the child
                ;;! that turns, so the pivot is the barrel's own origin and the
                ;;! script only ever sets its yaw. A cylinder is a unit across
                ;;! and a unit tall, so the barrel is 6 cm by 1.1 m.
                (entity (name "gun-post")
                        (mesh     "builtin://mesh/cylinder")
                        (material "builtin://material/pbr-metal")
                        (at 0 0.52 1.2) (scale 0.12 1.05 0.12))
                (entity (name "gun-barrel")
                        (mesh     "builtin://mesh/box")
                        (material "builtin://material/pbr-metal")
                        (script   "project://script/ducks-gun")
                        (at 0 1.05 1.2) (scale 0.06 0.06 1.1))

                ;;! The sight, out on the lane. A torus is a ring, which is what
                ;;! a crosshair reads as at a distance; 0.24 m across, so it
                ;;! frames a duck rather than covering it.
                (entity (name "crosshair")
                        (mesh     "builtin://mesh/torus")
                        (material "project://material/ducks-crosshair")
                        (script   "project://script/ducks-crosshair")
                        (at 0 1.15 -3.65) (scale 0.24 0.24 0.24)))

         (on-load ducks-reset))
