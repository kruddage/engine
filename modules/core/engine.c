/* SPDX-License-Identifier: MIT */
#include "engine.h"
#include "subsystem.h"

#include "log.h"
#include "memory.h"

#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static const struct subsystem subsystems[] = {
	{ "log",    log_init,  NULL, log_shutdown  },
	{ "memory", mem_init,  NULL, mem_shutdown  },
	{ NULL }
};

static int32_t frame_count;

void engine_init(void)
{
	subsystem_init_all(subsystems);
	frame_count = 0;
	LOG_INFO("engine: init");
}

void engine_tick(void)
{
	frame_count++;
	if (frame_count % 60 == 0)
		LOG_DEBUG("engine: frame %d", frame_count);
	subsystem_tick_all(subsystems);
}

void engine_shutdown(void)
{
	LOG_INFO("engine: shutdown");
	subsystem_shutdown_all(subsystems);
}

int main(void)
{
	engine_init();
#ifdef __EMSCRIPTEN__
	emscripten_set_main_loop(engine_tick, 0, 1);
#endif
	engine_shutdown();
	return 0;
}
