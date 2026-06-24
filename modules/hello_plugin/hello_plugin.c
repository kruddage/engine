/* SPDX-License-Identifier: MIT */
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
	"hello", hello_init, hello_tick, hello_shutdown,
};

void plugin_entry(struct subsystem_manager *mgr)
{
	subsystem_manager_register(mgr, &desc);
}
