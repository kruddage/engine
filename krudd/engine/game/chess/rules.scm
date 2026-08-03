; SPDX-License-Identifier: GPL-2.0-or-later

;;! chess rules — the game's logic, in Scheme, living in the shared image, the
;;! way game/tictactoe/rules.scm drives tic-tac-toe. This is the first playable
;;! slice: click a piece of the side to move to pick it up, then click where it
;;! goes — an empty square to slide there, or an enemy piece to capture it — and
;;! the turn passes. It is deliberately FREE movement: any piece may go to any
;;! square (proving selection + transform + board-coordinate mapping). Per-piece
;;! legality, blocking, check, castling, en passant and promotion are a later
;;! slice layered on top of this same select→move plumbing.
;;!
;;! The chess plugin loads this image and, each frame, hands the id of the entity
;;! under a fresh click to chess-on-selected (via entity_api.dispatch_scm, which
;;! binds the live world so the scene-* primitives can read and move entities).
;;! Two clicks make a move: the first records the picked piece, the second acts.

;;! Board mapping mirrors scene.scm: files a..h run along +X at x = file - 3.5
;;! (a = 0 -> -3.5, h = 7 -> +3.5); ranks 1..8 along Z with white's first rank
;;! nearest the camera, z = 4.5 - rank (rank 1 -> +3.5, rank 8 -> -3.5). Pieces
;;! sit foot-on-board at y = 0.03. Squares are named "sq-<file><rank>" and pieces
;;! "<colour><type>-<square>": colour is #\w or #\b, type one of R N B Q K P, and
;;! the square suffix is only the piece's *starting* home — it is never read as a
;;! current position (a moved piece keeps its authored name), so names stay
;;! globally unique for the life of the set and identify colour+type by prefix.

;;! Whose turn it is: 1 = white (ivory), 2 = black (ebony). White moves first.
(define *chess-turn* 1)

;;! The picked-up piece's entity id while a move is mid-choice, or -1 when the
;;! board is waiting for a first click. Set by the first click, cleared once the
;;! second click resolves (a move, a capture, or a deselect).
(define *chess-sel* -1)

;;! (chess-reset) — a fresh game: white to move, nothing picked up. Variadic
;;! because dispatch_scm always calls with one throwaway argument, yet the tests
;;! and a fresh load may call it with none, exactly like ttt-reset.
(define (chess-reset . ignored)
  (set! *chess-turn* 1)
  (set! *chess-sel* -1)
  (scene-outline! -1)
  (chess-cam-reset!))

;;! (chess-turn ignored) -> *chess-turn*, a read hook the host (or a test) polls
;;! through dispatch_scm (which calls with one integer argument).
(define (chess-turn ignored) *chess-turn*)

;;! (chess-toggle-turn) passes the move to the other side.
(define (chess-toggle-turn)
  (set! *chess-turn* (if (= *chess-turn* 1) 2 1)))

;;! (chess-pick! id) picks a piece up: record it and light its selection outline
;;! (scene-outline!, honoured by the renderer's outline pass in-game). (chess-
;;! drop!) is the inverse — clear the pick and the outline — run on a move, a
;;! capture, or a deselect, so the highlight only ever rides the piece in hand.
(define (chess-pick! id)
  (set! *chess-sel* id)
  (scene-outline! id))

(define (chess-drop!)
  (set! *chess-sel* -1)
  (scene-outline! -1))

;;! --- name parsing -------------------------------------------------------

;;! (chess-type-char? c) -> #t when C is one of the six piece letters. Guards a
;;! piece test against the other 'b'-initial entity, "board-base": its second
;;! character is #\o, not a piece type.
(define (chess-type-char? c)
  (and (memv c '(#\R #\N #\B #\Q #\K #\P)) #t))

;;! (chess-piece? name) -> #t when NAME tags a chess piece: a colour, a type, and
;;! the '-' before its home square. Everything else the ray-pick might return —
;;! a "sq-…" tile, the ground, the board slab, the camera — is not a piece.
(define (chess-piece? name)
  (and (>= (string-length name) 3)
       (memv (string-ref name 0) '(#\w #\b))
       (chess-type-char? (string-ref name 1))
       (char=? (string-ref name 2) #\-)))

;;! (chess-piece-colour name) -> 1 for a white piece, 2 for black. Only valid on
;;! a name chess-piece? accepts.
(define (chess-piece-colour name)
  (if (char=? (string-ref name 0) #\w) 1 2))

;;! (chess-piece-yrot name) -> the Y rotation (degrees) a piece wears, so a move
;;! that rewrites its transform keeps its facing. Only white knights are turned
;;! (180, to face the enemy down -Z); every other piece keeps the mesh default,
;;! so a move must not silently spin them. Mirrors scene.scm's authored rotations.
(define (chess-piece-yrot name)
  (if (and (char=? (string-ref name 0) #\w)
           (char=? (string-ref name 1) #\N))
      180 0))

;;! (chess-square? name) -> #t when NAME is a "sq-<file><rank>" board tile.
(define (chess-square? name)
  (and (>= (string-length name) 5)
       (string=? (substring name 0 3) "sq-")))

;;! (chess-square-x name) / (chess-square-z name) -> the world centre of a
;;! "sq-<file><rank>" tile, the same coordinates scene.scm placed the tile at, so
;;! a piece dropped there lands dead-centre. File is the letter at index 3, rank
;;! the digit at index 4.
(define (chess-square-x name)
  (- (- (char->integer (string-ref name 3)) (char->integer #\a)) 3.5))
(define (chess-square-z name)
  (- 4.5 (string->number (substring name 4 5))))

;;! --- feedback -----------------------------------------------------------

;;! (chess-spark x z count) fires a small cosmetic particle puff of COUNT
;;! particles at world (x, z), a little above the board, in a warm ivory-dust
;;! colour — the "piece landed here" flourish. Guarded on particle-burst! being
;;! bound: the renderer registers it (scene_renderer's register_particle_script),
;;! but the headless rules test runs with no renderer, so there the primitive is
;;! absent and the puff is simply skipped — the rules never depend on it, exactly
;;! as tic-tac-toe's ttt-spark is guarded.
(define (chess-spark x z count)
  (when (defined? 'particle-burst!)
    (particle-burst! x 0.25 z 0.85 0.72 0.45 count)))

;;! --- moving -------------------------------------------------------------

;;! (chess-place! id x z) drops piece ID onto the board at world (x, z),
;;! foot-on-board (y = 0.03), preserving its facing. scene-xform! overwrites the
;;! whole local transform, so the piece's own Y rotation is re-supplied and its
;;! scale pinned to 1; a composite piece (the king and its cross children) rides
;;! along because the children are parented and keep their local transforms.
(define (chess-place! id x z)
  (scene-xform! id x 0.03 z 0 (chess-piece-yrot (scene-entity-name id)) 0
                1 1 1))

;;! (chess-move! selid x z) slides the picked piece to (x, z), ends the pick, and
;;! passes the turn. Returns 1 (a plain move happened).
(define (chess-move! selid x z)
  (chess-place! selid x z)
  (chess-spark x z 16)
  (chess-cam-mark-move! x z)
  (chess-drop!)
  (chess-toggle-turn)
  1)

;;! (chess-capture! selid targetid) takes the enemy piece TARGETID: read its
;;! square, sweep it off the board (by its unique name), then move the picked
;;! piece onto the vacated square and pass the turn. Returns 2 (a capture) so the
;;! host can, later, give a capture its own louder cue (#629). A target whose
;;! position cannot be read is left untouched and the pick stands (return 0).
(define (chess-capture! selid targetid)
  (let ((pos   (scene-entity-pos targetid))
        (tname (scene-entity-name targetid)))
    (if (pair? pos)
        (begin
          (scene-destroy-named! tname)
          (chess-place! selid (car pos) (caddr pos))
          (chess-spark (car pos) (caddr pos) 34)
          (chess-cam-mark-move! (car pos) (caddr pos))
          (chess-drop!)
          (chess-toggle-turn)
          2)
        0)))

;;! (chess-on-selected id) -> 0 when the click changed nothing, 1 on a move, 2 on
;;! a capture. The plugin calls this with the world bound; a fault is caught so a
;;! bad click never takes the frame down. First click of the side to move picks a
;;! piece up; the second resolves it. Clicking your own piece (any time) re-picks
;;! that piece instead of moving; clicking off the pieces-and-squares (the board
;;! slab, the ground) with a piece in hand cancels the pick.
(define (chess-on-selected id)
  (catch #t
         (lambda ()
           (let ((name (scene-entity-name id)))
             (cond
              ;;! First click: pick up a piece of the side to move, else ignore.
              ((< *chess-sel* 0)
               (if (and (chess-piece? name)
                        (= (chess-piece-colour name) *chess-turn*))
                   (begin (chess-pick! id) 0)
                   0))
              ;;! Second click on one of your own pieces: re-pick it, no move.
              ((and (chess-piece? name)
                    (= (chess-piece-colour name) *chess-turn*))
               (chess-pick! id) 0)
              ;;! Second click on an enemy piece: capture it.
              ((chess-piece? name)
               (chess-capture! *chess-sel* id))
              ;;! Second click on an empty square: slide there.
              ((chess-square? name)
               (chess-move! *chess-sel* (chess-square-x name)
                            (chess-square-z name)))
              ;;! Second click on anything else (board slab, ground): deselect.
              (else (chess-drop!) 0))))
         (lambda args (krudd-log 2 "chess: rule fault") 0)))

;;! --- camera ---------------------------------------------------------------
;;!
;;! The "Camera" entity (scene.scm) carries the chess-camera script this file
;;! registers at the bottom, a one-line dispatch to chess-camera-tick! below —
;;! the actual behaviour lives here, in the same file as the turn/selection
;;! state it reads, rather than in a shared built-in. Three zones: a 3/4 view of
;;! the side to move, a lean toward a picked-up piece, and a park on a just-
;;! landed square for a beat before easing on to the next player's view. The eye
;;! is the only thing that moves — scene_renderer's look-at target is a fixed
;;! (0, 0, 0), the board's centre (see scene.scm) — so "focus" is approximated
;;! by pushing the eye out along the ray from that centre through the square of
;;! interest, which lands the square near frame-centre without the engine
;;! needing a scriptable look-at target.

;;! The authored 3/4-view eye for each side, mirrored across the board's centre
;;! so black sees its own side the same way white always saw its own — this is
;;! the exact fixed spot scene.scm used to park the "Camera" entity at.
(define *chess-cam-white-eye* (list 5.5 8.5 10.5))
(define *chess-cam-black-eye* (list -5.5 8.5 -10.5))

;;! The live eye, eased a little toward chess-cam-desired every tick rather
;;! than jumping — this is what makes a turn change, a pick-up, and a move all
;;! read as camera MOVEMENT instead of a cut. Reset to white's eye by
;;! chess-cam-reset!.
(define *chess-cam-x* (car   *chess-cam-white-eye*))
(define *chess-cam-y* (cadr  *chess-cam-white-eye*))
(define *chess-cam-z* (caddr *chess-cam-white-eye*))

;;! The clock (entity-script's `t`, seconds since boot) as of the last tick, so
;;! chess-cam-ease! can measure dt and stay framerate-independent; -1 before
;;! the render loop has ever ticked this camera (also how chess-cam-mark-move!
;;! tells "a real clock is running" from "the headless rules test", which never
;;! ticks the camera and so must not be pushed into a hold it can't leave).
(define *chess-cam-prev-t* -1)

;;! >= 0 while the camera is holding on the last move's destination square: the
;;! clock time the hold ends. A plain comparison against `t` is enough to tell
;;! "still holding" from "lerp on to the next player".
(define *chess-cam-hold-until* -1)

;;! The last move or capture's destination, in world (x, z) — where the hold
;;! phase parks the camera. Set by chess-cam-mark-move!.
(define *chess-cam-move-x* 0)
(define *chess-cam-move-z* 0)

;;! How long (seconds) the camera parks on a just-moved square before easing on.
(define chess-cam-hold-secs 0.6)

;;! How fast the eye closes the gap to its desired spot; bigger closes faster.
;;! Framerate-independent (see chess-cam-ease!).
(define chess-cam-ease-rate 2.2)

;;! (chess-cam-reset!) — park the eye back at white's opening view with no hold
;;! and no clock history. Called by chess-reset.
(define (chess-cam-reset!)
  (set! *chess-cam-hold-until* -1)
  (set! *chess-cam-prev-t* -1)
  (set! *chess-cam-x* (car   *chess-cam-white-eye*))
  (set! *chess-cam-y* (cadr  *chess-cam-white-eye*))
  (set! *chess-cam-z* (caddr *chess-cam-white-eye*)))

;;! (chess-cam-mark-move! x z) — called once a piece has landed on (x, z):
;;! records the square and starts the hold phase. Guarded on the render clock
;;! having ticked at least once, so the headless rules test (which drives moves
;;! with no camera script ever ticking) leaves the hold permanently off instead
;;! of arming a timer nothing will ever advance past.
(define (chess-cam-mark-move! x z)
  (set! *chess-cam-move-x* x)
  (set! *chess-cam-move-z* z)
  (when (>= *chess-cam-prev-t* 0)
    (set! *chess-cam-hold-until* (+ *chess-cam-prev-t* chess-cam-hold-secs))))

;;! (chess-cam-square-eye x z) -> an (x y z) eye that frames square (x, z): out
;;! along the ray from the board's centre through the square, so the square
;;! lands near frame-centre despite the look-at target staying at the origin.
(define (chess-cam-square-eye x z)
  (list (* x 2.1) 2.4 (+ (* z 2.1) (if (>= z 0) 2.0 -2.0))))

;;! (chess-cam-blend a b k) -> the point K of the way from A to B (each an
;;! (x y z) list), K in [0, 1].
(define (chess-cam-blend a b k)
  (list (+ (car   a) (* k (- (car   b) (car   a))))
        (+ (cadr  a) (* k (- (cadr  b) (cadr  a))))
        (+ (caddr a) (* k (- (caddr b) (caddr a))))))

;;! (chess-cam-desired t) -> the (x y z) eye the camera is currently easing
;;! toward: the just-moved square while the hold lasts, a lean toward the
;;! picked-up piece while one is in hand, else the mover's own 3/4 zone.
(define (chess-cam-desired t)
  (let ((zone (if (= *chess-turn* 1) *chess-cam-white-eye* *chess-cam-black-eye*)))
    (cond
     ((and (>= *chess-cam-hold-until* 0) (< t *chess-cam-hold-until*))
      (chess-cam-square-eye *chess-cam-move-x* *chess-cam-move-z*))
     ((>= *chess-sel* 0)
      (let ((pos (entity-base-position *chess-sel*)))
        (if (pair? pos)
            (chess-cam-blend zone (chess-cam-square-eye (car pos) (caddr pos)) 0.55)
            zone)))
     (else
      (set! *chess-cam-hold-until* -1)
      zone))))

;;! (chess-cam-ease! desired dt) — nudge the live eye a fraction of the way
;;! from where it is to DESIRED, the fraction shrinking with a smaller DT so a
;;! frame hitch never overshoots: 1 - e^(-rate*dt), the standard framerate-
;;! independent exponential ease.
(define (chess-cam-ease! desired dt)
  (let ((k (- 1 (exp (* (- chess-cam-ease-rate) dt)))))
    (set! *chess-cam-x* (+ *chess-cam-x* (* k (- (car   desired) *chess-cam-x*))))
    (set! *chess-cam-y* (+ *chess-cam-y* (* k (- (cadr  desired) *chess-cam-y*))))
    (set! *chess-cam-z* (+ *chess-cam-z* (* k (- (caddr desired) *chess-cam-z*))))))

;;! (chess-camera-tick! self t) — the chess-camera script's on-tick:
;;! ease the live eye toward chess-cam-desired and push it onto the Camera
;;! entity SELF via entity-set-position!. A fault (a stray tick before
;;! chess-reset has ever run, say) is caught so a bad frame never takes the
;;! game down, mirroring chess-on-selected's own guard.
(define (chess-camera-tick! self t)
  (catch #t
         (lambda ()
           (let ((dt (if (< *chess-cam-prev-t* 0) 0 (- t *chess-cam-prev-t*))))
             (set! *chess-cam-prev-t* t)
             (chess-cam-ease! (chess-cam-desired t) dt)
             (entity-set-position! self *chess-cam-x* *chess-cam-y* *chess-cam-z*)))
         (lambda args (krudd-log 2 "chess: camera fault") #f)))

;;! (chess-cam-holding? ignored) -> 1 while the post-move hold is in effect, 0
;;! otherwise — a poll hook mirroring chess-turn, so a headless test can check
;;! the hold-arming guard without a real render clock (see chess_test.c).
(define (chess-cam-holding? ignored)
  (if (>= *chess-cam-hold-until* 0) 1 0))

;;! --- the camera script this game brings with it ----------------------------
;;!
;;! The catalog path scene.scm's "Camera" entity binds. It is project://, not
;;! builtin://, because the engine seeds nothing named chess: the entry appears
;;! in the catalog only because the form below put it there.
(define chess-camera-script-path "project://script/chess-camera")

;;! The (script ...) source itself — one on-tick clause dispatching to
;;! chess-camera-tick! above. Written a line per string the way the engine's own
;;! built-in script sources are (world/asset/include/asset/builtin_scripts.h),
;;! so the text reads as the Scheme it is.
(define chess-camera-script
  (string-append "(script chess-camera\n"
                 "  (on-tick (self t)\n"
                 "    (chess-camera-tick! self t)))\n"))

;;! Register it as a catalog asset when these rules load, which is before the
;;! scene is ever built — so by the time scene.scm's (script ...) clause resolves
;;! the path, the asset is there. A reload re-runs this and replaces the source
;;! in place (script-define! keeps the id), so the Camera entity stays bound.
;;! Returns 0 in the headless rules test, which boots no asset catalog; nothing
;;! there ticks a camera, so there is nothing to bind and nothing to report.
(script-define! chess-camera-script-path chess-camera-script)

;;! --- the piece meshes this game brings with it ------------------------------
;;!
;;! The six turned/blocky pieces scene.scm binds, showcasing the engine's lathe
;;! (mesh-lathe) the way its cylinder/cone/torus built-ins showcase mesh-revolve.
;;! Each is authored base-at-origin (its foot sits on y = 0, so a scene drops it
;;! straight onto a board square) and sized against a one-unit square: a body no
;;! wider than ~0.6 units, heights rising pawn < knight < rook < bishop < queen <
;;! king. Five are pure surfaces of revolution — a single (r y) silhouette swept
;;! around Y, mesh-lathe deriving the smooth normals — so the whole set reads as
;;! one turned-ivory family.
;;!
;;! These used to be seeded by the engine as builtin://mesh/chess-* (world/asset/
;;! asset_plugin.c). They are the game's geometry, not the engine's, so the
;;! sources live here and reach the catalog through mesh-define! — the same text,
;;! byte for byte, at project:// paths. Written a line per string the way the
;;! engine's own built-in mesh sources are (world/asset/include/asset/
;;! builtin_mesh_scripts.h), so the text reads as the Scheme it is.

;;! pawn — the simplest turning: a flared foot, a slim waist under a collar, and
;;! a round ball head. 15 silhouette points, 24 sectors: 375 verts / 2016
;;! indices.
(define chess-pawn-mesh
  (string-append "(mesh chess-pawn\n"
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
;;! crenellation notches (the engine's implicit-surface builtin://mesh/sdf-rook
;;! can, via CSG), so this revolved rook keeps the ring silhouette and leaves the
;;! battlements smooth. 15 points: 375 verts / 2016 indices.
(define chess-rook-mesh
  (string-append "(mesh chess-rook\n"
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
(define chess-bishop-mesh
  (string-append "(mesh chess-bishop\n"
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
;;! to a rim (a real crown's points are not revolvable, so the rim stays a smooth
;;! ring) and a small neck and ball finish it. 19 points: 475 verts / 2592
;;! indices.
(define chess-queen-mesh
  (string-append "(mesh chess-queen\n"
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
;;! knobbed finial. The king's cross is added in scene.scm as two small crossed
;;! boxes on top, the way tic-tac-toe builds its X — the lathe body cannot
;;! express it. 19 points: 475 verts / 2592 indices.
(define chess-king-mesh
  (string-append "(mesh chess-king\n"
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

;;! knight — the one piece that is NOT a surface of revolution, so it is a
;;! deliberately pragmatic, blocky approximation rather than a sculpted horse: a
;;! revolved pedestal (mesh-lathe, capped flat on top) fused with two tilted
;;! boxes — a head block and a shorter snout — forward-leaning about the X axis.
;;! The three sub-meshes are welded index-offset by chess-fuse; chess-box builds
;;! an axis-aligned box from the six quad faces and chess-tilt rotates and
;;! translates a sub-mesh into place. Faces the +Z ("forward") side; the scene
;;! turns one army 180 degrees so the knights face each other. 273 verts / 1224
;;! indices.
(define chess-knight-mesh
  (string-append "(mesh chess-knight\n"
                 "  (generate ()\n"
                 "    (define (chess-box hx hy hz)\n"
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
                 "    (define (chess-tilt blob ang tx ty tz)\n"
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
                 "    (define (chess-fuse a b)\n"
                 "      (let ((na (length (car a))))\n"
                 "        (cons (append (car a) (car b))\n"
                 "              (append (cdr a) (map (lambda (i) (+ i na)) (cdr b))))))\n"
                 "    (let ((base (mesh-lathe\n"
                 "                  (list (list 0.0 0.0) (list 0.28 0.0) (list 0.29 0.05)\n"
                 "                        (list 0.21 0.1) (list 0.16 0.2) (list 0.155 0.34)\n"
                 "                        (list 0.185 0.42) (list 0.15 0.47) (list 0.0 0.47))\n"
                 "                  24))\n"
                 "          (head (chess-tilt (chess-box 0.12 0.23 0.28) -0.42 0.0 0.6 0.02))\n"
                 "          (snout (chess-tilt (chess-box 0.1 0.09 0.16) -0.15 0.0 0.55 0.24)))\n"
                 "      (chess-fuse (chess-fuse base head) snout))))\n"))

;;! Register the six when these rules load, which is before the scene is ever
;;! built — so by the time scene.scm's (mesh ...) clauses resolve their paths,
;;! the assets are there. A reload re-runs this and replaces each source in place
;;! (mesh-define! keeps the id), so a piece already bound keeps its geometry
;;! rather than losing it to a stale id. Returns 0 apiece in the headless rules
;;! test, which boots no asset catalog; nothing there draws, so nothing binds.
(mesh-define! "project://mesh/chess-pawn" chess-pawn-mesh)
(mesh-define! "project://mesh/chess-rook" chess-rook-mesh)
(mesh-define! "project://mesh/chess-bishop" chess-bishop-mesh)
(mesh-define! "project://mesh/chess-queen" chess-queen-mesh)
(mesh-define! "project://mesh/chess-king" chess-king-mesh)
(mesh-define! "project://mesh/chess-knight" chess-knight-mesh)
