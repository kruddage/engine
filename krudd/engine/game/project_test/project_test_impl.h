/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PROJECT_TEST_IMPL_H
#define PROJECT_TEST_IMPL_H

struct asset_api;

/*
 * The one seam between the harness's two translation units, and the reason
 * there are two.
 *
 * The bring-up order is fixed and it is not the obvious one: the catalog must
 * be seeded after the image exists (it bakes through it) and bound before the
 * project source is evaluated (a top-level *-define! is what puts a project's
 * own assets in it), which puts the catalog steps in the MIDDLE of a sequence
 * the lean half owns. Passing that middle in as a callback is what lets the
 * order live in one place while the code that mentions the catalog lives in a
 * member no lean binary pulls — see the note in project_test.h on why the
 * catalog is optional at the call site rather than at the link site.
 *
 * Internal to the module: not in include/, so nothing outside it can call this
 * or get the order wrong by hand.
 */
void project_test_bring_up(const struct asset_api *(*catalog_up)(void));

#endif /* PROJECT_TEST_IMPL_H */
