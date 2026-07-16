/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BUILTIN_SOUND_SCRIPTS_H
#define BUILTIN_SOUND_SCRIPTS_H

/*
 * Built-in sounds, seeded as read-only ASSET_TYPE_SOUND assets (see
 * asset_plugin.c). Each is a single (sound NAME (params ...) (sample (t) ...))
 * form in the same S7 Scheme the texture / shader / entity / mesh DSLs use —
 * the runtime image's (sound ...) macro evaluates it to a sample procedure and
 * asset/sound_script.c bakes the result into a sound_blob on demand (see
 * core/sound_script.scm). There is no hardcoded C synthesizer: every built-in
 * clip, like these three, is authored the same way an editor-created sound
 * script is, and is sample-rate-independent — sample is a pure function of the
 * time t in seconds, so the same source bakes at any rate.
 *
 * Each declares a (duration ...) param: the host reads it to size the bake
 * (frames = rate * duration), so the clip's length lives in the same param
 * system, and the same editor widgets, as its pitch and shape.
 *
 * Kept here as one shared source of truth so the asset seeder and the native
 * sound_script oracle test can compile the exact same script text.
 */

/*
 * beep — a sine tone shaped by a gentle ADSR. freq is the pitch in Hz; duration
 * the clip length in seconds. The 440 Hz default is concert A, a plain
 * confirmation tone.
 */
#define BEEP_SOUND_SCRIPT_SRC \
	"(sound beep\n" \
	"  (params\n" \
	"    (freq     float (edit range 20 4000) (default 440))\n" \
	"    (duration float (edit range 0.02 4)  (default 0.3)))\n" \
	"  (sample (t)\n" \
	"    (* (snd-adsr t (or (param 'duration) 0.3) 0.005 0.04 0.7 0.08)\n" \
	"       (snd-sine (* (or (param 'freq) 440.0) t)))))\n"

/*
 * blip — a short percussive click: a square wave plucked by a fast attack and
 * decay to silence (sustain 0), the UI tick twin of the beep. Higher default
 * pitch and a tenth the length so it reads as a tap, not a note.
 */
#define BLIP_SOUND_SCRIPT_SRC \
	"(sound blip\n" \
	"  (params\n" \
	"    (freq     float (edit range 200 4000) (default 880))\n" \
	"    (duration float (edit range 0.01 1)   (default 0.08)))\n" \
	"  (sample (t)\n" \
	"    (let ((d (or (param 'duration) 0.08)))\n" \
	"      (* 0.6\n" \
	"         (snd-adsr t d 0.001 0.03 0.0 0.02)\n" \
	"         (snd-square (* (or (param 'freq) 880.0) t))))))\n"

/*
 * noise-burst — white noise under a long linear fade: an impact / whoosh. The
 * hash is fed 9000*t so consecutive samples draw uncorrelated values (true
 * per-sample noise), and the release spans almost the whole clip so it tails off
 * smoothly. Self-contained — the hash is a fract(sin·k) trick, no external state
 * — so it bakes deterministically for a given rate.
 */
#define NOISE_BURST_SOUND_SCRIPT_SRC \
	"(sound noise-burst\n" \
	"  (params\n" \
	"    (duration float (edit range 0.02 3) (default 0.4)))\n" \
	"  (sample (t)\n" \
	"    (let ((d (or (param 'duration) 0.4)))\n" \
	"      (* (snd-adsr t d 0.002 0.0 1.0 (* d 0.98))\n" \
	"         (snd-noise (* 9000.0 t))))))\n"

#endif /* BUILTIN_SOUND_SCRIPTS_H */
