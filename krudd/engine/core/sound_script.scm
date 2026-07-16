; SPDX-License-Identifier: GPL-2.0-or-later

;;! sound-script — the (sound NAME (sample (t) ...)) form, the oscillator /
;;! envelope helper vocabulary, and the sound generator. Every sound in the
;;! engine is one of these forms; there is no hardcoded C synthesizer, exactly
;;! as there is no hardcoded C texture or mesh generator (see
;;! texture_script.scm / mesh_script.scm). Mirrors the (texture ...) / (mesh ...)
;;! dispatchers.
;;!
;;! A sound script is one (sound NAME [(params ...)] (sample (t) EXPR ...))
;;! S-expression. Its sample clause is a *pure function of the time t in
;;! seconds* and must evaluate to an amplitude in [-1,1] (a real, mono — placed
;;! in both output channels) or a (list L R) stereo frame (each channel a real
;;! in [-1,1]; a short list defaults the missing channel to L, exactly as a
;;! short pixel list defaults its texture channels). Because sample is a
;;! function of seconds, not of a sample index, the same script bakes the same
;;! waveform at 44.1k or 48k: the host loop over frames is the only thing that
;;! changes. A 440 Hz tone is (snd-sine (* 440 t)) at any rate.
;;!
;;! The engine's sound-script driver (asset/sound_script.c) reads a bound
;;! ASSET_TYPE_SOUND asset's (duration ...) param, derives a frame count from
;;! the host's sample rate, calls sound-script-generate with the source text,
;;! resolved params, rate, and frame count, and marshals the returned
;;! float-vector (frame_count interleaved stereo samples) into a sound_blob.
;;! Like a texture (whose buffer size is an input) and unlike a param-less mesh,
;;! this layer does not cache: the mixer keys its ready-to-play buffers on
;;! (source, params, rate) and only reaches here on a genuine miss, so the
;;! unbounded (source x params x rate) space never accumulates in the image.

;;! --- sample helpers --------------------------------------------------
;;!
;;! A small shared vocabulary every sample clause can call — oscillators over a
;;! phase measured in whole cycles, a deterministic hash / white noise, and an
;;! ADSR envelope — so authoring a sound rarely means spelling out clamping and
;;! waveform math by hand. Loaded once into the image, available to every sound
;;! script the same way the tex-* helpers are to every texture.

;;! Clamp an amplitude to the [-1,1] the wire format stores.
(define (snd-clamp x)
  (if (real? x) (max -1.0 (min 1.0 x)) 0.0))

;;! Fractional part of x in [0,1) — the phase within one cycle. The oscillators
;;! key on this, so they stay periodic at any frequency.
(define (snd-frac x)
  (- x (floor x)))

;;! Oscillators take a phase in *whole cycles*: frequency (Hz) times t (seconds)
;;! is the cycle count, so (snd-sine (* freq t)) is a freq-Hz tone. Each returns
;;! a bipolar wave in [-1,1].
(define (snd-sine cyc)   (sin (* 2.0 pi cyc)))
(define (snd-saw cyc)    (- (* 2.0 (snd-frac cyc)) 1.0))
(define (snd-square cyc) (if (< (snd-frac cyc) 0.5) 1.0 -1.0))
(define (snd-tri cyc)    (- (* 4.0 (abs (- (snd-frac cyc) 0.5))) 1.0))

;;! Deterministic scalar hash in [0,1) — a fract(sin·k) trick, no external state,
;;! so noise bakes reproducibly for a given (source, rate). snd-noise maps it to
;;! the bipolar [-1,1] a sample clause returns.
(define (snd-hash x)
  (snd-frac (* (sin (* x 12.9898)) 43758.5453)))
(define (snd-noise x)
  (- (* 2.0 (snd-hash x)) 1.0))

;;! (snd-adsr t dur a d s r) -> an envelope gain in [0,1] at time T for a clip of
;;! length DUR seconds: a linear attack over A seconds (0->1), a decay over D
;;! (1->S), a sustain at level S until the release, then a release over R (S->0).
;;! T outside [0,DUR) is silent. The branches are ordered, so a clip too short to
;;! hold every stage simply reaches release early rather than faulting — the
;;! caller's snd-clamp bounds the final sample regardless.
(define (snd-adsr t dur a d s r)
  (cond ((< t 0.0)   0.0)
        ((>= t dur)  0.0)
        ((< t a)     (if (> a 0.0) (/ t a) 1.0))
        ((< t (+ a d))
         (if (> d 0.0) (+ 1.0 (* (- s 1.0) (/ (- t a) d))) s))
        ((< t (- dur r)) s)
        (else (if (> r 0.0) (max 0.0 (* s (/ (- dur t) r))) 0.0))))

;;! --- the (sound ...) form and driver ---------------------------------

;;! The (sound NAME [(params ...)] (sample (t) BODY ...)) form evaluates to the
;;! sample procedure — a one-argument (lambda (t) BODY ...). NAME is a
;;! human-readable label only (like the texture/mesh DSLs); the mixer keys on
;;! source text, params, and rate, not NAME. A (params ...) clause, when present,
;;! is authored data read separately (see sound-script-params); it is not a
;;! lifecycle clause, so the form ignores every clause but sample. The sample
;;! body reads its params through (param NAME) — the same reader a (texture ...)
;;! or (mesh ...) clause uses — so one source can retune, retime, or reshape.
(define-macro (sound name . clauses)
  (let ((s (assq 'sample clauses)))
    `(lambda ,(cadr s) ,@(cddr s))))

;;! (sound-script-params src) -> (TOTAL-SIZE (NAME TYPE OFFSET SIZE COMPONENTS
;;! EDIT-KIND EDIT-MIN EDIT-MAX DEFAULT) ...): a sound's authorable parameters,
;;! tight-packed, in the exact shape script-params / texture-script-params
;;! report, so the one C marshaller (script_sound_params -> query_params) and one
;;! set of editor widgets serve sounds too. Reuses the entity dispatcher's
;;! script-params-form since a (params ...) clause is identical wherever it
;;! appears. The host also reads the (duration ...) param through this to size
;;! the bake.
(define (sound-script-params src)
  (catch #t
    (lambda ()
      (script-params-form (with-input-from-string src (lambda () (read)))))
    (lambda args (cons 0 '()))))

;;! Read channel K (a real in [-1,1]) from a sample result, or FALLBACK when the
;;! list is short or the channel is non-numeric — so a malformed frame degrades
;;! to a sane value rather than faulting the whole bake.
(define (snd-chan s k fallback)
  (if (and (pair? s) (< k (length s)))
      (let ((c (list-ref s k)))
        (if (real? c) c fallback))
      fallback))

;;! Reduce a sample result to a mono amplitude: a real is itself; a (list L R)
;;! downmixes to (L+R)/2, so a stereo-authored sound still bakes mono (a
;;! spatialized point source needs one channel) without losing either side.
(define (snd-mono s)
  (if (pair? s)
      (let ((l (snd-chan s 0 0.0)))
        (* 0.5 (+ l (snd-chan s 1 l))))
      (if (real? s) s 0.0)))

;;! Parse and run SRC's sample procedure over FRAMES frames at RATE Hz into
;;! CHANNELS channels (1 mono, 2 stereo) with PARAMS (an ((name . value) ...)
;;! alist) bound as the current (param ...) scope, returning a float-vector of
;;! FRAMES*CHANNELS interleaved samples. sample is evaluated at t = i/RATE
;;! seconds for frame i, the standard PCM convention, so the result is
;;! independent of RATE for a given wall-clock time. CHANNELS is the host's
;;! choice, not the sound's: a stereo bake writes a (list L R) as-is and a real
;;! duplicated into both channels; a mono bake writes each frame downmixed to one
;;! sample (see snd-mono). A fault (a malformed form, or a sample body that
;;! errors) is caught and logged, never taking the frame down, and degrades to
;;! #f — which the host marshals to "no sound". *params* is restored on both
;;! paths so a sound fault never leaks its params into a later texture or
;;! mesh call.
(define (sound-script-run src params rate frames channels)
  (set! *params* params)
  (let ((result
          (catch #t
            (lambda ()
              (let ((sample (eval (with-input-from-string src
                                                          (lambda () (read)))
                                  (rootlet)))
                    (buf (make-float-vector (* frames channels) 0.0))
                    (mono (= channels 1)))
                (do ((i 0 (+ i 1)))
                    ((= i frames) buf)
                  (let* ((t (/ (exact->inexact i) rate))
                         (s (sample t))
                         (j (* i channels)))
                    (if mono
                        (float-vector-set! buf j (snd-clamp (snd-mono s)))
                        (if (pair? s)
                            (let ((l (snd-chan s 0 0.0)))
                              (float-vector-set! buf j       (snd-clamp l))
                              (float-vector-set! buf (+ j 1) (snd-clamp (snd-chan s 1 l))))
                            (let ((m (snd-clamp (if (real? s) s 0.0))))
                              (float-vector-set! buf j       m)
                              (float-vector-set! buf (+ j 1) m))))))))
            (lambda args
              (krudd-log 2 (string-append "sound-script: fault: " src))
              #f))))
    (set! *params* '())
    result))

;;! (sound-script-generate src params rate frames channels) -> a float-vector of
;;! FRAMES*CHANNELS interleaved samples, or #f on fault / non-positive size /
;;! unsupported channel count. The one entry point the C driver calls; see
;;! sound-script-run for the sampling contract. RATE and FRAMES are integers the
;;! host chooses — the rate from its audio context, the frame count from the
;;! sound's (duration ...) param times that rate — and CHANNELS is 1 or 2, so
;;! the script itself never names any of them.
(define (sound-script-generate src params rate frames channels)
  (if (and (integer? rate) (integer? frames) (> rate 0) (> frames 0)
           (or (= channels 1) (= channels 2)))
      (sound-script-run src params rate frames channels)
      #f))
