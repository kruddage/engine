/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BUILTIN_TEXTURE_SCRIPTS_H
#define BUILTIN_TEXTURE_SCRIPTS_H

/*
 * Built-in textures, seeded as read-only ASSET_TYPE_TEXTURE assets (see
 * asset_plugin.c). Each is a single (texture NAME (params ...) (shade (u v)
 * ...)) form in the same S7 Scheme the shader / entity / mesh DSLs use — the
 * runtime image's (texture ...) macro evaluates it to a shade procedure and
 * asset/texture_script.c bakes the result into a texture_blob on demand (see
 * core/texture_script.scm). There is no hardcoded C texture generator: every
 * built-in image, like these three, is authored the same way an editor-created
 * texture script is, and is resolution-independent — shade is a pure function of
 * the normalized coordinate u,v in [0,1), so the same source bakes at any size.
 *
 * Kept here as one shared source of truth so the asset seeder and the native
 * texture_script oracle test can compile the exact same script text.
 */

/*
 * checker — a two-colour checkerboard. scale is the number of checks across the
 * unit square; color-a / color-b are the two tints. Parity of the summed cell
 * indices picks the tint, so the pattern is crisp at any resolution.
 */
#define CHECKER_TEXTURE_SCRIPT_SRC \
	"(texture checker\n" \
	"  (params\n" \
	"    (scale   float (edit range 1 64) (default 8))\n" \
	"    (color-a vec4  (edit color) (default 0.85 0.85 0.85 1.0))\n" \
	"    (color-b vec4  (edit color) (default 0.12 0.12 0.14 1.0)))\n" \
	"  (shade (u v)\n" \
	"    (let ((s (or (param 'scale) 8.0)))\n" \
	"      (if (even? (+ (tex-ifloor (* u s)) (tex-ifloor (* v s))))\n" \
	"          (or (param 'color-a) (list 0.85 0.85 0.85 1.0))\n" \
	"          (or (param 'color-b) (list 0.12 0.12 0.14 1.0))))))\n"

/*
 * gradient — a vertical blend from top to bottom. tex-mix interpolates the two
 * colour lists (alpha included) by v, so the same script fills any resolution
 * with the identical ramp.
 */
#define GRADIENT_TEXTURE_SCRIPT_SRC \
	"(texture gradient\n" \
	"  (params\n" \
	"    (top    vec4 (edit color) (default 0.12 0.34 0.80 1.0))\n" \
	"    (bottom vec4 (edit color) (default 0.95 0.62 0.20 1.0)))\n" \
	"  (shade (u v)\n" \
	"    (tex-mix (or (param 'top)    (list 0.12 0.34 0.80 1.0))\n" \
	"             (or (param 'bottom) (list 0.95 0.62 0.20 1.0))\n" \
	"             v)))\n"

/*
 * noise — smooth value noise. A hashed value per integer lattice cell, bilinearly
 * interpolated with a smoothstep fade, so it reads as soft grey clouds rather
 * than per-texel static. scale sets the lattice frequency (cells across the unit
 * square); seed offsets the hash for a different field. Self-contained — the hash
 * is a fract(sin·k) trick, no external state — so it bakes deterministically at
 * any resolution.
 */
#define NOISE_TEXTURE_SCRIPT_SRC \
	"(texture noise\n" \
	"  (params\n" \
	"    (scale float (edit range 1 32) (default 6))\n" \
	"    (seed  float (edit range 0 100) (default 0)))\n" \
	"  (shade (u v)\n" \
	"    (let* ((s  (or (param 'scale) 6.0))\n" \
	"           (sd (or (param 'seed) 0.0))\n" \
	"           (x  (* u s)) (y (* v s))\n" \
	"           (ix (tex-ifloor x)) (iy (tex-ifloor y))\n" \
	"           (fx (- x ix)) (fy (- y iy))\n" \
	"           (h  (lambda (a b)\n" \
	"                 (let ((r (* (sin (+ (* a 127.1) (* b 311.7) (* sd 74.7)))\n" \
	"                             43758.5453)))\n" \
	"                   (- r (floor r)))))\n" \
	"           (ux (* fx fx (- 3.0 (* 2.0 fx))))\n" \
	"           (uy (* fy fy (- 3.0 (* 2.0 fy))))\n" \
	"           (na (h ix iy)) (nb (h (+ ix 1) iy))\n" \
	"           (nc (h ix (+ iy 1))) (nd (h (+ ix 1) (+ iy 1)))\n" \
	"           (top (+ na (* (- nb na) ux)))\n" \
	"           (bot (+ nc (* (- nd nc) ux)))\n" \
	"           (n   (+ top (* (- bot top) uy))))\n" \
	"      (list n n n 1.0))))\n"

/*
 * grass — a substance-designer-style ground cover: two-octave value noise
 * mottles a dark-to-light green field into patchy tufts, and a sharper, higher-
 * frequency third octave rides along in the alpha channel as a per-texel
 * "blade height". Colour and height share the same lattice (offset by `seed`
 * per octave) so a bright tuft in the albedo is also a raised tuft in the
 * height field — the two read as one surface, not two independent layers.
 * Nothing here is grass-specific machinery: it is the same hashed-lattice
 * smoothstep technique as the noise texture above, just layered and tinted.
 * The pbr-textured shader (asset_plugin.c) reads the alpha channel back out
 * to bump the surface normal, so this texture doubles as color AND a height map
 * with no separate bake.
 */
#define GRASS_TEXTURE_SCRIPT_SRC \
	"(texture grass\n" \
	"  (params\n" \
	"    (scale     float (edit range 1 64) (default 18))\n" \
	"    (dark      vec4  (edit color) (default 0.07 0.20 0.06 1.0))\n" \
	"    (light     vec4  (edit color) (default 0.28 0.52 0.16 1.0))\n" \
	"    (bump-freq float (edit range 1 128) (default 46)))\n" \
	"  (shade (u v)\n" \
	"    (let* ((lat (lambda (x y sd)\n" \
	"                  (let* ((ix (tex-ifloor x)) (iy (tex-ifloor y))\n" \
	"                         (fx (- x ix)) (fy (- y iy))\n" \
	"                         (h  (lambda (a b)\n" \
	"                               (let ((r (* (sin (+ (* a 127.1) (* b 311.7)\n" \
	"                                                    (* sd 74.7)))\n" \
	"                                           43758.5453)))\n" \
	"                                 (- r (floor r)))))\n" \
	"                         (ux (* fx fx (- 3.0 (* 2.0 fx))))\n" \
	"                         (uy (* fy fy (- 3.0 (* 2.0 fy))))\n" \
	"                         (na (h ix iy)) (nb (h (+ ix 1) iy))\n" \
	"                         (nc (h ix (+ iy 1))) (nd (h (+ ix 1) (+ iy 1)))\n" \
	"                         (top (+ na (* (- nb na) ux)))\n" \
	"                         (bot (+ nc (* (- nd nc) ux))))\n" \
	"                    (+ top (* (- bot top) uy)))))\n" \
	"           (s   (or (param 'scale) 18.0))\n" \
	"           (bf  (or (param 'bump-freq) 46.0))\n" \
	"           (lo  (lat (* u s) (* v s) 1.0))\n" \
	"           (hi  (lat (* u s 2.0) (* v s 2.0) 7.0))\n" \
	"           (mottle (tex-clamp01 (+ (* 0.7 lo) (* 0.3 hi))))\n" \
	"           (col (tex-mix (or (param 'dark)  (list 0.07 0.20 0.06 1.0))\n" \
	"                         (or (param 'light) (list 0.28 0.52 0.16 1.0))\n" \
	"                         mottle))\n" \
	"           (blade (lat (* u bf) (* v bf) 3.0))\n" \
	"           (height (tex-clamp01 (+ (* 0.55 mottle) (* 0.45 blade)))))\n" \
	"      (list (tex-channel col 0 0.0) (tex-channel col 1 0.0)\n" \
	"            (tex-channel col 2 0.0) height))))\n"

#endif /* BUILTIN_TEXTURE_SCRIPTS_H */
