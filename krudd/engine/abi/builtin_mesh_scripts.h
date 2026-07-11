/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BUILTIN_MESH_SCRIPTS_H
#define BUILTIN_MESH_SCRIPTS_H

/*
 * Built-in mesh scripts, seeded as read-only ASSET_TYPE_MESH_SCRIPT assets
 * (see asset_plugin.c). Each is a single (mesh NAME (generate () ...)) form
 * in the same S7 Scheme the shader and entity-script DSLs use — the runtime
 * image's (mesh ...) macro registers it and asset/mesh_script.c marshals its
 * result into a mesh_blob on demand (see core/mesh_script.scm).
 *
 * Kept here as one shared source of truth so the asset seeder and the native
 * mesh_script oracle test compile the exact same script text.
 */

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
