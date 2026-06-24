/* SPDX-License-Identifier: MIT */
#include "engine.h"

#include "memory.h"

#include <stdint.h>
#include <stdio.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static int32_t frame_count;

void engine_init(void)
{
	mem_init();
	frame_count = 0;
	printf("engine: init\n");
}

void engine_tick(void)
{
	frame_count++;
	if (frame_count % 60 == 0)
		printf("engine: frame %d\n", frame_count);
}

void engine_shutdown(void)
{
	printf("engine: shutdown\n");
	mem_shutdown();
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
