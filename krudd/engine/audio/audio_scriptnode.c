/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * audio_scriptnode — the browser device backend that drives audio_core from a
 * Web Audio ScriptProcessorNode.
 *
 * This is the low-friction alternative to the AudioWorklet backend: a
 * ScriptProcessorNode's onaudioprocess fires on the MAIN thread, so there is no
 * separate audio thread, no shared memory, and no build-flag changes — it runs
 * under the stock WASM link flags (no AUDIO_WORKLET / WASM_WORKERS, no
 * GROWABLE_ARRAYBUFFERS flip, no cross-origin isolation). Because play and
 * render both land on the main thread, audio_core's command ring is drained on
 * the same thread it is filled — harmless, and it keeps this backend a drop-in
 * for the exact same core.
 *
 * The trade-off: ScriptProcessorNode is deprecated and runs the mix on the main
 * thread, so a heavy frame can glitch audio. For procedural SFX that is a fine
 * exchange for shipping sound with zero build risk; the AudioWorklet backend is
 * the upgrade when its build story is verified.
 *
 * Browser-only, fenced under __EMSCRIPTEN__; the native build gets a no-op
 * plugin_entry. The mixer and thread-seam logic are native-tested in
 * mixer_test.c / audio_core_test.c.
 */
#include "audio_core.h"
#include "audio_api.h"

#include "asset_api.h"
#include "log_api.h"
#include "memory_api.h"
#include "subsystem.h"
#include "subsystem_manager.h"

#ifdef __EMSCRIPTEN__

#include "sound_script.h" /* sound_script_generate */

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include <string.h>

/* ScriptProcessorNode block size (a power of two in [256, 16384]); 1024 frames
 * is ~21 ms at 48 kHz — a reasonable latency/robustness point for SFX. */
#define AUDIO_SP_BUFFER 1024
/* Upper bound the render scratch must cover (the largest legal block). */
#define AUDIO_SP_MAX 16384

static const struct log_api    *g_log;
static const struct memory_api *g_mem;
static const struct asset_api  *g_asset;
static struct audio_core       *g_core;

/* Interleaved stereo scratch the node reads each block. Static, so its address
 * is stable under memory growth (growth appends; it never moves existing data). */
static float g_scratch[AUDIO_SP_MAX * 2];

/*
 * Create the AudioContext (stored on the Module for later JS steps) and return
 * its sample rate, or 0 if Web Audio is unavailable. Kept in JS because
 * ScriptProcessorNode has no Emscripten C binding (unlike the worklet API).
 */
EM_JS(int, krudd_sp_create_context, (void), {
	var AC = window.AudioContext || window.webkitAudioContext;
	if (!AC)
		return 0;
	var ctx = new AC();
	Module.__kruddAudioCtx = ctx;
	return ctx.sampleRate | 0;
})

/*
 * Attach a ScriptProcessorNode whose callback pulls one interleaved-stereo block
 * from C (audio_sp_render) and splits it into the node's two output channels.
 * HEAPF32 is re-read each call so a memory-growth reallocation can't leave a
 * stale view.
 */
EM_JS(void, krudd_sp_attach, (int buf_size), {
	var ctx = Module.__kruddAudioCtx;
	if (!ctx)
		return;
	var node = ctx.createScriptProcessor(buf_size, 0, 2);
	node.onaudioprocess = function (e) {
		var out = e.outputBuffer;
		var n = out.length;
		var ptr = Module._audio_sp_render(n);
		var base = ptr >> 2;
		var heap = HEAPF32;
		var l = out.getChannelData(0);
		var r = out.getChannelData(1);
		for (var i = 0; i < n; i++) {
			l[i] = heap[base + 2 * i];
			r[i] = heap[base + 2 * i + 1];
		}
	};
	node.connect(ctx.destination);
	Module.__kruddAudioNode = node;
})

/* Resume the context; must be called from within a user gesture (autoplay). */
EM_JS(void, krudd_sp_resume, (void), {
	var ctx = Module.__kruddAudioCtx;
	if (ctx && ctx.state !== 'running' && ctx.resume)
		ctx.resume();
})

/*
 * Main thread (the ScriptProcessorNode callback runs here): render one block of
 * FRAMES interleaved stereo samples and hand back the scratch pointer for JS to
 * read out of HEAPF32. Exported so the onaudioprocess closure can call it.
 */
EMSCRIPTEN_KEEPALIVE float *audio_sp_render(int frames)
{
	if (frames < 0)
		frames = 0;
	if (frames > AUDIO_SP_MAX)
		frames = AUDIO_SP_MAX;
	audio_core_render(g_core, g_scratch, (uint32_t)frames);
	return g_scratch;
}

/* Main thread: bake sound asset ID to a mono blob at RATE; audio_core caches
 * and owns it. Identical resolver to the worklet backend. */
static struct sound_blob *sp_bake(void *ctx, uint32_t asset_id,
				  uint32_t sample_rate, uint32_t channels)
{
	const char *src;
	uint32_t    size = 0;

	(void)ctx;
	src = g_asset ? g_asset->get_data(asset_id, &size) : NULL;
	if (!src)
		return NULL;
	return sound_script_generate(src, NULL, 0, sample_rate, channels,
				     g_mem, NULL);
}

static EM_BOOL sp_on_gesture(int type, const EmscriptenMouseEvent *e, void *ud)
{
	(void)type;
	(void)e;
	(void)ud;
	krudd_sp_resume();
	return EM_FALSE;
}

static void audio_sp_init(void)
{
	int rate = krudd_sp_create_context();

	if (rate <= 0)
		rate = 48000; /* no Web Audio: core still bakes, just stays silent */

	g_core = audio_core_create(g_mem, (uint32_t)rate, sp_bake, NULL);
	if (!g_core) {
		g_log->write(LOG_LEVEL_ERROR, "audio: core creation failed");
		return;
	}
	krudd_sp_attach(AUDIO_SP_BUFFER);
	emscripten_set_click_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL,
				      EM_TRUE, sp_on_gesture);
	g_log->write(LOG_LEVEL_INFO,
		     "audio: ScriptProcessorNode at %d Hz", rate);
}

static void audio_sp_shutdown(void)
{
	if (g_core) {
		audio_core_destroy(g_core);
		g_core = NULL;
	}
}

/* Resolve a catalog path to its stable id by scanning the catalog, or 0. */
static uint32_t sp_find_path(const char *path)
{
	uint32_t i, n;

	if (!g_asset || !path)
		return 0;
	n = g_asset->count();
	for (i = 0; i < n; i++) {
		struct asset_info info;

		if (g_asset->info(i, &info) == 0 && info.path &&
		    strcmp(info.path, path) == 0)
			return info.id;
	}
	return 0;
}

static void api_play(uint32_t asset_id, float gain, float pan, float rate)
{
	if (g_core)
		audio_core_play_asset(g_core, asset_id, gain, pan, rate);
}

static uint32_t api_play_path(const char *path, float gain, float pan,
			      float rate)
{
	uint32_t id = sp_find_path(path);

	if (id)
		api_play(id, gain, pan, rate);
	return id;
}

static const struct audio_api g_audio_api = {
	.play      = api_play,
	.play_path = api_play_path,
};

/*
 * Console test hooks: after one click unlocks the context,
 * `Module._audio_test_beep()` (or _blip / _noise) plays a built-in through the
 * whole path — bake, enqueue, node mix. The audible proof this backend works.
 */
EMSCRIPTEN_KEEPALIVE void audio_test_beep(void)
{
	api_play_path("builtin://sound/beep", 0.8f, 0.0f, 1.0f);
}
EMSCRIPTEN_KEEPALIVE void audio_test_blip(void)
{
	api_play_path("builtin://sound/blip", 0.8f, 0.0f, 1.0f);
}
EMSCRIPTEN_KEEPALIVE void audio_test_noise(void)
{
	api_play_path("builtin://sound/noise-burst", 0.8f, 0.0f, 1.0f);
}

static const struct subsystem desc = {
	.name     = "audio",
	.api      = &g_audio_api,
	.init     = audio_sp_init,
	.shutdown = audio_sp_shutdown,
};

void audio_scriptnode_plugin_entry(struct subsystem_manager *mgr)
{
	g_log   = subsystem_manager_get_api(mgr, "log");
	g_mem   = subsystem_manager_get_api(mgr, "memory");
	g_asset = subsystem_manager_get_api(mgr, "asset");
	subsystem_manager_register(mgr, &desc);
}

#else /* !__EMSCRIPTEN__ */

/* Native builds have no audio device; the backend is a no-op so the tree
 * compiles and links. The playback core itself is native-tested directly. */
void audio_scriptnode_plugin_entry(struct subsystem_manager *mgr)
{
	(void)mgr;
}

#endif /* __EMSCRIPTEN__ */
