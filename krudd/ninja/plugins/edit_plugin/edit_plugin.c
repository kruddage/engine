/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "edit.h"
#include "edit_api.h"
#include "subsystem_manager.h"

#include <stddef.h>
#include <stdint.h>

/*
 * The "edit" subsystem owns one global history. Other plugins reach it only
 * through the edit_api vtable (subsystem_manager_get_api(mgr, "edit")), never
 * by importing g_hist — the same rule the scene subsystem follows for its
 * world.
 */
static struct edit_history g_hist;

static void edit_push(const struct edit_cmd *cmd)
{
	edit_history_push(&g_hist, cmd);
}

static void edit_begin(const char *label)
{
	edit_history_begin(&g_hist, label);
}

static void edit_commit(void)
{
	edit_history_commit(&g_hist);
}

static void edit_abort(void)
{
	edit_history_abort(&g_hist);
}

static int32_t edit_undo(void)
{
	return edit_history_undo(&g_hist);
}

static int32_t edit_redo(void)
{
	return edit_history_redo(&g_hist);
}

static int32_t edit_can_undo(void)
{
	return edit_history_can_undo(&g_hist);
}

static int32_t edit_can_redo(void)
{
	return edit_history_can_redo(&g_hist);
}

static const char *edit_undo_label(void)
{
	return edit_history_undo_label(&g_hist);
}

static const char *edit_redo_label(void)
{
	return edit_history_redo_label(&g_hist);
}

static void edit_clear(void)
{
	edit_history_clear(&g_hist);
}

static void edit_set_recording(int32_t on)
{
	edit_history_set_recording(&g_hist, on);
}

static const struct edit_api g_edit_api = {
	.push          = edit_push,
	.begin         = edit_begin,
	.commit        = edit_commit,
	.abort         = edit_abort,
	.undo          = edit_undo,
	.redo          = edit_redo,
	.can_undo      = edit_can_undo,
	.can_redo      = edit_can_redo,
	.undo_label    = edit_undo_label,
	.redo_label    = edit_redo_label,
	.clear         = edit_clear,
	.set_recording = edit_set_recording,
};

static void edit_init(void)
{
	edit_history_reset(&g_hist);
}

static void edit_shutdown(void)
{
	/* Free any mementos still held so nothing leaks at teardown. */
	edit_history_clear(&g_hist);
}

static const struct subsystem edit_desc = {
	.name     = "edit",
	.api      = &g_edit_api,
	.init     = edit_init,
	.shutdown = edit_shutdown,
};

#ifdef __EMSCRIPTEN__
void plugin_entry(struct subsystem_manager *mgr)
#else
void edit_plugin_entry(struct subsystem_manager *mgr)
#endif
{
	subsystem_manager_register(mgr, &edit_desc);
}
