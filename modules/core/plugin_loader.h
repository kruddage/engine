/* SPDX-License-Identifier: MIT */
#ifndef PLUGIN_LOADER_H
#define PLUGIN_LOADER_H

#include "subsystem_manager.h"

/* Must be called before plugin_loader_init so the loader knows where to
 * register plugins as they finish loading. */
void plugin_loader_set_manager(struct subsystem_manager *mgr);
void plugin_loader_init(void);
void plugin_loader_shutdown(void);

#endif /* PLUGIN_LOADER_H */
