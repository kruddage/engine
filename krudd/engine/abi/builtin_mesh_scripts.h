/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BUILTIN_MESH_SCRIPTS_H
#define BUILTIN_MESH_SCRIPTS_H

/*
 * Built-in meshes, seeded as read-only ASSET_TYPE_MESH assets (see
 * asset_plugin.c). Each is a single (mesh NAME (generate () ...)) form
 * in the same S7 Scheme the shader and entity-script DSLs use — the runtime
 * image's (mesh ...) macro registers it and asset/mesh_script.c marshals its
 * result into a mesh_blob on demand (see core/mesh_script.scm). There is no
 * hardcoded C mesh generator anymore: every built-in shape, including these
 * five, is authored the same way an editor-created mesh script is.
 *
 * Kept here as one shared source of truth so the asset seeder and the native
 * mesh_script oracle test compile the exact same script text.
 *
 * Beyond the original five, two families lean on the shared shape "engines" in
 * core/mesh_script.scm rather than hand-listing vertices:
 *
 *   - cylinder/cone/capsule/disc/torus are each a meridian PROFILE swept around
 *     the Y axis by mesh-revolve — a lathe. A profile point is (r y nr ny v);
 *     mesh-arc-profile emits the circular runs (hemisphere caps, torus tube).
 *   - superquadric/heightfield are each a (u,v)->(x y z) position function
 *     sampled over a grid by mesh-param-surface, which derives normals from the
 *     surface tangents. Both carry a (params ...) clause, so one source draws a
 *     family of shapes (box<->sphere<->star; calm<->jagged terrain).
 */

/*
 * box — 6 quad faces via mesh-quad-verts/-indices (core/mesh_script.scm),
 * 24 verts / 36 indices, parameterized by full width/height/depth. Its
 * generate body reads (param 'width) etc. and scales the unit-cube positions,
 * so two entities sharing this one mesh asset draw at different sizes purely
 * from their per-entity param overrides. The first built-in to carry a
 * (params ...) clause — the geometry twin of a material's shader uniforms.
 * Defaults to the unit cube (1x1x1).
 */
#define BOX_MESH_SCRIPT_SRC \
	"(mesh box\n" \
	"  (params\n" \
	"    (width  float (edit range 0.1 3.0) (default 1.0))\n" \
	"    (height float (edit range 0.1 3.0) (default 1.0))\n" \
	"    (depth  float (edit range 0.1 3.0) (default 1.0)))\n" \
	"  (generate ()\n" \
	"    (let* ((w (or (param 'width) 1.0))\n" \
	"           (h (or (param 'height) 1.0))\n" \
	"           (d (or (param 'depth) 1.0))\n" \
	"           (faces (list\n" \
	"                    (list (list 1.0 0.0 0.0) (list 0.0 0.0 1.0) (list 0.0 1.0 0.0))\n" \
	"                    (list (list -1.0 0.0 0.0) (list 0.0 0.0 1.0) (list 0.0 1.0 0.0))\n" \
	"                    (list (list 0.0 1.0 0.0) (list 1.0 0.0 0.0) (list 0.0 0.0 1.0))\n" \
	"                    (list (list 0.0 -1.0 0.0) (list 1.0 0.0 0.0) (list 0.0 0.0 1.0))\n" \
	"                    (list (list 0.0 0.0 1.0) (list 1.0 0.0 0.0) (list 0.0 1.0 0.0))\n" \
	"                    (list (list 0.0 0.0 -1.0) (list 1.0 0.0 0.0) (list 0.0 1.0 0.0)))))\n" \
	"      (let loop ((f 0) (verts '()) (indices '()))\n" \
	"        (if (= f 6)\n" \
	"            (cons (map (lambda (vx)\n" \
	"                         (list (* (list-ref vx 0) w) (* (list-ref vx 1) h) (* (list-ref vx 2) d)\n" \
	"                               (list-ref vx 3) (list-ref vx 4) (list-ref vx 5)\n" \
	"                               (list-ref vx 6) (list-ref vx 7)))\n" \
	"                       verts)\n" \
	"                  indices)\n" \
	"            (let ((face (list-ref faces f)))\n" \
	"              (loop (+ f 1)\n" \
	"                    (append verts (mesh-quad-verts (car face) (cadr face) (caddr face) 0.5))\n" \
	"                    (append indices (mesh-quad-indices (* f 4))))))))))\n"

/*
 * sphere — a UV sphere via mesh-sphere-verts/-indices (16 rings, 32
 * sectors), 561 verts / 2880 indices. Radius 0.5, centred on the origin.
 */
#define SPHERE_MESH_SCRIPT_SRC \
	"(mesh sphere\n" \
	"  (generate ()\n" \
	"    (cons (mesh-sphere-verts 16 32) (mesh-sphere-indices 16 32))))\n"

/*
 * plane — a single quad on the XZ plane facing +Y, 4 verts / 6 indices.
 */
#define PLANE_MESH_SCRIPT_SRC \
	"(mesh plane\n" \
	"  (generate ()\n" \
	"    (cons (mesh-quad-verts (list 0.0 1.0 0.0) (list 1.0 0.0 0.0) (list 0.0 0.0 1.0) 0.0)\n" \
	"          (mesh-quad-indices 0))))\n"

/*
 * pyramid — a square base (mesh-quad-verts/-indices) plus 4 triangular
 * sides fanned to the apex, 16 verts / 18 indices. Base sits at y = -0.5,
 * apex at y = 0.5.
 */
#define PYRAMID_MESH_SCRIPT_SRC \
	"(mesh pyramid\n" \
	"  (generate ()\n" \
	"    (let* ((base-verts (mesh-quad-verts (list 0.0 -1.0 0.0) (list 1.0 0.0 0.0) (list 0.0 0.0 1.0) 0.5))\n" \
	"           (base-idx (mesh-quad-indices 0))\n" \
	"           (b (list (list -0.5 -0.5 -0.5) (list 0.5 -0.5 -0.5)\n" \
	"                    (list 0.5 -0.5 0.5) (list -0.5 -0.5 0.5)))\n" \
	"           (apex (list 0.0 0.5 0.0)))\n" \
	"      (let loop ((s 0) (verts base-verts) (indices base-idx))\n" \
	"        (if (= s 4)\n" \
	"            (cons verts indices)\n" \
	"            (let* ((p0 (list-ref b s))\n" \
	"                   (p1 (list-ref b (modulo (+ s 1) 4)))\n" \
	"                   (vb (+ 4 (* s 3)))\n" \
	"                   (e0 (mesh-sub p1 p0))\n" \
	"                   (e1 (mesh-sub apex p0))\n" \
	"                   (n (mesh-normalize (mesh-cross e0 e1))))\n" \
	"              (loop (+ s 1)\n" \
	"                    (append verts\n" \
	"                            (list (list (car p0) (cadr p0) (caddr p0) (car n) (cadr n) (caddr n) 0.0 0.0)\n" \
	"                                  (list (car p1) (cadr p1) (caddr p1) (car n) (cadr n) (caddr n) 1.0 0.0)\n" \
	"                                  (list (car apex) (cadr apex) (caddr apex) (car n) (cadr n) (caddr n) 0.5 1.0)))\n" \
	"                    (append indices (list vb (+ vb 1) (+ vb 2))))))))))\n"

/*
 * grid — a subdivided unit plane on the XZ plane (4x4 quads, facing +Y),
 * built from a nested loop rather than hand-listed vertices. This is the
 * shape a fixed C primitive generator can't parameterize cleanly (the plane
 * built-in is a single quad); a mesh script can, and the subdivision count
 * is one number an author can change without touching engine code.
 */
#define GRID_MESH_SCRIPT_SRC                                                 \
	"(mesh grid\n"                                                        \
	"  (generate ()\n"                                                    \
	"    (let* ((n 4)\n"                                                  \
	"           (verts\n"                                                 \
	"             (let rloop ((r 0) (acc '()))\n"                        \
	"               (if (> r n)\n"                                       \
	"                   (reverse acc)\n"                                 \
	"                   (rloop (+ r 1)\n"                                \
	"                          (let cloop ((c 0) (a acc))\n"             \
	"                            (if (> c n)\n"                          \
	"                                a\n"                                \
	"                                (let* ((u (/ (exact->inexact c) n))\n" \
	"                                       (v (/ (exact->inexact r) n)))\n" \
	"                                  (cloop (+ c 1)\n"                 \
	"                                         (cons (list (- u 0.5) 0.0 (- v 0.5)\n" \
	"                                                     0.0 1.0 0.0 u v)\n" \
	"                                               a)))))))))\n"        \
	"           (indices\n"                                              \
	"             (let rloop ((r 0) (acc '()))\n"                        \
	"               (if (= r n)\n"                                       \
	"                   acc\n"                                           \
	"                   (rloop (+ r 1)\n"                                \
	"                          (let cloop ((c 0) (a acc))\n"             \
	"                            (if (= c n)\n"                          \
	"                                a\n"                                \
	"                                (let* ((k1 (+ (* r (+ n 1)) c))\n"  \
	"                                       (k2 (+ k1 n 1)))\n"          \
	"                                  (cloop (+ c 1)\n"                 \
	"                                         (append a\n"               \
	"                                                 (list k1 (+ k1 1) (+ k2 1)\n" \
	"                                                       k1 (+ k2 1) k2)))))))))))\n" \
	"      (cons verts indices))))\n"

/*
 * cylinder — a six-point meridian profile swept around Y by mesh-revolve
 * (core/mesh_script.scm), 24 sectors: bottom cap (down normals), the side wall
 * (radial normals), top cap (up normals), with the rim doubled at each cap so
 * the cap/side crease stays hard. Radius 0.5, height 1 (y in ±0.5). 150 verts /
 * 432 indices; the two zero-height crease rings emit no triangles.
 */
#define CYLINDER_MESH_SCRIPT_SRC \
	"(mesh cylinder\n" \
	"  (generate ()\n" \
	"    (mesh-revolve\n" \
	"      (list (list 0.0 -0.5 0.0 -1.0 0.0)\n" \
	"            (list 0.5 -0.5 0.0 -1.0 0.0)\n" \
	"            (list 0.5 -0.5 1.0 0.0 0.0)\n" \
	"            (list 0.5 0.5 1.0 0.0 1.0)\n" \
	"            (list 0.5 0.5 0.0 1.0 1.0)\n" \
	"            (list 0.0 0.5 0.0 1.0 1.0))\n" \
	"      24)))\n"

/*
 * cone — the cylinder's profile with the top rim collapsed to an apex at r=0.
 * The side normal is the unit (2,1)/sqrt(5) tilt of a 45-degree-ish slope,
 * shared by the base rim and the apex. Base radius 0.5, height 1. 100 verts /
 * 288 indices.
 */
#define CONE_MESH_SCRIPT_SRC \
	"(mesh cone\n" \
	"  (generate ()\n" \
	"    (let* ((nl (sqrt 5.0)) (nr (/ 2.0 nl)) (ny (/ 1.0 nl)))\n" \
	"      (mesh-revolve\n" \
	"        (list (list 0.0 -0.5 0.0 -1.0 0.0)\n" \
	"              (list 0.5 -0.5 0.0 -1.0 0.0)\n" \
	"              (list 0.5 -0.5 nr ny 0.0)\n" \
	"              (list 0.0 0.5 nr ny 1.0))\n" \
	"        24))))\n"

/*
 * disc — the simplest lathe: a two-point profile (centre, rim) swept into a
 * flat circle on the XZ plane facing +Y. Radius 0.5. 50 verts / 144 indices.
 */
#define DISC_MESH_SCRIPT_SRC \
	"(mesh disc\n" \
	"  (generate ()\n" \
	"    (mesh-revolve\n" \
	"      (list (list 0.0 0.0 0.0 1.0 0.0)\n" \
	"            (list 0.5 0.0 0.0 1.0 1.0))\n" \
	"      24)))\n"

/*
 * capsule — two hemisphere caps (mesh-arc-profile quarter turns) joined by a
 * cylindrical wall; the caps' equator normals are already radial, so the whole
 * profile is smooth (no crease). Radius 0.5, cylinder length 1, total height 2
 * (y in ±1). 450 verts / 2448 indices.
 */
#define CAPSULE_MESH_SCRIPT_SRC \
	"(mesh capsule\n" \
	"  (generate ()\n" \
	"    (mesh-revolve\n" \
	"      (append (mesh-arc-profile 0.0 -0.5 0.5 (* -0.5 pi) 0.0 8 0.0 0.4)\n" \
	"              (mesh-arc-profile 0.0 0.5 0.5 0.0 (* 0.5 pi) 8 0.6 1.0))\n" \
	"      24)))\n"

/*
 * torus — a full-circle tube profile (mesh-arc-profile, 0..2pi) offset from the
 * axis and swept around it. Major radius 0.35, minor 0.15 (outer radius 0.5,
 * inner 0.2). No poles, so the cleanest lathe of all. 425 verts / 2304 indices.
 */
#define TORUS_MESH_SCRIPT_SRC \
	"(mesh torus\n" \
	"  (generate ()\n" \
	"    (mesh-revolve\n" \
	"      (mesh-arc-profile 0.35 0.0 0.15 0.0 (* 2.0 pi) 16 0.0 1.0)\n" \
	"      24)))\n"

/*
 * superquadric — the parametric showcase: a superellipsoid sampled over a
 * 32x24 (u,v) grid by mesh-param-surface, which takes normals from the surface
 * tangents. The two exponents morph one source across a family — e1=e2=1 is the
 * radius-0.5 ellipsoid, e1=e2->0 a box, larger values pinch toward a star. sp
 * is the signed power |b|^e that keeps the surface real for negative b. 825
 * verts / 4608 indices.
 */
#define SUPERQUADRIC_MESH_SCRIPT_SRC \
	"(mesh superquadric\n" \
	"  (params\n" \
	"    (e1 float (edit range 0.1 3.0) (default 1.0))\n" \
	"    (e2 float (edit range 0.1 3.0) (default 1.0)))\n" \
	"  (generate ()\n" \
	"    (let ((e1 (or (param 'e1) 1.0))\n" \
	"          (e2 (or (param 'e2) 1.0)))\n" \
	"      (define (sp b e) (* (if (< b 0.0) -1.0 1.0) (expt (abs b) e)))\n" \
	"      (mesh-param-surface\n" \
	"        (lambda (u v)\n" \
	"          (let* ((lon (* (- (* 2.0 u) 1.0) pi))\n" \
	"                 (lat (* (- v 0.5) pi))\n" \
	"                 (cl (cos lat)) (sl (sin lat))\n" \
	"                 (co (cos lon)) (so (sin lon)))\n" \
	"            (list (* 0.5 (sp cl e1) (sp co e2))\n" \
	"                  (* 0.5 (sp sl e1))\n" \
	"                  (* 0.5 (sp cl e1) (sp so e2)))))\n" \
	"        32 24))))\n"

/*
 * heightfield — a unit patch on XZ (x,z in ±0.5) displaced in Y by a two-octave
 * sum of sines, sampled over a 24x24 grid by mesh-param-surface so its normals
 * follow the terrain. amp scales the relief, freq its wavelength — a calm swell
 * or choppy ground from one source. 625 verts / 3456 indices.
 */
#define HEIGHTFIELD_MESH_SCRIPT_SRC \
	"(mesh heightfield\n" \
	"  (params\n" \
	"    (amp  float (edit range 0.0 0.5) (default 0.15))\n" \
	"    (freq float (edit range 0.25 4.0) (default 1.0)))\n" \
	"  (generate ()\n" \
	"    (let ((amp (or (param 'amp) 0.15))\n" \
	"          (freq (or (param 'freq) 1.0)))\n" \
	"      (mesh-param-surface\n" \
	"        (lambda (u v)\n" \
	"          (let* ((x (- u 0.5)) (z (- v 0.5))\n" \
	"                 (fx (* freq 6.2831853 x)) (fz (* freq 6.2831853 z))\n" \
	"                 (h (* amp (+ (* 0.6 (sin fx) (cos fz))\n" \
	"                              (* 0.4 (sin (+ (* 2.0 fx) 1.3))\n" \
	"                                     (cos (+ (* 2.1 fz) 0.7)))))))\n" \
	"            (list x h z)))\n" \
	"        24 24))))\n"

/*
 * sdf-rook — the implicit-surface showcase: a chess rook built as a signed
 * distance field and polygonised by marching cubes (mesh-sdf, see
 * core/mesh_script.scm), the third shape engine after the lathe and the
 * parametric grid. Where those emit an explicit surface, this one composes a
 * *field* and lets the marcher recover the mesh — so the whole piece is
 * constructive solid geometry: a flared foot, a decorative collar ring, a
 * shaft, and a battlement rim are fused with sdf-smooth-union (their seams
 * become rounded fillets no lathe profile gives for free), then the
 * crenellations and the central bore are sdf-subtracted as a crossed pair of
 * boxes and a cylinder. Normals come from the field's own gradient, so the
 * surface shades smoothly across every CSG seam. Marched on a 40^3 grid over a
 * box a little larger than the piece; 5962 verts / 11920 triangles, one closed
 * genus-0 solid. The heaviest built-in to bake, but a param-less pure field, so
 * it is generated once and cached like any other.
 */
#define SDF_ROOK_MESH_SCRIPT_SRC \
	"(mesh sdf-rook\n" \
	"  (generate ()\n" \
	"    (let* ((foot    (sdf-translate (sdf-cylinder 0.34 0.10) 0.0 -0.40 0.0))\n" \
	"           (collar  (sdf-translate (sdf-torus 0.20 0.07)    0.0 -0.24 0.0))\n" \
	"           (shaft   (sdf-translate (sdf-cylinder 0.17 0.34) 0.0 -0.02 0.0))\n" \
	"           (rim     (sdf-translate (sdf-cylinder 0.29 0.14) 0.0  0.30 0.0))\n" \
	"           (solid   (sdf-smooth-union\n" \
	"                      (sdf-smooth-union\n" \
	"                        (sdf-smooth-union foot shaft 0.08)\n" \
	"                        collar 0.04)\n" \
	"                      rim 0.05))\n" \
	"           (bore    (sdf-translate (sdf-cylinder 0.13 0.10)  0.0 0.40 0.0))\n" \
	"           (notch-x (sdf-translate (sdf-box 0.40 0.10 0.075) 0.0 0.44 0.0))\n" \
	"           (notch-z (sdf-translate (sdf-box 0.075 0.10 0.40) 0.0 0.44 0.0))\n" \
	"           (piece   (sdf-subtract solid (sdf-union bore notch-x notch-z))))\n" \
	"      (mesh-sdf piece 40 (list -0.42 -0.56 -0.42) (list 0.42 0.56 0.42)))))\n"

/*
 * The chess set — six turned/blocky pieces that showcase the lathe (mesh-lathe,
 * core/mesh_script.scm) the way cylinder/cone/torus showcase mesh-revolve. Each
 * piece is authored base-at-origin (its foot sits on y = 0, so a scene drops it
 * straight onto a board square) and sized against a one-unit square: a body no
 * wider than ~0.6 units, heights rising pawn < knight < rook < bishop < queen <
 * king. Five are pure surfaces of revolution — a single (r y) silhouette swept
 * around Y, mesh-lathe deriving the smooth normals — so the whole set reads as
 * one turned-ivory family. games/chess binds these through builtin://mesh/chess-*.
 */

/*
 * pawn — the simplest turning: a flared foot, a slim waist under a collar, and a
 * round ball head. 15 silhouette points, 24 sectors: 375 verts / 2016 indices.
 */
#define CHESS_PAWN_MESH_SCRIPT_SRC \
	"(mesh chess-pawn\n" \
	"  (generate ()\n" \
	"    (mesh-lathe\n" \
	"      (list\n" \
	"        (list 0 0) (list 0.26 0) (list 0.27 0.05)\n" \
	"        (list 0.2 0.08) (list 0.13 0.16) (list 0.115 0.28)\n" \
	"        (list 0.175 0.34) (list 0.2 0.375) (list 0.11 0.4)\n" \
	"        (list 0.105 0.45) (list 0.165 0.52) (list 0.185 0.6)\n" \
	"        (list 0.155 0.7) (list 0.085 0.775) (list 0 0.8)\n" \
	"      )\n" \
	"      24)))\n"

/*
 * rook — a castle turret: a heavy foot, a straight shaft, and a crown that flares
 * to a raised rim over a recessed top. A lathe cannot cut the crenellation
 * notches (the engine's implicit-surface builtin://mesh/sdf-rook can, via CSG —
 * see SDF_ROOK_MESH_SCRIPT_SRC), so this revolved rook keeps the ring silhouette
 * and leaves the battlements smooth. 15 points: 375 verts / 2016 indices.
 */
#define CHESS_ROOK_MESH_SCRIPT_SRC \
	"(mesh chess-rook\n" \
	"  (generate ()\n" \
	"    (mesh-lathe\n" \
	"      (list\n" \
	"        (list 0 0) (list 0.29 0) (list 0.3 0.05)\n" \
	"        (list 0.22 0.1) (list 0.175 0.18) (list 0.165 0.4)\n" \
	"        (list 0.205 0.46) (list 0.15 0.52) (list 0.165 0.6)\n" \
	"        (list 0.255 0.66) (list 0.27 0.74) (list 0.27 0.82)\n" \
	"        (list 0.2 0.82) (list 0.19 0.76) (list 0 0.76)\n" \
	"      )\n" \
	"      24)))\n"

/*
 * bishop — a tall slim body under a bulbous mitre tapering to a finial ball. The
 * mitre's diagonal slit is not a surface of revolution and is omitted. 17 points:
 * 425 verts / 2304 indices.
 */
#define CHESS_BISHOP_MESH_SCRIPT_SRC \
	"(mesh chess-bishop\n" \
	"  (generate ()\n" \
	"    (mesh-lathe\n" \
	"      (list\n" \
	"        (list 0 0) (list 0.25 0) (list 0.26 0.05)\n" \
	"        (list 0.185 0.1) (list 0.12 0.2) (list 0.11 0.4)\n" \
	"        (list 0.175 0.46) (list 0.115 0.52) (list 0.1 0.6)\n" \
	"        (list 0.155 0.72) (list 0.185 0.82) (list 0.16 0.92)\n" \
	"        (list 0.1 1) (list 0.075 1.04) (list 0.09 1.09)\n" \
	"        (list 0.055 1.14) (list 0 1.17)\n" \
	"      )\n" \
	"      24)))\n"

/*
 * queen — the bishop's body grown taller with a coronet: the head flares wide to
 * a rim (a real crown's points are not revolvable, so the rim stays a smooth
 * ring) and a small neck and ball finish it. 19 points: 475 verts / 2592 indices.
 */
#define CHESS_QUEEN_MESH_SCRIPT_SRC \
	"(mesh chess-queen\n" \
	"  (generate ()\n" \
	"    (mesh-lathe\n" \
	"      (list\n" \
	"        (list 0 0) (list 0.29 0) (list 0.3 0.05)\n" \
	"        (list 0.21 0.11) (list 0.14 0.22) (list 0.125 0.48)\n" \
	"        (list 0.195 0.55) (list 0.12 0.61) (list 0.11 0.7)\n" \
	"        (list 0.16 0.8) (list 0.225 0.92) (list 0.265 1.02)\n" \
	"        (list 0.27 1.08) (list 0.21 1.1) (list 0.17 1.06)\n" \
	"        (list 0.115 1.14) (list 0.155 1.24) (list 0.085 1.3)\n" \
	"        (list 0 1.34)\n" \
	"      )\n" \
	"      24)))\n"

/*
 * king — the tallest piece: the queen's proportions with a taller crown and a
 * knobbed finial. The king's cross is added in the scene as two small crossed
 * boxes on top (games/chess/scene.scm), the way tic-tac-toe builds its X — the
 * lathe body cannot express it. 19 points: 475 verts / 2592 indices.
 */
#define CHESS_KING_MESH_SCRIPT_SRC \
	"(mesh chess-king\n" \
	"  (generate ()\n" \
	"    (mesh-lathe\n" \
	"      (list\n" \
	"        (list 0 0) (list 0.3 0) (list 0.31 0.05)\n" \
	"        (list 0.22 0.11) (list 0.15 0.24) (list 0.135 0.52)\n" \
	"        (list 0.205 0.59) (list 0.13 0.65) (list 0.12 0.76)\n" \
	"        (list 0.17 0.86) (list 0.235 0.98) (list 0.275 1.1)\n" \
	"        (list 0.28 1.16) (list 0.22 1.18) (list 0.18 1.14)\n" \
	"        (list 0.135 1.22) (list 0.155 1.32) (list 0.12 1.4)\n" \
	"        (list 0 1.44)\n" \
	"      )\n" \
	"      24)))\n"

/*
 * knight — the one piece that is NOT a surface of revolution, so it is a
 * deliberately pragmatic, blocky approximation rather than a sculpted horse: a
 * revolved pedestal (mesh-lathe, capped flat on top) fused with two tilted boxes
 * — a head block and a shorter snout — forward-leaning about the X axis. The
 * three sub-meshes are welded index-offset by chess-fuse; chess-box builds an
 * axis-aligned box from the six quad faces and chess-tilt rotates/translates a
 * sub-mesh into place. Faces the +Z ("forward") side; the scene turns one army
 * 180 degrees so the knights face each other. 273 verts / 1224 indices.
 */
#define CHESS_KNIGHT_MESH_SCRIPT_SRC \
	"(mesh chess-knight\n" \
	"  (generate ()\n" \
	"    (define (chess-box hx hy hz)\n" \
	"      (let ((faces (list\n" \
	"              (list (list  1.0 0.0 0.0) (list 0.0 0.0 1.0) (list 0.0 1.0 0.0))\n" \
	"              (list (list -1.0 0.0 0.0) (list 0.0 0.0 1.0) (list 0.0 1.0 0.0))\n" \
	"              (list (list 0.0  1.0 0.0) (list 1.0 0.0 0.0) (list 0.0 0.0 1.0))\n" \
	"              (list (list 0.0 -1.0 0.0) (list 1.0 0.0 0.0) (list 0.0 0.0 1.0))\n" \
	"              (list (list 0.0 0.0  1.0) (list 1.0 0.0 0.0) (list 0.0 1.0 0.0))\n" \
	"              (list (list 0.0 0.0 -1.0) (list 1.0 0.0 0.0) (list 0.0 1.0 0.0)))))\n" \
	"        (let loop ((f 0) (verts '()) (indices '()))\n" \
	"          (if (= f 6)\n" \
	"              (cons (map (lambda (vx)\n" \
	"                           (list (* (list-ref vx 0) (* 2.0 hx))\n" \
	"                                 (* (list-ref vx 1) (* 2.0 hy))\n" \
	"                                 (* (list-ref vx 2) (* 2.0 hz))\n" \
	"                                 (list-ref vx 3) (list-ref vx 4) (list-ref vx 5)\n" \
	"                                 (list-ref vx 6) (list-ref vx 7)))\n" \
	"                         verts)\n" \
	"                    indices)\n" \
	"              (let ((fc (list-ref faces f)))\n" \
	"                (loop (+ f 1)\n" \
	"                      (append verts (mesh-quad-verts (car fc) (cadr fc) (caddr fc) 0.5))\n" \
	"                      (append indices (mesh-quad-indices (* f 4)))))))))\n" \
	"    (define (chess-tilt blob ang tx ty tz)\n" \
	"      (let ((c (cos ang)) (s (sin ang)))\n" \
	"        (cons (map (lambda (vx)\n" \
	"                     (let ((px (list-ref vx 0)) (py (list-ref vx 1)) (pz (list-ref vx 2))\n" \
	"                           (nx (list-ref vx 3)) (ny (list-ref vx 4)) (nz (list-ref vx 5)))\n" \
	"                       (list (+ px tx) (+ (- (* py c) (* pz s)) ty)\n" \
	"                             (+ (+ (* py s) (* pz c)) tz)\n" \
	"                             nx (- (* ny c) (* nz s)) (+ (* ny s) (* nz c))\n" \
	"                             (list-ref vx 6) (list-ref vx 7))))\n" \
	"                   (car blob))\n" \
	"              (cdr blob))))\n" \
	"    (define (chess-fuse a b)\n" \
	"      (let ((na (length (car a))))\n" \
	"        (cons (append (car a) (car b))\n" \
	"              (append (cdr a) (map (lambda (i) (+ i na)) (cdr b))))))\n" \
	"    (let ((base (mesh-lathe\n" \
	"                  (list (list 0.0 0.0) (list 0.28 0.0) (list 0.29 0.05)\n" \
	"                        (list 0.21 0.1) (list 0.16 0.2) (list 0.155 0.34)\n" \
	"                        (list 0.185 0.42) (list 0.15 0.47) (list 0.0 0.47))\n" \
	"                  24))\n" \
	"          (head (chess-tilt (chess-box 0.12 0.23 0.28) -0.42 0.0 0.6 0.02))\n" \
	"          (snout (chess-tilt (chess-box 0.1 0.09 0.16) -0.15 0.0 0.55 0.24)))\n" \
	"      (chess-fuse (chess-fuse base head) snout))))\n"

#endif /* BUILTIN_MESH_SCRIPTS_H */
