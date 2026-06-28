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
};

#endif /* ASSET_CODEC_API_H */
