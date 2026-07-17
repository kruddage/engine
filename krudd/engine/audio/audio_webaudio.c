/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * audio_webaudio — the browser device backend that drives audio_core from an
 * Emscripten AudioWorklet.
 *
 * On registration it creates a (suspended) AudioContext, reads the device's
 * sample rate, builds an audio_core at that rate, and spins up a WASM audio
 * worklet whose process callback — running on the real audio thread — calls
 * audio_core_render into the output. The main thread bakes sounds (S7 runs
 * here, never on the audio thread) and enqueues them through audio_core; the
 * worklet drains and mixes. Autoplay policy keeps the context suspended until
 * the first user gesture, which resume() unlocks.
 *
 * Everything here is browser-only and fenced under __EMSCRIPTEN__; the native
 * build gets a no-op plugin_entry so the tree still compiles. The mixer and the
 * thread-seam logic it drives are native-tested in audio_core_test.c /
 * mixer_test.c — this file is the device glue those tests can't reach.
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
#include <emscripten/webaudio.h>

#include <string.h>

/* Render quantum of a Web Audio worklet: always 128 frames per process call. */
#define AUDIO_WA_QUANTUM 128

static const struct log_api    *g_log;
static const struct memory_api *g_mem;
static const struct asset_api  *g_asset;
static struct audio_core       *g_core;
static EMSCRIPTEN_WEBAUDIO_T     g_ctx;

/* The audio worklet thread runs on its own stack; keep it comfortably larger
 * than the render callback's frame (a 256-float scratch plus mixer locals). */
static char g_worklet_stack[16384] __attribute__((aligned(16)));

/* Read the live AudioContext's sample rate; emscriptenGetAudioObject maps the
 * handle to its Web Audio object. 0 if the handle is stale. */
EM_JS(int, krudd_wa_sample_rate, (EMSCRIPTEN_WEBAUDIO_T ctx), {
	var o = emscriptenGetAudioObject(ctx);
	return o ? o.sampleRate : 0;
})

/* Main thread: bake sound asset ID to a mono blob at RATE. audio_core caches
 * and owns the result. S7 runs here, on the main thread, never on the worklet. */
static struct sound_blob *wa_bake(void *ctx, uint32_t asset_id,
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

/* Audio thread: fill the worklet's output. Web Audio output is planar — channel
 * c occupies data[c*128 .. c*128+127] — so render interleaved into a scratch
 * buffer and split it out. Returning true keeps the processor alive. */
static EM_BOOL wa_process(int num_inputs, const AudioSampleFrame *inputs,
			  int num_outputs, AudioSampleFrame *outputs,
			  int num_params, const AudioParamFrame *params,
			  void *user_data)
{
	struct audio_core *core = user_data;
	float              scratch[AUDIO_WA_QUANTUM * 2];
	int                i, ch;

	(void)num_inputs;
	(void)inputs;
	(void)num_params;
	(void)params;
	if (num_outputs < 1)
		return EM_TRUE;

	audio_core_render(core, scratch, AUDIO_WA_QUANTUM);

	ch = outputs[0].numberOfChannels;
	if (ch >= 2) {
		float *l = outputs[0].data;
		float *r = outputs[0].data + AUDIO_WA_QUANTUM;

		for (i = 0; i < AUDIO_WA_QUANTUM; i++) {
			l[i] = scratch[2 * i];
			r[i] = scratch[2 * i + 1];
		}
	} else if (ch == 1) {
		float *m = outputs[0].data;

		for (i = 0; i < AUDIO_WA_QUANTUM; i++)
			m[i] = 0.5f * (scratch[2 * i] + scratch[2 * i + 1]);
	}
	return EM_TRUE;
}

/* Worklet processor is ready: create the node and wire it to the destination. */
static void wa_processor_created(EMSCRIPTEN_WEBAUDIO_T ctx, EM_BOOL success,
				 void *user_data)
{
	int output_channels[1] = { 2 };
	EmscriptenAudioWorkletNodeCreateOptions opts = {
		.numberOfInputs  = 0,
		.numberOfOutputs = 1,
		.outputChannelCounts = output_channels,
	};
	EMSCRIPTEN_AUDIO_WORKLET_NODE_T node;

	(void)user_data;
	if (!success) {
		g_log->write(LOG_LEVEL_ERROR,
			     "audio: worklet processor creation failed");
		return;
	}
	node = emscripten_create_wasm_audio_worklet_node(ctx, "krudd-mixer",
							 &opts, &wa_process,
							 g_core);
	emscripten_audio_node_connect(node, ctx, 0, 0);
	g_log->write(LOG_LEVEL_INFO, "audio: worklet node connected");
}

/* Worklet thread is up: create our processor on it. */
static void wa_thread_started(EMSCRIPTEN_WEBAUDIO_T ctx, EM_BOOL success,
			      void *user_data)
{
	WebAudioWorkletProcessorCreateOptions opts = {
		.name = "krudd-mixer",
		.numAudioParams = 0,
		.audioParamDescriptors = 0,
	};

	(void)user_data;
	if (!success) {
		g_log->write(LOG_LEVEL_ERROR,
			     "audio: worklet thread start failed");
		return;
	}
	emscripten_create_wasm_audio_worklet_processor_async(
		ctx, &opts, wa_processor_created, NULL);
}

/* Autoplay unlock: the context can only resume inside a user gesture. Resume on
 * the first document click/tap; the browser tolerates repeat resumes. */
static EM_BOOL wa_on_gesture(int type, const EmscriptenMouseEvent *e, void *ud)
{
	(void)type;
	(void)e;
	(void)ud;
	if (g_ctx)
		emscripten_resume_audio_context_sync(g_ctx);
	return EM_FALSE;
}

static void audio_wa_init(void)
{
	int rate;

	g_ctx = emscripten_create_audio_context(0);
	rate  = krudd_wa_sample_rate(g_ctx);
	if (rate <= 0)
		rate = 48000; /* a sane default if the handle read fails */

	g_core = audio_core_create(g_mem, (uint32_t)rate, wa_bake, NULL);
	if (!g_core) {
		g_log->write(LOG_LEVEL_ERROR, "audio: core creation failed");
		return;
	}

	emscripten_set_click_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL,
				      EM_TRUE, wa_on_gesture);
	emscripten_start_wasm_audio_worklet_thread_async(
		g_ctx, g_worklet_stack, sizeof(g_worklet_stack),
		wa_thread_started, NULL);
	g_log->write(LOG_LEVEL_INFO, "audio: worklet starting at %d Hz", rate);
}

static void audio_wa_shutdown(void)
{
	if (g_core) {
		audio_core_destroy(g_core);
		g_core = NULL;
	}
}

/* Resolve a catalog path to its stable id by scanning the catalog, or 0. */
static uint32_t wa_find_path(const char *path)
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
	uint32_t id = wa_find_path(path);

	if (id)
		api_play(id, gain, pan, rate);
	return id;
}

static const struct audio_api g_audio_api = {
	.play      = api_play,
	.play_path = api_play_path,
};

/*
 * Console test hooks: from the browser dev console, `Module._audio_test_beep()`
 * (or _blip / _noise) plays a built-in through the whole path — bake, enqueue,
 * worklet mix — after one click has unlocked the context. The audible proof
 * that this backend works, without any game logic wired to it yet.
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
	.init     = audio_wa_init,
	.shutdown = audio_wa_shutdown,
};

void audio_webaudio_plugin_entry(struct subsystem_manager *mgr)
{
	g_log   = subsystem_manager_get_api(mgr, "log");
	g_mem   = subsystem_manager_get_api(mgr, "memory");
	g_asset = subsystem_manager_get_api(mgr, "asset");
	subsystem_manager_register(mgr, &desc);
}

#else /* !__EMSCRIPTEN__ */

/* Native builds have no audio device; the backend is a no-op so the tree
 * compiles and links. The playback core itself is native-tested directly. */
void audio_webaudio_plugin_entry(struct subsystem_manager *mgr)
{
	(void)mgr;
}

#endif /* __EMSCRIPTEN__ */
