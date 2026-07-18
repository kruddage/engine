/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * krudd_native — the offscreen native harness for the WebGPU backend.
 *
 * Boot the WebGPU backend against native Dawn, run a fixed number of frames
 * with no browser and no compositor anywhere in the path, read the colour target
 * back, and write it to a PNG. The PNG is the deliverable: it is the picture the
 * engine actually produced, in bytes.
 *
 * Why this exists: the browser canvas comes back pure black under WebGPU — not
 * even the pass's clear colour — and every hypothesis about why costs a wasm
 * rebuild, a restage and a headful-Chrome run to learn one bit. This binary
 * answers the same questions in seconds, and answers one the browser cannot
 * answer at all: whether the engine's output ever reaches a colour target, with
 * presentation taken out of the picture entirely. See spec-dawn-native-build.
 *
 * It is deliberately NOT the full engine boot. engine.c's boot is emscripten-only
 * and drags in the whole plugin table; this registers the WebGPU backend alone
 * and lets its own tick draw the probe (a clear plus two depth-tested triangles,
 * through the gpu_api vtable). That exercises pipeline creation, the render pass,
 * binding and both draw paths — the machinery under suspicion — without the
 * cluster on top of it.
 */
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log.h"
#include "log_api.h"
#include "memory.h"
#include "memory_api.h"
#include "script.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

void renderer_webgpu_plugin_entry(struct subsystem_manager *mgr);
int  renderer_webgpu_device_ready(void);
int  renderer_webgpu_read_backbuffer(uint8_t *rgba);

/* Mirrors engine.c's core service table; that one is static to a translation
 * unit whose main() is emscripten-only, so it cannot simply be shared. */
static const struct log_api g_log_api = {
	.write       = log_write,
	.get_history = log_get_history,
};

static const struct memory_api g_mem_api = {
	.alloc        = mem_alloc,
	.alloc_zero   = mem_alloc_zero,
	.free         = mem_free,
	.pool_create  = mem_pool_create,
	.pool_alloc   = mem_pool_alloc,
	.pool_free    = mem_pool_free,
	.pool_destroy = mem_pool_destroy,
};

static const struct subsystem subsystems[] = {
	{ .name = "log",    .api = &g_log_api, .init = log_init, .shutdown = log_shutdown },
	{ .name = "memory", .api = &g_mem_api, .init = mem_init, .shutdown = mem_shutdown },
	{ NULL }
};

static struct subsystem_manager manager;

/* ------------------------------------------------------------- PNG output */

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data,
		      uint32_t len)
{
	uint8_t hdr[4];
	uint8_t crcbuf[4];
	uLong crc;

	put_be32(hdr, len);
	fwrite(hdr, 1, 4, f);
	fwrite(type, 1, 4, f);
	if (len)
		fwrite(data, 1, len, f);

	crc = crc32(0L, (const Bytef *)type, 4);
	if (len)
		crc = crc32(crc, (const Bytef *)data, len);
	put_be32(crcbuf, (uint32_t)crc);
	fwrite(crcbuf, 1, 4, f);
}

static int write_png(const char *path, const uint8_t *rgba, uint32_t w,
		     uint32_t h)
{
	static const uint8_t sig[8] = {
		0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
	};
	uint8_t ihdr[13];
	uLong raw_len = (uLong)h * (1 + (uLong)w * 4);
	uint8_t *raw;
	uLong z_cap, z_len;
	uint8_t *z;
	FILE *f;
	uint32_t y;

	raw = malloc(raw_len);
	if (!raw)
		return 0;
	for (y = 0; y < h; y++) {
		uint8_t *row = raw + (size_t)y * (1 + (size_t)w * 4);

		row[0] = 0; /* filter: none */
		memcpy(row + 1, rgba + (size_t)y * (size_t)w * 4,
		       (size_t)w * 4);
	}

	z_cap = compressBound(raw_len);
	z = malloc(z_cap);
	if (!z) {
		free(raw);
		return 0;
	}
	z_len = z_cap;
	if (compress2(z, &z_len, raw, raw_len, 6) != Z_OK) {
		free(raw);
		free(z);
		return 0;
	}
	free(raw);

	f = fopen(path, "wb");
	if (!f) {
		free(z);
		return 0;
	}

	fwrite(sig, 1, sizeof sig, f);
	put_be32(ihdr + 0, w);
	put_be32(ihdr + 4, h);
	ihdr[8]  = 8; /* bit depth */
	ihdr[9]  = 6; /* colour type: RGBA */
	ihdr[10] = 0;
	ihdr[11] = 0;
	ihdr[12] = 0;
	png_chunk(f, "IHDR", ihdr, sizeof ihdr);
	png_chunk(f, "IDAT", z, (uint32_t)z_len);
	png_chunk(f, "IEND", NULL, 0);
	fclose(f);
	free(z);
	return 1;
}

/* ------------------------------------------------------------------- main */

static uint32_t size_from_env(const char *name, uint32_t fallback)
{
	const char *s = getenv(name);
	long v;

	if (!s || !*s)
		return fallback;
	v = strtol(s, NULL, 10);
	if (v < 1 || v > 16384)
		return fallback;
	return (uint32_t)v;
}

/*
 * Report what is actually in the image rather than just writing it. The whole
 * failure mode this binary exists to avoid is a confident report about a picture
 * nobody decoded — so summarise the pixels here, at the point where they are
 * still bytes.
 */
static void describe(const uint8_t *rgba, uint32_t w, uint32_t h)
{
	size_t n = (size_t)w * h;
	size_t black = 0;
	size_t i;
	uint32_t cx = w / 2;
	uint32_t cy = h / 2;
	const uint8_t *c = rgba + ((size_t)cy * w + cx) * 4;

	for (i = 0; i < n; i++) {
		const uint8_t *p = rgba + i * 4;

		if (p[0] == 0 && p[1] == 0 && p[2] == 0)
			black++;
	}

	printf("krudd_native: %ux%u, pure black %zu/%zu (%.2f%%), "
	       "centre pixel (%u,%u,%u,%u)\n",
	       w, h, black, n, n ? (double)black * 100.0 / (double)n : 0.0,
	       c[0], c[1], c[2], c[3]);
}

int main(int argc, char **argv)
{
	const char *out = (argc > 1) ? argv[1] : "krudd-native.png";
	uint32_t w = size_from_env("KRUDD_WEBGPU_WIDTH", 800);
	uint32_t h = size_from_env("KRUDD_WEBGPU_HEIGHT", 600);
	uint32_t frames = size_from_env("KRUDD_NATIVE_FRAMES", 8);
	uint8_t *rgba;
	uint32_t i;
	int ready = 0;

	subsystem_manager_init(&manager, subsystems);

	/* The shader transpiler lives in the Scheme image, and the backend lowers
	 * the probe's shaders through it while building pipelines. */
	script_init();

	renderer_webgpu_plugin_entry(&manager);

	/*
	 * The adapter/device handshake is async even natively, and the backend's
	 * own tick is what pumps the instance — so ticking is how the device
	 * arrives, not something done after it has.
	 */
	for (i = 0; i < 1000 && !ready; i++) {
		subsystem_manager_tick(&manager);
		ready = renderer_webgpu_device_ready();
	}
	if (!ready) {
		fprintf(stderr, "krudd_native: device never became ready\n");
		return 1;
	}
	printf("krudd_native: device ready after %u tick(s)\n", i);

	for (i = 0; i < frames; i++)
		subsystem_manager_tick(&manager);

	rgba = malloc((size_t)w * h * 4);
	if (!rgba) {
		fprintf(stderr, "krudd_native: out of memory\n");
		return 1;
	}
	if (!renderer_webgpu_read_backbuffer(rgba)) {
		fprintf(stderr, "krudd_native: backbuffer readback failed\n");
		free(rgba);
		return 1;
	}

	describe(rgba, w, h);

	if (!write_png(out, rgba, w, h)) {
		fprintf(stderr, "krudd_native: failed writing %s\n", out);
		free(rgba);
		return 1;
	}
	printf("krudd_native: wrote %s (%u frame(s))\n", out, frames);

	free(rgba);
	subsystem_manager_shutdown(&manager);
	return 0;
}
