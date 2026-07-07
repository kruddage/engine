/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASSET_CODEC_API_H
#define ASSET_CODEC_API_H

#include <stdint.h>

/*
 * Plugin-facing codec interface.  Obtain via
 * subsystem_manager_get_api(mgr, "asset_codec").
 */
struct asset_codec_api {
	/* Register a decoder for a file extension (without leading dot). */
	void  (*register_codec)(const char *ext,
				void *(*decode)(const void *bytes,
						uint32_t size));
	/*
	 * Extracts the extension from path, retrieves raw bytes via
	 * asset_get, calls the registered decoder, and returns the result.
	 * Returns NULL if no decoder is registered for the extension or
	 * the asset is not yet loaded.  Caller owns the returned pointer.
	 */
	void *(*get_typed)(const char *path);

	/*
	 * Register an encoder (the inverse of a decoder) for an extension.
	 * `encode` serializes a typed object back to a freshly allocated byte
	 * buffer, writing its size to *out_size; the caller owns the buffer.
	 * Attaches to the same ext slot as register_codec, so one codec holds
	 * both directions.  Added for content-addressing (#214/#235): the
	 * branching runtime encodes the live scene to canonical bytes to hash it.
	 */
	void  (*register_encoder)(const char *ext,
				  void *(*encode)(const void *typed,
						  uint32_t *out_size));

	/*
	 * Decode a raw byte range with the codec registered for `ext`, bypassing
	 * the asset lookup that get_typed does — used to rehydrate content
	 * addressed by hash rather than by path.  Returns NULL if no decoder is
	 * registered for ext.  Caller owns the returned pointer.
	 */
	void *(*decode_bytes)(const char *ext, const void *bytes, uint32_t size);

	/*
	 * Encode a typed object to bytes with the encoder registered for `ext`.
	 * Writes the size to *out_size.  Returns NULL if no encoder is registered
	 * for ext.  Caller owns the returned buffer.
	 */
	void *(*encode)(const char *ext, const void *typed, uint32_t *out_size);
};

#endif /* ASSET_CODEC_API_H */
