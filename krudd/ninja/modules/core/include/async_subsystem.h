/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASYNC_SUBSYSTEM_H
#define ASYNC_SUBSYSTEM_H

/*
 * Like struct subsystem but with an async init: the manager calls
 * async_init(done, ctx) and the subsystem calls done(ctx) when ready.
 * tick and shutdown are gated on readiness by the manager.
 */
struct async_subsystem {
	const char *name;
	const void *api;
	void (*async_init)(void (*done)(void *ctx), void *ctx);
	void (*tick)(void);
	void (*shutdown)(void);
};

#endif /* ASYNC_SUBSYSTEM_H */
