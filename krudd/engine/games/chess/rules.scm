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
  (scene-outline! -1))

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
