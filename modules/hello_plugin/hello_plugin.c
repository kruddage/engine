/* SPDX-License-Identifier: MIT */
#include <stdio.h>

void hello_init(void)
{
	printf("hello_plugin: init\n");
}

void hello_tick(void)
{
}

void hello_shutdown(void)
{
	printf("hello_plugin: shutdown\n");
}
