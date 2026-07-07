/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "subsystem.h"
#include "subsystem_manager.h"

#include <stdio.h>

static void hello_init(void)
{
	printf("hello_plugin: init\n");
}

static void hello_tick(void)
{
}

static void hello_shutdown(void)
{
	printf("hello_plugin: shutdown\n");
}

static const struct subsystem desc = {
	.name     = "hello",
	.init     = hello_init,
	.tick     = hello_tick,
	.shutdown = hello_shutdown,
};

void plugin_entry(struct subsystem_manager *mgr)
{
	subsystem_manager_register(mgr, &desc);
}
