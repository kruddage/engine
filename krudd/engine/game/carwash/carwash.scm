; SPDX-License-Identifier: GPL-2.0-or-later

;;! carwash — an attract mode, written the way chess is: one file, no C, one
;;! (project ...) form at the bottom and the definitions it names above it. What
;;! it shows is a detail bay running on a loop. A filthy car rolls in, a pressure
;;! wand rinses it down, an applicator lays product over the flanks, a buffer
;;! swirls the hood and the trunk to a shine, the finished car turns under a slow
;;! hero orbit, and it rolls back out for the next one. Nothing is clicked and
;;! nothing is waited for: it plays itself, forever, which is the whole of what
;;! an attract mode is.
;;!
;;! Chess is the engine's showcase for authored GEOMETRY — six lathed meshes and
;;! a board. This is deliberately its opposite number and brings no mesh at all:
;;! every shape here is a builtin box, cylinder or plane, and what is authored is
;;! the LOOK and the CHOREOGRAPHY. Two things carry it:
;;!
;;!   material-define! is a per-frame call, not a load-time one. The car's paint
;;!   lives at one catalog path, and the show repacks that path ten times a
;;!   second from three scalars — how much dirt is left, how much product is on,
;;!   how far the buff has got. Every entity bound to it picks the new bytes up
;;!   because the id survives a repack (see material-define!'s own contract), so
;;!   a car going from dust-dulled brown to deep wet gloss is one asset being
;;!   rewritten under the renderer rather than a swap between two.
;;!
;;!   The camera rides the tool. scene_renderer drives only the eye from the
;;!   "Camera" entity — the look-at target is a fixed (0, 0, 0), the middle of
;;!   the car — so following a tool means swinging the eye around to the tool's
;;!   own side of the car and closing in, which is what the arcade cut between
;;!   tools is made of. It is the same trick chess's camera plays on a square,
;;!   with a moving subject instead of a stationary one.
;;!
;;! Everything the show does is a pure function of one clock. carwash-clock! is
;;! the only thing that advances state, every script calls it, and it is
;;! idempotent on the timestamp — so the tools, the car and the camera all read
;;! one frame's worth of state no matter which entity the engine happens to tick
;;! first. That is what makes the whole loop checkable from a headless test with
;;! no renderer: drive the clock, read the phase.

;;! --- the show's clock ------------------------------------------------------

;;! The loop, in order: each phase is (NAME SECONDS TOOL), where TOOL is the
;;! index of the tool that works it or -1 for the phases no tool is out for. The
;;! three work phases are the "switch tools every so often" of an arcade attract
;;! reel — six seconds is long enough to read what a tool is doing and short
;;! enough that nothing outstays its welcome. Between them sit the three phases
;;! that are pure camera: the car arriving, the reveal, and the car leaving.
;;!
;;! The roll-in and roll-out are not decoration either. The surface state resets
;;! to filthy the moment the loop wraps, and a hard cut from mirror gloss back to
;;! dirt in front of the camera would read as a bug; happening while the car is
;;! most of a lane away and moving, it reads as the next car.
(define carwash-phases
  (list (list 'roll-in  2.4 -1)
        (list 'rinse    6.0  0)
        (list 'product  6.0  1)
        (list 'buff     6.0  2)
        (list 'reveal   3.6 -1)
        (list 'roll-out 2.4 -1)))

(define (carwash-phase-count) (length carwash-phases))

(define (carwash-phase-entry i) (list-ref carwash-phases i))

;;! Which phase is running, as an index into the list above.
(define *carwash-phase* 0)

;;! Seconds into that phase.
(define *carwash-phase-t* 0.0)

;;! The clock reading (entity-script's `t`, seconds since boot) this frame's
;;! state was computed from, or -1 before the first tick. Every script's on-tick
;;! hands its `t` to carwash-clock!, which is why this is also the idempotence
;;! key: the second script to call in a frame passes the same number and gets a
;;! no-op, so state advances once per frame rather than once per scripted entity.
(define *carwash-prev-t* -1)

;;! The seconds carwash-clock! last advanced by. Read by the camera for its ease
;;! — the one consumer that needs a dt rather than the state the dt produced.
(define *carwash-dt* 0.0)

;;! The angle (radians) of the slow hero orbit. It advances every frame, in every
;;! phase, so each pass around the loop reveals the car from a fresh side rather
;;! than replaying one camera move.
(define *carwash-hero* 0.0)

;;! Seconds until the next puff of spray. Counted down rather than derived from
;;! the phase clock so the cadence is the same whatever the framerate.
(define *carwash-spray* 0.0)

;;! Seconds until the paint is repacked. The repack parses a (material ...)
;;! source and packs it to std140, so it runs on carwash-paint-every rather than
;;! once a frame; the eye cannot tell, and the frame budget can.
(define *carwash-paint* 0.0)

(define carwash-tau (* 8 (atan 1)))
(define carwash-pi  (* 4 (atan 1)))
(define carwash-deg (/ 180.0 carwash-pi))

;;! The longest step carwash-clock! will take in one go. A tab that was in the
;;! background for a minute comes back with a vast `t`, and without this the show
;;! would jump most of a lap; with it, it resumes where it left off.
(define carwash-max-dt 0.1)

;;! How fast the hero orbit turns, in radians a second.
(define carwash-hero-rate 0.28)

;;! Seconds between puffs of spray while a tool is working.
(define carwash-spray-every 0.09)

;;! Seconds between paint repacks — ten a second, which over the six seconds of a
;;! wash is sixty distinct surfaces, far finer than the eye resolves a gradient.
(define carwash-paint-every 0.1)

(define (carwash-clamp v lo hi) (max lo (min hi v)))

(define (carwash-mix a b k) (+ a (* k (- b a))))

;;! Smoothstep: 0 at 0, 1 at 1, flat at both ends. Every ramp in the show runs
;;! through it, so a tool starts and stops moving rather than snapping into and
;;! out of its pass.
(define (carwash-smooth u)
  (let ((k (carwash-clamp u 0.0 1.0)))
    (* k k (- 3.0 (* 2.0 k)))))

;;! The phase's symbol, its length, and the tool working it.
(define (carwash-now) (car (carwash-phase-entry *carwash-phase*)))
(define (carwash-len) (cadr (carwash-phase-entry *carwash-phase*)))
(define (carwash-tool) (caddr (carwash-phase-entry *carwash-phase*)))

;;! How far through the current phase, in 0..1. Held a hair under 1 because
;;! several poses divide by it or floor it into a pass number, and the instant
;;! u = 1 belongs to the next phase in any case.
(define (carwash-u)
  (carwash-clamp (/ *carwash-phase-t* (carwash-len)) 0.0 0.9999))

;;! (carwash-enter! i) — move to phase I and announce it. The cue is the arcade
;;! punctuation on a tool change: a low hiss as the wand comes out, a tap for the
;;! applicator, a whir for the buffer, and a bright chime on the reveal. Guarded
;;! on audio-play! being bound exactly as chess guards it, so the headless rules
;;! test — which boots no audio subsystem — runs the same loop in silence.
(define (carwash-enter! i)
  (set! *carwash-phase* i)
  (set! *carwash-phase-t* 0.0)
  (set! *carwash-paint* 0.0)
  (when (defined? 'audio-play!)
    (case (carwash-now)
      ((rinse)   (audio-play! "builtin://sound/noise-burst" 0.45 0.0 0.7))
      ((product) (audio-play! "builtin://sound/blip" 0.5 0.0 0.8))
      ((buff)    (audio-play! "builtin://sound/beep" 0.4 0.0 1.35))
      ((reveal)  (audio-play! "builtin://sound/beep" 0.55 0.0 1.9)))))

;;! (carwash-advance! dt) — carry the phase clock forward, rolling over into the
;;! next phase as many times as DT covers. A loop rather than one test because
;;! nothing guarantees a step lands inside the phase it started in; it is bounded
;;! by carwash-max-dt against every phase being at least two seconds, so in
;;! practice it turns over at most once.
(define (carwash-advance! dt)
  (set! *carwash-phase-t* (+ *carwash-phase-t* dt))
  (let loop ()
    (when (>= *carwash-phase-t* (carwash-len))
      (let ((over (- *carwash-phase-t* (carwash-len))))
        (carwash-enter! (modulo (+ *carwash-phase* 1) (carwash-phase-count)))
        (set! *carwash-phase-t* over))
      (loop))))

;;! (carwash-clock! t) — one frame of the show, at clock reading T. Called from
;;! every script's on-tick and idempotent on T, so whichever entity the engine
;;! ticks first in a frame is the one that advances the world and the rest read
;;! what it computed. A first tick (or a tick after a reset) takes no time at
;;! all: there is no previous reading to measure against, and inventing one would
;;! start the show at a random point in its first phase.
(define (carwash-clock! t)
  (when (not (= t *carwash-prev-t*))
    (let ((dt (if (< *carwash-prev-t* 0)
                  0.0
                  (carwash-clamp (- t *carwash-prev-t*) 0.0 carwash-max-dt))))
      (set! *carwash-prev-t* t)
      (set! *carwash-dt* dt)
      (carwash-advance! dt)
      (set! *carwash-hero* (let ((a (+ *carwash-hero* (* dt carwash-hero-rate))))
                             (if (> a carwash-tau) (- a carwash-tau) a)))
      (carwash-spray! dt)
      (carwash-repaint! dt))))

;;! --- the surface -----------------------------------------------------------
;;!
;;! Three scalars in 0..1 say everything about what the car looks like, and all
;;! three are pure functions of the phase and how far through it the show is —
;;! no accumulated state, so the loop cannot drift out of step with itself and a
;;! test can assert the surface at any moment by putting the clock there.

;;! How much of the dirt is gone: 0 filthy, 1 stripped. The rinse is the only
;;! thing that moves it, and it stays down until the loop wraps.
(define (carwash-clean)
  (case (carwash-now)
    ((roll-in) 0.0)
    ((rinse)   (carwash-smooth (carwash-u)))
    (else      1.0)))

;;! How much product is sitting on the paint: 0 none, 1 fully laid on. It goes on
;;! through the product phase and comes off through the buff, which is what makes
;;! the buff read as work rather than as a light show.
(define (carwash-product)
  (case (carwash-now)
    ((product) (carwash-smooth (carwash-u)))
    ((buff)    (- 1.0 (carwash-smooth (carwash-u))))
    (else      0.0)))

;;! How far the shine has come up: 0 flat, 1 mirror.
(define (carwash-gloss)
  (case (carwash-now)
    ((buff)             (carwash-smooth (carwash-u)))
    ((reveal roll-out)  1.0)
    (else               0.0)))

;;! How far down the lane the car is, in world X. The bay is at 0; a car arriving
;;! comes up from -X and a finished one leaves along +X.
(define carwash-lane 17.0)

(define (carwash-car-x)
  (case (carwash-now)
    ((roll-in)  (* (- carwash-lane) (- 1.0 (carwash-smooth (carwash-u)))))
    ((roll-out) (* carwash-lane (carwash-smooth (carwash-u))))
    (else       0.0)))

;;! --- the paint -------------------------------------------------------------

;;! The catalog path the car's painted panels bind. project://, not builtin://:
;;! the engine seeds nothing for this show, and the path holds bytes only because
;;! the form further down put them there.
(define carwash-paint-path "project://material/carwash-paint")

;;! The three colours the paint is mixed from: the film of road dirt it arrives
;;! under, the candy red underneath it, and the milky cast of product laid on and
;;! not yet buffed off.
(define carwash-dirt-rgb  (list 0.30 0.26 0.21))
(define carwash-body-rgb  (list 0.52 0.05 0.06))
(define carwash-foam-rgb  (list 0.88 0.88 0.85))

;;! (carwash-num v) -> V rounded to four places, as the text a (material ...)
;;! source carries. Rounded rather than printed raw so a repack writes a short,
;;! readable source instead of seventeen digits of float noise, and so two frames
;;! that land on the same surface produce byte-identical sources.
(define (carwash-num v)
  (number->string (/ (round (* 10000.0 v)) 10000.0)))

;;! (carwash-paint-source) -> the (material ...) text for the surface as it
;;! stands this instant.
;;!
;;! The mapping from the three scalars to the pbr shader's fields is the whole of
;;! the look. Base colour is the dirt film lifting off the paint, then the
;;! product cast laid over whatever that left. Roughness is the one doing most of
;;! the work: dust scatters (high), a rinse knocks that back, product hazes it up
;;! again, and the buff drives it down to the tight highlight that reads as a
;;! mirror. Metallic comes up with the shine because a polished candy coat is
;;! flake under clear, and subsurface — the pbr shader's wrap-diffuse-and-rim
;;! term — is what gives the finished panel depth instead of a painted-on
;;! highlight.
(define (carwash-paint-source)
  (let* ((clean (carwash-clean))
         (prod  (carwash-product))
         (gloss (carwash-gloss))
         (chan  (lambda (i)
                  (carwash-mix (carwash-mix (list-ref carwash-dirt-rgb i)
                                            (list-ref carwash-body-rgb i)
                                            clean)
                               (list-ref carwash-foam-rgb i)
                               (* prod 0.6)))))
    (string-append
     "(material carwash-paint\n"
     "  (shader \"builtin://shader/pbr\")\n"
     "  (base_color " (carwash-num (chan 0))
     " " (carwash-num (chan 1))
     " " (carwash-num (chan 2)) " 1.0)\n"
     "  (metallic " (carwash-num (carwash-clamp
                                  (+ 0.04 (* 0.34 gloss) (* -0.1 prod))
                                  0.0 1.0)) ")\n"
                                  "  (roughness " (carwash-num (carwash-clamp
                                                                (+ 0.88 (* -0.3 clean) (* 0.16 prod)
                                                                   (* -0.5 gloss))
                                                                0.06 0.95)) ")\n"
                                                                "  (subsurface " (carwash-num (* 0.35 gloss)) "))\n")))

;;! (carwash-repaint! dt) — repack the paint when the throttle comes due. This is
;;! the call that makes the whole show visible: material-define! on a path that
;;! already holds a material repacks it in place and keeps the id, so the panels
;;! bound to it at load time are drawn with the new bytes on the very next frame
;;! without anything rebinding. Returns 0 in the headless test, which boots no
;;! catalog and so has no pbr shader to pack against — the loop runs there
;;! regardless, it just paints nothing.
(define (carwash-repaint! dt)
  (set! *carwash-paint* (- *carwash-paint* dt))
  (when (<= *carwash-paint* 0.0)
    (set! *carwash-paint* carwash-paint-every)
    (material-define! carwash-paint-path (carwash-paint-source))))

;;! --- the tools -------------------------------------------------------------
;;!
;;! Three tools, each one entity per part, and none of them parented. That is not
;;! an oversight: world_propagate_transforms composes the hierarchy from the
;;! AUTHORED locals before scripts run, and a script writes the animated world
;;! pose of its own entity only — so a child does not follow a scripted parent.
;;! Every moving thing here therefore carries the script itself and places itself
;;! from the pose the rules compute, which is the read-rest / write-animated
;;! contract entity_script.h sets out, applied to a rig rather than one prop.
;;!
;;! A part is authored at its rack slot, so the scene at rest is the bay with its
;;! tools racked, and the offset the script rotates and carries is recovered by
;;! subtracting the slot back off. Tool space points +Z at the car.

;;! Where each tool parks between jobs, in world space — a row along the rack.
(define carwash-racks
  (list (list -1.75 1.52 -4.6)
        (list 0.05 1.52 -4.6)
        (list 1.85 1.52 -4.6)))

;;! The working end of each tool, in tool space: where the water leaves the
;;! wand, the face of the applicator, the underside of the buffer's disc. The
;;! spray is emitted from here, so a puff comes off the part of the tool that is
;;! touching the car rather than out of its middle.
(define carwash-tips
  (list (list 0.0 0.0 1.35)
        (list 0.0 0.0 0.55)
        (list 0.0 -0.2 0.5)))

;;! What each tool throws: rinse water, then a creamy product foam, then the warm
;;! dust a pad throws coming up to a shine. The count falls as the job gets finer.
(define carwash-spray-rgb
  (list (list 0.62 0.78 0.95)
        (list 0.95 0.93 0.86)
        (list 1.0 0.9 0.62)))

(define carwash-spray-count (list 11 8 5))

;;! (carwash-aim x z) -> the Y rotation in degrees that points a tool at the
;;! bay's centre from (X, Z). A yaw of θ carries tool-space +Z to (sin θ, 0,
;;! cos θ), so aiming at the origin from (x, z) is atan2 of the vector back to it.
(define (carwash-aim x z)
  (* carwash-deg (atan (- x) (- z))))

;;! (carwash-swirl k cx y r turns) -> a pose orbiting (CX, Z=0) at height Y and
;;! radius R, K of the way through TURNS laps, squashed along Z so the circle
;;! reads as the ellipse a buffer actually walks over a panel.
(define (carwash-swirl k cx y r turns)
  (let ((a (* k turns carwash-tau)))
    (list (+ cx (* r (cos a))) y (* r 0.62 (sin a)) 180.0)))

;;! (carwash-pose-wand u) — the rinse. The wand swings around the car's +Z flank
;;! from the nose to the tail, bobbing up and down six times on the way, always
;;! pointed inward. One long sweep rather than a scrub: this is the phase whose
;;! job is to establish the car, and the camera riding a steady arc around it is
;;! the shot that does that.
(define (carwash-pose-wand u)
  (let* ((a (carwash-mix 0.42 2.72 u))
         (r 3.45)
         (x (* r (cos a)))
         (z (* r (sin a)))
         (y (+ 1.45 (* 0.78 (sin (* u 6.0 carwash-tau))))))
    (list x y z (carwash-aim x z))))

;;! (carwash-pose-pad u) — the product. Three passes along the flank, alternating
;;! direction and stepping down a panel each time: the glass, then the upper
;;! door, then the lower. The pad's face is authored thin in Z, so held at yaw
;;! 180 it lies flat against the side of the car it is working.
(define (carwash-pose-pad u)
  (let* ((p (* u 3.0))
         (k (floor p))
         (s (- p k))
         (d (if (even? k) s (- 1.0 s))))
    (list (carwash-mix -2.25 2.25 d) (- 1.62 (* 0.44 k)) 1.52 180.0)))

;;! (carwash-pose-buffer u) — the buff. The buffer's head is a disc lying flat,
;;! so it works the horizontal panels: swirls over the hood, lifts over the roof,
;;! then swirls over the trunk. The lift is what keeps it out of the cabin, which
;;! a straight run down the middle of the car would pass through — and it is also
;;! the best few seconds in the loop, because the camera follows it up and over.
(define (carwash-pose-buffer u)
  (cond ((< u 0.44) (carwash-swirl (/ u 0.44) 1.62 1.34 0.52 5.0))
        ((> u 0.56) (carwash-swirl (/ (- u 0.56) 0.44) -1.92 1.34 0.46 4.0))
        (else
         (let ((k (carwash-smooth (/ (- u 0.44) 0.12))))
           (list (carwash-mix 1.62 -1.92 k)
                 (+ 1.34 (* 1.35 (sin (* k carwash-pi))))
                 0.0
                 180.0)))))

;;! (carwash-pose i) -> tool I's pose right now as (X Y Z YAW-DEGREES). A tool
;;! that is not the one working sits on its rack; the one that is eases out of
;;! the rack over the first half-second of its phase and back onto it over the
;;! last, so a tool change is a hand-off you can watch rather than two cuts.
(define carwash-tool-out 0.55)

(define (carwash-pose i)
  (let ((rack (list-ref carwash-racks i)))
    (if (not (= i (carwash-tool)))
        (list (car rack) (cadr rack) (caddr rack) 0.0)
        (let* ((u    (carwash-u))
               (work (case i
                       ((0) (carwash-pose-wand u))
                       ((1) (carwash-pose-pad u))
                       (else (carwash-pose-buffer u))))
               (len  (carwash-len))
               (k    (min (carwash-smooth (/ (* u len) carwash-tool-out))
                          (carwash-smooth (/ (* (- 1.0 u) len)
                                             carwash-tool-out)))))
          (list (carwash-mix (car rack) (car work) k)
                (carwash-mix (cadr rack) (cadr work) k)
                (carwash-mix (caddr rack) (caddr work) k)
                (* k (list-ref work 3)))))))

;;! (carwash-place x y z yaw ox oy oz) -> where a part authored at tool-space
;;! offset (OX OY OZ) stands when its tool is at (X Y Z) turned YAW degrees. The
;;! rotation is the plain Y one the yaw names: +Z swings to (sin θ, 0, cos θ) and
;;! +X to (cos θ, 0, -sin θ).
(define (carwash-place x y z yaw ox oy oz)
  (let* ((a (/ yaw carwash-deg))
         (c (cos a))
         (s (sin a)))
    (list (+ x (* c ox) (* s oz))
          (+ y oy)
          (+ z (- (* c oz) (* s ox))))))

;;! (carwash-spray! dt) — throw a puff off the working tool's tip when the
;;! cadence comes due, and nothing at all in the phases no tool is out for.
;;! Guarded on particle-burst! being bound the way chess guards its own puff: the
;;! renderer registers it, and the headless test has no renderer, so there the
;;! show simply runs dry.
(define (carwash-spray! dt)
  (let ((i (carwash-tool)))
    (if (< i 0)
        (set! *carwash-spray* 0.0)
        (begin
          (set! *carwash-spray* (- *carwash-spray* dt))
          (when (<= *carwash-spray* 0.0)
            (set! *carwash-spray* carwash-spray-every)
            (when (defined? 'particle-burst!)
              (let* ((p   (carwash-pose i))
                     (tip (list-ref carwash-tips i))
                     (at  (carwash-place (car p) (cadr p) (caddr p)
                                         (list-ref p 3)
                                         (car tip) (cadr tip) (caddr tip)))
                     (rgb (list-ref carwash-spray-rgb i)))
                (particle-burst! (car at) (cadr at) (caddr at)
                                 (car rgb) (cadr rgb) (caddr rgb)
                                 (list-ref carwash-spray-count i)))))))))

;;! --- the camera ------------------------------------------------------------
;;!
;;! Two shots and nothing else. While a tool is working the eye rides it: swing
;;! around to the tool's side of the car, close in, and rise with it. Between
;;! jobs the eye falls back to a slow orbit — wide while the car is coming and
;;! going, tighter for the reveal.
;;!
;;! What the follow shot cannot do is centre the tool, because the look-at target
;;! is a fixed (0, 0, 0) and only the eye is scriptable (scene_renderer.c). So it
;;! frames the car with the tool in the near half of the shot, which for a tool
;;! working a car is the composition you wanted anyway — and it is why the
;;! distance and the height are clamped rather than taken straight from the pose:
;;! within those bounds the framing holds for every tool the show has.
;;!
;;! The eye is carried as (BEARING RADIUS HEIGHT) about the bay's centre rather
;;! than as a point, and every move is eased in those three. That is not a
;;! flourish, it is the difference between a camera and a bug: eased as a point,
;;! a swing from one side of the car to the other is a straight line between two
;;! ends of an arc, and a straight line between two opposite sides of a car goes
;;! through the car. In polar the same move is the orbit it looks like, the
;;! radius never drops below the smaller of the two shots, and the eye cannot
;;! arrive anywhere the view degenerates.

(define carwash-cam-follow-k 2.35)
(define carwash-cam-lift 2.3)
(define carwash-cam-min-r 6.2)
(define carwash-cam-max-r 9.6)
(define carwash-cam-min-h 2.5)
(define carwash-cam-max-h 6.4)

;;! Below this distance from the bay's centre a tool is not standing in any
;;! direction worth watching from — the buffer crossing the roof passes right
;;! over it. The last bearing that was is held instead, so the eye keeps its side
;;! of the car through the crossing rather than chasing the noise across it.
(define carwash-cam-near 0.9)

(define carwash-cam-hero-r 12.4)
(define carwash-cam-hero-h 5.4)
(define carwash-cam-reveal-r 8.6)
(define carwash-cam-reveal-h 3.4)

;;! How fast the eye closes on where it wants to be; framerate-independent (see
;;! carwash-cam-ease!). Fast enough that a tool change lands inside a second,
;;! slow enough that every change is a move rather than a cut.
(define carwash-cam-rate 2.4)

;;! The live eye, in the polar terms it eases in: bearing about the bay's centre
;;! (radians, 0 along +X), distance from it, and height.
(define *carwash-cam-a* 0.0)
(define *carwash-cam-r* carwash-cam-hero-r)
(define *carwash-cam-y* carwash-cam-hero-h)

;;! The bearing the follow shot is watching from, held over the frames where the
;;! tool is too near the centre to name one.
(define *carwash-cam-aim* 0.0)

;;! (carwash-wrap a) -> A folded into (-pi, pi]. Applied to the difference
;;! between two bearings it is what makes a swing take the short way around
;;! instead of the long one back through where it came from.
(define (carwash-wrap a)
  (cond ((> a carwash-pi) (carwash-wrap (- a carwash-tau)))
        ((<= a (- carwash-pi)) (carwash-wrap (+ a carwash-tau)))
        (else a)))

;;! (carwash-cam-hero) -> the between-jobs shot: the slow orbit, pulled in and
;;! dropped for the reveal so the finished car fills the frame.
(define (carwash-cam-hero)
  (let ((close (eq? (carwash-now) 'reveal)))
    (list *carwash-hero*
          (if close carwash-cam-reveal-r carwash-cam-hero-r)
          (if close carwash-cam-reveal-h carwash-cam-hero-h))))

;;! (carwash-cam-work p) -> the shot that watches a tool at pose P: its bearing,
;;! backed off far enough to hold the car in frame, raised with it.
(define (carwash-cam-work p)
  (let* ((px (car p))
         (py (cadr p))
         (pz (caddr p))
         (r  (sqrt (+ (* px px) (* pz pz)))))
    (when (>= r carwash-cam-near)
      (set! *carwash-cam-aim* (atan pz px)))
    (list *carwash-cam-aim*
          (carwash-clamp (* r carwash-cam-follow-k)
                         carwash-cam-min-r carwash-cam-max-r)
          (carwash-clamp (* py carwash-cam-lift)
                         carwash-cam-min-h carwash-cam-max-h))))

(define (carwash-cam-desired)
  (let ((i (carwash-tool)))
    (if (< i 0)
        (carwash-cam-hero)
        (carwash-cam-work (carwash-pose i)))))

;;! (carwash-cam-ease! want dt) — close a fraction of the gap to WANT in each of
;;! the three, the fraction shrinking with a smaller DT so a frame hitch never
;;! overshoots: 1 - e^(-rate*dt), the standard framerate-independent exponential
;;! ease. The bearing is eased through carwash-wrap, and folded back into one
;;! turn afterwards so it cannot creep away over a long run.
(define (carwash-cam-ease! want dt)
  (let ((k (- 1.0 (exp (* (- carwash-cam-rate) dt)))))
    (set! *carwash-cam-a*
          (carwash-wrap (+ *carwash-cam-a*
                           (* k (carwash-wrap (- (car want)
                                                 *carwash-cam-a*))))))
    (set! *carwash-cam-r*
          (+ *carwash-cam-r* (* k (- (cadr want) *carwash-cam-r*))))
    (set! *carwash-cam-y*
          (+ *carwash-cam-y* (* k (- (caddr want) *carwash-cam-y*))))))

;;! --- what the scripts call -------------------------------------------------
;;!
;;! Three entry points, one per kind of scripted entity, each opening by handing
;;! its clock reading to carwash-clock! and each wrapped in a catch so one bad
;;! frame logs and passes instead of taking the engine's tick down with it —
;;! the same guard chess puts on its camera, for the same reason.

(define (carwash-camera-tick! self t)
  (catch #t
         (lambda ()
           (carwash-clock! t)
           (carwash-cam-ease! (carwash-cam-desired) *carwash-dt*)
           (entity-set-position! self
                                 (* *carwash-cam-r* (cos *carwash-cam-a*))
                                 *carwash-cam-y*
                                 (* *carwash-cam-r* (sin *carwash-cam-a*))))
         (lambda args (krudd-log 2 "carwash: camera fault") #f)))

;;! Every panel, lamp and wheel of the car carries this: read the authored rest
;;! pose, carry it down the lane by however far the car has come. The car is one
;;! rigid thing, so one offset moves all of it and no part needs to know which
;;! part it is.
(define (carwash-car-tick! self t)
  (catch #t
         (lambda ()
           (carwash-clock! t)
           (let ((b (entity-base-position self)))
             (when (pair? b)
               (entity-set-position! self (+ (car b) (carwash-car-x))
                                     (cadr b) (caddr b)))))
         (lambda args (krudd-log 2 "carwash: car fault") #f)))

;;! Every part of tool I carries this: recover the part's offset in tool space by
;;! subtracting the rack slot it was authored at, then stand it where the tool is
;;! now, turned to face what it is working.
(define (carwash-tool-tick! i self t)
  (catch #t
         (lambda ()
           (carwash-clock! t)
           (let ((b    (entity-base-position self))
                 (rack (list-ref carwash-racks i))
                 (p    (carwash-pose i)))
             (when (pair? b)
               (let ((at (carwash-place (car p) (cadr p) (caddr p)
                                        (list-ref p 3)
                                        (- (car b) (car rack))
                                        (- (cadr b) (cadr rack))
                                        (- (caddr b) (caddr rack)))))
                 (entity-set-position! self (car at) (cadr at) (caddr at))
                 (entity-set-euler! self 0 (list-ref p 3) 0)))))
         (lambda args (krudd-log 2 "carwash: tool fault") #f)))

;;! (carwash-reset) — the (on-load ...) hook: back to the top of the loop with a
;;! filthy car, the eye parked on the opening hero shot and no clock history, so
;;! opening the project twice shows the same first frame twice. Variadic because
;;! dispatch_scm always calls with one throwaway argument, while a fresh load and
;;! the tests call it with none.
(define (carwash-reset . ignored)
  (set! *carwash-prev-t* -1)
  (set! *carwash-dt* 0.0)
  (set! *carwash-hero* 0.0)
  (set! *carwash-spray* 0.0)
  (set! *carwash-cam-aim* 0.0)
  (carwash-enter! 0)
  (let ((eye (carwash-cam-hero)))
    (set! *carwash-cam-a* (car eye))
    (set! *carwash-cam-r* (cadr eye))
    (set! *carwash-cam-y* (caddr eye)))
  (carwash-repaint! 0.0)
  0)

;;! --- what a test can read --------------------------------------------------
;;!
;;! dispatch_scm hands one integer in and takes one integer out, so the show's
;;! state is exposed as integer polls the way chess exposes its turn. Percent and
;;! millimetre scalings are what let a float be read through that door without a
;;! second primitive; nothing in the show reads them.

(define (carwash-phase ignored) *carwash-phase*)

(define (carwash-active-tool ignored) (carwash-tool))

(define (carwash-clean-pct ignored) (round (* 100.0 (carwash-clean))))

(define (carwash-product-pct ignored) (round (* 100.0 (carwash-product))))

(define (carwash-gloss-pct ignored) (round (* 100.0 (carwash-gloss))))

(define (carwash-car-x-mm ignored) (round (* 1000.0 (carwash-car-x))))

;;! The live eye's height and its distance from the bay's centre, both in
;;! millimetres. Together they are what "the camera never ends up somewhere
;;! useless" means as an assertion: an eye directly over the target has no
;;! horizontal distance left and the view degenerates, so the follow shot's floor
;;! on that distance is the thing worth pinning.
(define (carwash-cam-h-mm ignored) (round (* 1000.0 *carwash-cam-y*)))

(define (carwash-cam-r-mm ignored) (round (* 1000.0 *carwash-cam-r*)))

;;! (carwash-run! ms) — drive the show forward MS milliseconds in frame-sized
;;! steps, as if the scripts had ticked. A headless test has no entity-script
;;! driver to run the clock for it, and stepping it by hand in C would put the
;;! frame size and the wrap arithmetic in two places; this keeps them here, next
;;! to the loop they belong to. Returns the phase it ends on.
(define (carwash-run! ms)
  (let ((step 0.016))
    (let loop ((left (/ ms 1000.0)))
      (when (> left 0.0)
        (carwash-clock! (+ (max *carwash-prev-t* 0.0) step))
        (loop (- left step)))))
  *carwash-phase*)

;;! --- the assets this show brings with it -----------------------------------
;;!
;;! No meshes: every shape in the bay is a builtin box, cylinder or plane, which
;;! is the point — what is authored here is surface and motion. The materials and
;;! the scripts are registered as this source is evaluated, which is before any
;;! scene is built, so the paths in the scene clause resolve when the launcher
;;! opens the show. A reload re-runs each definition and replaces it in place
;;! (both -define! calls keep the id), so a reloaded show keeps its bindings.

;;! The camera, the car and one per tool. The tool scripts differ only in the
;;! index they pass, so they are built from one template rather than written out
;;! three times — a script's NAME is a label and not a registry key (see
;;! entity_script.scm), but the three sources differ in their text, which is what
;;! the registry actually keys on.
(define carwash-camera-script
  (string-append "(script carwash-camera\n"
                 "  (on-tick (self t)\n"
                 "    (carwash-camera-tick! self t)))\n"))

(define carwash-car-script
  (string-append "(script carwash-car\n"
                 "  (on-tick (self t)\n"
                 "    (carwash-car-tick! self t)))\n"))

(define (carwash-tool-script i)
  (let ((n (number->string i)))
    (string-append "(script carwash-tool-" n "\n"
                   "  (on-tick (self t)\n"
                   "    (carwash-tool-tick! " n " self t)))\n")))

(define (carwash-tool-script-path i)
  (string-append "project://script/carwash-tool-" (number->string i)))

(script-define! "project://script/carwash-camera" carwash-camera-script)
(script-define! "project://script/carwash-car" carwash-car-script)
(script-define! (carwash-tool-script-path 0) (carwash-tool-script 0))
(script-define! (carwash-tool-script-path 1) (carwash-tool-script 1))
(script-define! (carwash-tool-script-path 2) (carwash-tool-script 2))

;;! Everything in the bay that is not the paint. All matte-to-middling
;;! dielectrics except the chrome, so the one surface that moves — the paint —
;;! is the only thing in frame doing anything, and the buffed reveal has
;;! something dull to be seen against.
(define carwash-glass-material
  (string-append "(material carwash-glass\n"
                 "  (shader \"builtin://shader/pbr\")\n"
                 "  (base_color 0.05 0.07 0.08 1.0)\n"
                 "  (metallic 0.0)\n"
                 "  (roughness 0.08)\n"
                 "  (subsurface 0.0))\n"))

(define carwash-chrome-material
  (string-append "(material carwash-chrome\n"
                 "  (shader \"builtin://shader/pbr\")\n"
                 "  (base_color 0.92 0.93 0.95 1.0)\n"
                 "  (metallic 1.0)\n"
                 "  (roughness 0.14)\n"
                 "  (subsurface 0.0))\n"))

(define carwash-rubber-material
  (string-append "(material carwash-rubber\n"
                 "  (shader \"builtin://shader/pbr\")\n"
                 "  (base_color 0.045 0.045 0.05 1.0)\n"
                 "  (metallic 0.0)\n"
                 "  (roughness 0.88)\n"
                 "  (subsurface 0.0))\n"))

(define carwash-floor-material
  (string-append "(material carwash-floor\n"
                 "  (shader \"builtin://shader/pbr\")\n"
                 "  (base_color 0.16 0.17 0.18 1.0)\n"
                 "  (metallic 0.0)\n"
                 "  (roughness 0.52)\n"
                 "  (subsurface 0.0))\n"))

(define carwash-bay-material
  (string-append "(material carwash-bay\n"
                 "  (shader \"builtin://shader/pbr\")\n"
                 "  (base_color 0.30 0.34 0.38 1.0)\n"
                 "  (metallic 0.1)\n"
                 "  (roughness 0.62)\n"
                 "  (subsurface 0.0))\n"))

(define carwash-tool-material
  (string-append "(material carwash-tool\n"
                 "  (shader \"builtin://shader/pbr\")\n"
                 "  (base_color 0.78 0.31 0.06 1.0)\n"
                 "  (metallic 0.0)\n"
                 "  (roughness 0.40)\n"
                 "  (subsurface 0.0))\n"))

(define carwash-foam-material
  (string-append "(material carwash-foam\n"
                 "  (shader \"builtin://shader/pbr\")\n"
                 "  (base_color 0.93 0.93 0.90 1.0)\n"
                 "  (metallic 0.0)\n"
                 "  (roughness 0.80)\n"
                 "  (subsurface 0.25))\n"))

(define carwash-pad-material
  (string-append "(material carwash-pad\n"
                 "  (shader \"builtin://shader/pbr\")\n"
                 "  (base_color 0.86 0.72 0.16 1.0)\n"
                 "  (metallic 0.0)\n"
                 "  (roughness 0.66)\n"
                 "  (subsurface 0.0))\n"))

(material-define! "project://material/carwash-glass" carwash-glass-material)
(material-define! "project://material/carwash-chrome" carwash-chrome-material)
(material-define! "project://material/carwash-rubber" carwash-rubber-material)
(material-define! "project://material/carwash-floor" carwash-floor-material)
(material-define! "project://material/carwash-bay" carwash-bay-material)
(material-define! "project://material/carwash-tool" carwash-tool-material)
(material-define! "project://material/carwash-foam" carwash-foam-material)
(material-define! "project://material/carwash-pad" carwash-pad-material)

;;! The paint is registered here too, at its filthy opening state, so the path
;;! holds bytes before the first entity binds it; every frame after this is the
;;! same call again with a different surface.
(material-define! carwash-paint-path (carwash-paint-source))

;;! --- the project -----------------------------------------------------------
;;!
;;! The form game/project's host reads. There is no (on-selected ...): nothing in
;;! an attract mode responds to a click, which is what makes it one. There is no
;;! (on-tick ...) either — the show is driven entirely from the entity scripts,
;;! ticked by the engine like any other, so the project's own per-frame hook has
;;! nothing left to do that the camera's tick has not already done.
;;!
;;! The bay: a wide floor with a lighter pad marking the wash bay, a gantry arch
;;! over it (thin enough to orbit through without blocking the car) and a rack at
;;! the back holding the three tools. The car is nine unparented entities all
;;! carrying carwash-car — a painted body, glass cabin, painted roof, two chrome
;;! lamps, and four cylinder wheels turned onto their sides. Every part of every
;;! tool is authored on its rack and carries that tool's script.
(project "Car Wash"
         (scene carwash
                (entity (name "Camera") (at 12.4 5.4 0)
                        (script "project://script/carwash-camera"))

                ;;! The bay itself: nothing here moves or is scripted.
                (entity (name "ground")
                        (mesh "builtin://mesh/plane")
                        (material "project://material/carwash-floor")
                        (at 0 0 0) (scale 60 1 60))
                (entity (name "bay-pad")
                        (mesh "builtin://mesh/plane")
                        (material "project://material/carwash-bay")
                        (at 0 0.012 -0.5) (scale 16 1 12))
                (entity (name "gantry-post-a")
                        (mesh "builtin://mesh/box")
                        (material "project://material/carwash-bay")
                        (at 0 2.1 3.0) (scale 0.36 4.2 0.36))
                (entity (name "gantry-post-b")
                        (mesh "builtin://mesh/box")
                        (material "project://material/carwash-bay")
                        (at 0 2.1 -3.0) (scale 0.36 4.2 0.36))
                (entity (name "gantry-beam")
                        (mesh "builtin://mesh/box")
                        (material "project://material/carwash-bay")
                        (at 0 4.4 0) (scale 0.5 0.4 6.4))
                (entity (name "rack-top")
                        (mesh "builtin://mesh/box")
                        (material "project://material/carwash-bay")
                        (at 0 1.25 -4.6) (scale 5.6 0.18 0.5))
                (entity (name "rack-leg-a")
                        (mesh "builtin://mesh/box")
                        (material "project://material/carwash-bay")
                        (at -2.5 0.62 -4.6) (scale 0.2 1.25 0.2))
                (entity (name "rack-leg-b")
                        (mesh "builtin://mesh/box")
                        (material "project://material/carwash-bay")
                        (at 2.5 0.62 -4.6) (scale 0.2 1.25 0.2))

                ;;! The car. Painted panels bind the one material the show
                ;;! rewrites; the glass, chrome and rubber stay put, so the
                ;;! change over a wash reads as the paint and not as a filter
                ;;! over the whole frame.
                (entity (name "car-body")
                        (mesh "builtin://mesh/box")
                        (material "project://material/carwash-paint")
                        (script "project://script/carwash-car")
                        (at 0 0.72 0) (scale 4.5 0.95 1.95))
                (entity (name "car-cabin")
                        (mesh "builtin://mesh/box")
                        (material "project://material/carwash-glass")
                        (script "project://script/carwash-car")
                        (at -0.25 1.5 0) (scale 2.3 0.7 1.74))
                (entity (name "car-roof")
                        (mesh "builtin://mesh/box")
                        (material "project://material/carwash-paint")
                        (script "project://script/carwash-car")
                        (at -0.25 1.9 0) (scale 2.12 0.13 1.8))
                (entity (name "car-lamp-l")
                        (mesh "builtin://mesh/box")
                        (material "project://material/carwash-chrome")
                        (script "project://script/carwash-car")
                        (at 2.22 0.92 0.62) (scale 0.14 0.22 0.42))
                (entity (name "car-lamp-r")
                        (mesh "builtin://mesh/box")
                        (material "project://material/carwash-chrome")
                        (script "project://script/carwash-car")
                        (at 2.22 0.92 -0.62) (scale 0.14 0.22 0.42))

                ;;! Wheels: the builtin cylinder is a disc about Y, so a quarter
                ;;! turn about Z lays it on its side into a tyre. Scaled thin
                ;;! along its own axis before that turn, which is the width.
                (entity (name "car-wheel-fl")
                        (mesh "builtin://mesh/cylinder")
                        (material "project://material/carwash-rubber")
                        (script "project://script/carwash-car")
                        (at 1.55 0.5 1.0) (rotate 0 0 90) (scale 1 0.42 1))
                (entity (name "car-wheel-fr")
                        (mesh "builtin://mesh/cylinder")
                        (material "project://material/carwash-rubber")
                        (script "project://script/carwash-car")
                        (at 1.55 0.5 -1.0) (rotate 0 0 90) (scale 1 0.42 1))
                (entity (name "car-wheel-rl")
                        (mesh "builtin://mesh/cylinder")
                        (material "project://material/carwash-rubber")
                        (script "project://script/carwash-car")
                        (at -1.55 0.5 1.0) (rotate 0 0 90) (scale 1 0.42 1))
                (entity (name "car-wheel-rr")
                        (mesh "builtin://mesh/cylinder")
                        (material "project://material/carwash-rubber")
                        (script "project://script/carwash-car")
                        (at -1.55 0.5 -1.0) (rotate 0 0 90) (scale 1 0.42 1))

                ;;! Tool 0, the pressure wand: a lance with a grip and a fan tip,
                ;;! authored on the left of the rack. A tool part is authored at
                ;;! its rack slot plus its offset in tool space, and the script
                ;;! subtracts the slot back off to recover the offset.
                (entity (name "tool-wand-lance")
                        (mesh "builtin://mesh/box")
                        (material "project://material/carwash-tool")
                        (script "project://script/carwash-tool-0")
                        (at -1.75 1.52 -4.05) (scale 0.11 0.11 1.2))
                (entity (name "tool-wand-grip")
                        (mesh "builtin://mesh/box")
                        (material "project://material/carwash-tool")
                        (script "project://script/carwash-tool-0")
                        (at -1.75 1.33 -4.66) (scale 0.16 0.34 0.2))
                (entity (name "tool-wand-tip")
                        (mesh "builtin://mesh/box")
                        (material "project://material/carwash-chrome")
                        (script "project://script/carwash-tool-0")
                        (at -1.75 1.52 -3.36) (scale 0.18 0.18 0.14))

                ;;! Tool 1, the applicator: a foam face on a stubby handle, thin
                ;;! along Z so it lies flat against the flank it works.
                (entity (name "tool-pad-face")
                        (mesh "builtin://mesh/box")
                        (material "project://material/carwash-foam")
                        (script "project://script/carwash-tool-1")
                        (at 0.05 1.52 -4.16) (scale 0.58 0.46 0.14))
                (entity (name "tool-pad-handle")
                        (mesh "builtin://mesh/box")
                        (material "project://material/carwash-tool")
                        (script "project://script/carwash-tool-1")
                        (at 0.05 1.65 -4.54) (scale 0.16 0.16 0.42))

                ;;! Tool 2, the buffer: a pad disc under a body and a grip. The
                ;;! disc is the builtin cylinder left about Y, so it lies flat
                ;;! over the horizontal panels it polishes.
                (entity (name "tool-buffer-head")
                        (mesh "builtin://mesh/cylinder")
                        (material "project://material/carwash-pad")
                        (script "project://script/carwash-tool-2")
                        (at 1.85 1.46 -4.1) (scale 0.78 0.14 0.78))
                (entity (name "tool-buffer-body")
                        (mesh "builtin://mesh/box")
                        (material "project://material/carwash-tool")
                        (script "project://script/carwash-tool-2")
                        (at 1.85 1.67 -4.5) (scale 0.28 0.26 0.5))
                (entity (name "tool-buffer-grip")
                        (mesh "builtin://mesh/box")
                        (material "project://material/carwash-tool")
                        (script "project://script/carwash-tool-2")
                        (at 1.85 1.67 -4.84) (scale 0.16 0.3 0.16)))
         (on-load carwash-reset))
