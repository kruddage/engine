; SPDX-License-Identifier: GPL-2.0-or-later

;;! chess rules — the game's logic, in Scheme, living in the shared image, the
;;! way games/tictactoe/rules.scm drives tic-tac-toe. This is the first playable
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
;;! The "Camera" entity (scene.scm) carries the "chess-camera" builtin script
;;! (builtin_scripts.h), a one-line dispatch to chess-camera-tick! below — the
;;! actual behaviour lives here, in the same file as the turn/selection state it
;;! reads, rather than baked into a shared built-in. Three zones: a 3/4 view of
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

;;! (chess-camera-tick! self t) — the "chess-camera" builtin script's on-tick:
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
