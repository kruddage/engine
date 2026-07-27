/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * dawn-smoke — the smallest possible proof that a native Dawn build works
 * offscreen on this box.
 *
 * It touches NO engine code. It exists to answer one question: can we create a
 * Dawn instance/adapter/device with no window, clear a texture to a known
 * colour, read the pixels back, and write them out? If this binary produces a
 * PNG with the right bytes in it, the build seam is real and chunk 2 can lean
 * on it.
 *
 * Deliberately offscreen: no surface, no swapchain, no GLFW, no X11 — see
 * spec-dawn-native-build.md, "The one design call".
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <webgpu/webgpu.h>
#include <zlib.h>

#define TEX_W 256
#define TEX_H 256

/* The colour we ask for. Chosen so every channel is exactly representable in
 * 8-bit unorm (k/255), which means an exact byte comparison on readback is a
 * fair test rather than a rounding argument. */
#define CLEAR_R 217 /* 0xD9 */
#define CLEAR_G 119 /* 0x77 */
#define CLEAR_B  87 /* 0x57 */
#define CLEAR_A 255

static const char *g_stage = "startup";

#define FAIL(...) do { \
	fprintf(stderr, "dawn-smoke: FAILED at stage '%s': ", g_stage); \
	fprintf(stderr, __VA_ARGS__); \
	fprintf(stderr, "\n"); \
	exit(1); \
} while (0)

static void sv_print(const char *label, WGPUStringView s)
{
	if (s.data && s.length)
		fprintf(stderr, "%s: %.*s\n", label, (int)s.length, s.data);
}

/* ---------------------------------------------------------------- callbacks */

struct adapter_result { WGPUAdapter adapter; int ok; };
struct device_result  { WGPUDevice device;   int ok; };
struct map_result     { int ok; };

static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
		       WGPUStringView message, void *ud1, void *ud2)
{
	struct adapter_result *r = ud1;
	(void)ud2;
	sv_print("  adapter message", message);
	r->ok = (status == WGPURequestAdapterStatus_Success);
	r->adapter = adapter;
}

static void on_device(WGPURequestDeviceStatus status, WGPUDevice device,
		      WGPUStringView message, void *ud1, void *ud2)
{
	struct device_result *r = ud1;
	(void)ud2;
	sv_print("  device message", message);
	r->ok = (status == WGPURequestDeviceStatus_Success);
	r->device = device;
}

static void on_map(WGPUMapAsyncStatus status, WGPUStringView message,
		   void *ud1, void *ud2)
{
	struct map_result *r = ud1;
	(void)ud2;
	sv_print("  map message", message);
	r->ok = (status == WGPUMapAsyncStatus_Success);
}

/* Any validation error at all is a hard failure — this binary's whole job is to
 * be a trustworthy oracle, so it must not quietly render past a complaint. */
static void on_uncaptured_error(WGPUDevice const *device, WGPUErrorType type,
				WGPUStringView message, void *ud1, void *ud2)
{
	(void)device; (void)ud1; (void)ud2;
	fprintf(stderr, "dawn-smoke: UNCAPTURED ERROR (type %d)\n", (int)type);
	sv_print("  ", message);
	exit(1);
}

static void on_device_lost(WGPUDevice const *device, WGPUDeviceLostReason reason,
			   WGPUStringView message, void *ud1, void *ud2)
{
	(void)device; (void)ud1; (void)ud2;
	if (reason == WGPUDeviceLostReason_Destroyed)
		return; /* normal teardown */
	fprintf(stderr, "dawn-smoke: DEVICE LOST (reason %d)\n", (int)reason);
	sv_print("  ", message);
	exit(1);
}

/* Block until `f` resolves. Requires the TimedWaitAny instance feature, which
 * we request below. */
static void wait_for(WGPUInstance instance, WGPUFuture f)
{
	WGPUFutureWaitInfo info = { .future = f, .completed = 0 };
	WGPUWaitStatus st = wgpuInstanceWaitAny(instance, 1, &info,
						UINT64_C(5000000000) /* 5s */);
	if (st != WGPUWaitStatus_Success)
		FAIL("wgpuInstanceWaitAny returned %d", (int)st);
	if (!info.completed)
		FAIL("future did not complete within timeout");
}

/* --------------------------------------------------------------- PNG output */

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
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

/* rgba is TEX_W * TEX_H * 4 bytes, top row first. */
static void write_png(const char *path, const uint8_t *rgba, int w, int h)
{
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	uint8_t ihdr[13];
	uLong raw_len = (uLong)h * (1 + (uLong)w * 4);
	uint8_t *raw = malloc(raw_len);
	uLong z_cap, z_len;
	uint8_t *z;
	FILE *f;
	int y;

	if (!raw)
		FAIL("out of memory building PNG scanlines");
	for (y = 0; y < h; y++) {
		uint8_t *row = raw + (size_t)y * (1 + (size_t)w * 4);
		row[0] = 0; /* filter: none */
		memcpy(row + 1, rgba + (size_t)y * (size_t)w * 4, (size_t)w * 4);
	}

	z_cap = compressBound(raw_len);
	z = malloc(z_cap);
	if (!z)
		FAIL("out of memory compressing PNG");
	z_len = z_cap;
	if (compress2(z, &z_len, raw, raw_len, 6) != Z_OK)
		FAIL("zlib compress2 failed");

	f = fopen(path, "wb");
	if (!f)
		FAIL("cannot open %s for writing", path);

	fwrite(sig, 1, sizeof sig, f);
	put_be32(ihdr + 0, (uint32_t)w);
	put_be32(ihdr + 4, (uint32_t)h);
	ihdr[8] = 8;  /* bit depth */
	ihdr[9] = 6;  /* colour type: RGBA */
	ihdr[10] = 0; /* compression */
	ihdr[11] = 0; /* filter */
	ihdr[12] = 0; /* interlace */
	png_chunk(f, "IHDR", ihdr, sizeof ihdr);
	png_chunk(f, "IDAT", z, (uint32_t)z_len);
	png_chunk(f, "IEND", NULL, 0);
	fclose(f);

	free(z);
	free(raw);
}

/* --------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
	const char *out_path = (argc > 1) ? argv[1] : "dawn-smoke.png";

	WGPUInstanceFeatureName want_features[] = { WGPUInstanceFeatureName_TimedWaitAny };
	WGPUInstanceDescriptor idesc = {
		.requiredFeatureCount = 1,
		.requiredFeatures = want_features,
	};
	WGPUInstance instance;
	struct adapter_result ares = { 0 };
	struct device_result dres = { 0 };
	struct map_result mres = { 0 };
	WGPUAdapterInfo info = { 0 };
	WGPUDevice device;
	WGPUQueue queue;
	WGPUTexture tex;
	WGPUTextureView view;
	WGPUBuffer readback;
	WGPUCommandEncoder enc;
	WGPURenderPassEncoder pass;
	WGPUCommandBuffer cmd;
	const uint8_t *mapped;
	uint8_t *pixels;
	size_t bytes = (size_t)TEX_W * TEX_H * 4;
	int bad = 0;
	size_t i;

	g_stage = "createInstance";
	instance = wgpuCreateInstance(&idesc);
	if (!instance)
		FAIL("wgpuCreateInstance returned NULL");
	printf("dawn-smoke: instance created\n");

	g_stage = "requestAdapter";
	{
		WGPURequestAdapterOptions opts = {
			.featureLevel = WGPUFeatureLevel_Core,
			.powerPreference = WGPUPowerPreference_HighPerformance,
			.backendType = WGPUBackendType_Vulkan,
		};
		WGPURequestAdapterCallbackInfo cb = {
			.mode = WGPUCallbackMode_WaitAnyOnly,
			.callback = on_adapter,
			.userdata1 = &ares,
		};
		wait_for(instance, wgpuInstanceRequestAdapter(instance, &opts, cb));
	}
	if (!ares.ok || !ares.adapter)
		FAIL("no Vulkan adapter");

	if (wgpuAdapterGetInfo(ares.adapter, &info) == WGPUStatus_Success) {
		printf("dawn-smoke: adapter = %.*s (backend %.*s, type %d)\n",
		       (int)info.device.length, info.device.data,
		       (int)info.description.length, info.description.data,
		       (int)info.adapterType);
	}

	g_stage = "requestDevice";
	{
		WGPUDeviceDescriptor ddesc = {
			.uncapturedErrorCallbackInfo = {
				.callback = on_uncaptured_error,
			},
			.deviceLostCallbackInfo = {
				.mode = WGPUCallbackMode_AllowSpontaneous,
				.callback = on_device_lost,
			},
		};
		WGPURequestDeviceCallbackInfo cb = {
			.mode = WGPUCallbackMode_WaitAnyOnly,
			.callback = on_device,
			.userdata1 = &dres,
		};
		wait_for(instance, wgpuAdapterRequestDevice(ares.adapter, &ddesc, cb));
	}
	if (!dres.ok || !dres.device)
		FAIL("device request failed");
	device = dres.device;
	queue = wgpuDeviceGetQueue(device);
	printf("dawn-smoke: device + queue acquired\n");

	g_stage = "createTexture";
	{
		WGPUTextureDescriptor tdesc = {
			.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc,
			.dimension = WGPUTextureDimension_2D,
			.size = { TEX_W, TEX_H, 1 },
			.format = WGPUTextureFormat_RGBA8Unorm,
			.mipLevelCount = 1,
			.sampleCount = 1,
		};
		tex = wgpuDeviceCreateTexture(device, &tdesc);
	}
	if (!tex)
		FAIL("texture creation returned NULL");
	view = wgpuTextureCreateView(tex, NULL);

	g_stage = "createReadbackBuffer";
	{
		WGPUBufferDescriptor bdesc = {
			.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead,
			.size = (uint64_t)bytes,
		};
		readback = wgpuDeviceCreateBuffer(device, &bdesc);
	}
	if (!readback)
		FAIL("readback buffer creation returned NULL");

	g_stage = "encodeClearPass";
	enc = wgpuDeviceCreateCommandEncoder(device, NULL);
	{
		WGPURenderPassColorAttachment color = {
			.view = view,
			.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
			.loadOp = WGPULoadOp_Clear,
			.storeOp = WGPUStoreOp_Store,
			.clearValue = {
				CLEAR_R / 255.0, CLEAR_G / 255.0,
				CLEAR_B / 255.0, CLEAR_A / 255.0,
			},
		};
		WGPURenderPassDescriptor pdesc = {
			.colorAttachmentCount = 1,
			.colorAttachments = &color,
		};
		pass = wgpuCommandEncoderBeginRenderPass(enc, &pdesc);
		wgpuRenderPassEncoderEnd(pass);
		wgpuRenderPassEncoderRelease(pass);
	}

	g_stage = "copyTextureToBuffer";
	{
		/* TEX_W * 4 == 1024, already a multiple of the 256-byte
		 * bytesPerRow alignment requirement. */
		WGPUTexelCopyTextureInfo src = {
			.texture = tex,
			.mipLevel = 0,
			.origin = { 0, 0, 0 },
			.aspect = WGPUTextureAspect_All,
		};
		WGPUTexelCopyBufferInfo dst = {
			.buffer = readback,
			.layout = {
				.offset = 0,
				.bytesPerRow = TEX_W * 4,
				.rowsPerImage = TEX_H,
			},
		};
		WGPUExtent3D extent = { TEX_W, TEX_H, 1 };
		wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &extent);
	}
	cmd = wgpuCommandEncoderFinish(enc, NULL);
	wgpuQueueSubmit(queue, 1, &cmd);
	wgpuCommandBufferRelease(cmd);
	wgpuCommandEncoderRelease(enc);
	printf("dawn-smoke: clear + copy submitted\n");

	g_stage = "mapReadback";
	{
		WGPUBufferMapCallbackInfo cb = {
			.mode = WGPUCallbackMode_WaitAnyOnly,
			.callback = on_map,
			.userdata1 = &mres,
		};
		wait_for(instance,
			 wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, bytes, cb));
	}
	if (!mres.ok)
		FAIL("buffer map failed");

	mapped = wgpuBufferGetConstMappedRange(readback, 0, bytes);
	if (!mapped)
		FAIL("wgpuBufferGetConstMappedRange returned NULL");

	pixels = malloc(bytes);
	if (!pixels)
		FAIL("out of memory copying pixels");
	memcpy(pixels, mapped, bytes);
	wgpuBufferUnmap(readback);

	g_stage = "verifyPixels";
	for (i = 0; i < bytes; i += 4) {
		if (pixels[i + 0] != CLEAR_R || pixels[i + 1] != CLEAR_G ||
		    pixels[i + 2] != CLEAR_B || pixels[i + 3] != CLEAR_A) {
			if (bad < 4)
				fprintf(stderr,
					"  pixel %zu = (%u,%u,%u,%u), expected (%u,%u,%u,%u)\n",
					i / 4, pixels[i], pixels[i + 1],
					pixels[i + 2], pixels[i + 3],
					CLEAR_R, CLEAR_G, CLEAR_B, CLEAR_A);
			bad++;
		}
	}
	printf("dawn-smoke: readback %dx%d, asked for (%u,%u,%u,%u), "
	       "first pixel is (%u,%u,%u,%u), mismatching pixels: %d/%d\n",
	       TEX_W, TEX_H, CLEAR_R, CLEAR_G, CLEAR_B, CLEAR_A,
	       pixels[0], pixels[1], pixels[2], pixels[3],
	       bad, TEX_W * TEX_H);

	g_stage = "writePNG";
	write_png(out_path, pixels, TEX_W, TEX_H);
	printf("dawn-smoke: wrote %s\n", out_path);

	free(pixels);
	wgpuBufferRelease(readback);
	wgpuTextureViewRelease(view);
	wgpuTextureRelease(tex);
	wgpuQueueRelease(queue);
	wgpuDeviceRelease(device);
	wgpuAdapterRelease(ares.adapter);
	wgpuInstanceRelease(instance);

	if (bad) {
		fprintf(stderr, "dawn-smoke: FAILED — %d pixels wrong\n", bad);
		return 1;
	}
	printf("dawn-smoke: OK\n");
	return 0;
}
