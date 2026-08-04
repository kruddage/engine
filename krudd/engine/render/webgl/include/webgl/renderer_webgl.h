/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RENDERER_WEBGL_H
#define RENDERER_WEBGL_H

#include <stdint.h>

/*
 * The backbuffer a "backbuffer" render pass (one whose colour attachment is
 * NULL — see fg_import_backbuffer) draws into is not always the canvas's own
 * framebuffer. A host that composites this canvas into something else of its
 * own — a stereo, per-eye render target shared by two views and distinguished
 * only by a rect within it, say — hands the module a framebuffer it neither
 * created nor may introspect: an external, opaque target this module is only
 * told the name and rect of.
 *
 * webgl_declare_backbuffer records that target for every backbuffer pass to
 * follow, until webgl_clear_backbuffer (or another declare) replaces it.
 * Declaring nothing — the state at startup, and after webgl_clear_backbuffer
 * — means "the canvas's own framebuffer": FBO 0 at the full drawing-buffer
 * size, queried fresh each pass. That is this backend's entire behaviour from
 * before this file existed, and it is what every caller that never declares a
 * backbuffer keeps, unchanged.
 *
 * fbo names an existing framebuffer object this module does not own: it must
 * never be probed with glCheckFramebufferStatus (an opaque target rejects
 * that call), deleted, or have its attachments reconfigured. x, y, width,
 * height are a rect within it, in the same bottom-left-origin framebuffer-
 * pixel space cmd_set_scissor uses. Two declarations naming the same fbo with
 * different rects (two eyes sharing one target) each get their own viewport
 * and their own scissored clear, so neither pass's clear wipes the other's
 * already-drawn half.
 */
void webgl_declare_backbuffer(uint32_t fbo, int32_t x, int32_t y,
			       uint32_t width, uint32_t height);

/*
 * Clear a prior declaration. Every backbuffer pass from here on reverts to
 * FBO 0 at the full drawing-buffer size, as if webgl_declare_backbuffer had
 * never been called.
 */
void webgl_clear_backbuffer(void);

/*
 * The declaration currently in effect — declared 0 and every other field 0
 * when webgl_declare_backbuffer has not been called (or has been undone by
 * webgl_clear_backbuffer). Test/inspection support: renderer_webgl.c's own
 * render-pass logic reads its private copy of this state directly.
 */
struct webgl_backbuffer_decl {
	int      declared;
	uint32_t fbo;
	int32_t  x;
	int32_t  y;
	uint32_t width;
	uint32_t height;
};

struct webgl_backbuffer_decl webgl_backbuffer_declared(void);

#endif /* RENDERER_WEBGL_H */
