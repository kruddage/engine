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
 */

/*
 * cube — 6 quad faces via mesh-quad-verts/-indices (core/mesh_script.scm),
 * 24 verts / 36 indices. Unit-sized, centred on the origin.
 */
#define CUBE_MESH_SCRIPT_SRC \
	"(mesh cube\n" \
	"  (generate ()\n" \
	"    (let* ((faces (list\n" \
	"                    (list (list 1.0 0.0 0.0) (list 0.0 0.0 1.0) (list 0.0 1.0 0.0))\n" \
	"                    (list (list -1.0 0.0 0.0) (list 0.0 0.0 1.0) (list 0.0 1.0 0.0))\n" \
	"                    (list (list 0.0 1.0 0.0) (list 1.0 0.0 0.0) (list 0.0 0.0 1.0))\n" \
	"                    (list (list 0.0 -1.0 0.0) (list 1.0 0.0 0.0) (list 0.0 0.0 1.0))\n" \
	"                    (list (list 0.0 0.0 1.0) (list 1.0 0.0 0.0) (list 0.0 1.0 0.0))\n" \
	"                    (list (list 0.0 0.0 -1.0) (list 1.0 0.0 0.0) (list 0.0 1.0 0.0)))))\n" \
	"      (let loop ((f 0) (verts '()) (indices '()))\n" \
	"        (if (= f 6)\n" \
	"            (cons verts indices)\n" \
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

#endif /* BUILTIN_MESH_SCRIPTS_H */
