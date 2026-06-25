/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log.h"

static void hello_init(void)
{
	LOG_INFO("hello_plugin: init");
}

static void hello_tick(void)
{
}

static void hello_shutdown(void)
{
	LOG_INFO("hello_plugin: shutdown");
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
