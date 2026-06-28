/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MD_DRAW_H
#define MD_DRAW_H

/*
 * md_draw — thin ImGui draw shim for parsed markdown blocks.
 *
 * This header is intentionally ImGui-only and does NOT compile on
 * the native (non-Emscripten) build where ImGui is absent.  It must
 * never be included from md_parse.c or md_parse_test.c.
 *
 * -----------------------------------------------------------------
 * How issue #190 should call this
 * -----------------------------------------------------------------
 *
 *   1. Parse the markdown text once (or when the source changes):
 *
 *        static struct md_block g_blocks[MD_BLOCKS_MAX];
 *        static int32_t         g_nblocks;
 *
 *        g_nblocks = md_parse(my_text, g_blocks, MD_BLOCKS_MAX);
 *
 *   2. Each frame (inside an ImGui window), call md_draw_blocks:
 *
 *        md_draw_blocks(g_blocks, g_nblocks);
 *
 *   The draw function is stateless — it just emits ImGui calls and
 *   returns.  Caching the parsed blocks across frames is recommended
 *   to avoid re-parsing every frame.
 *
 *   Fonts
 *   -----
 *   md_draw_blocks uses ImGui::GetIO().Fonts->Fonts[] by index:
 *     index 0 — default body font (paragraph, list items)
 *     index 1 — large/bold font for headings (optional; falls back
 *               to default if only one font is loaded)
 *     index 2 — monospace font for code blocks (optional; same
 *               fallback)
 *   Push the fonts in that order with ImGui::GetIO().Fonts->AddFont*
 *   before calling ImGui::NewFrame() for the first time.  If you
 *   only load the default font, all blocks render in that font.
 * -----------------------------------------------------------------
 */

#ifdef __EMSCRIPTEN__

extern "C" {
#include "md_parse.h"
}

#include "imgui.h"

/*
 * md_draw_blocks — render nblocks parsed markdown blocks as ImGui
 * widgets inside the current ImGui window.
 *
 * Must be called between ImGui::Begin() and ImGui::End().
 */
static inline void md_draw_blocks(const struct md_block *blocks,
				  int32_t nblocks)
{
	ImFont        *font_body = nullptr;
	ImFont        *font_head = nullptr;
	ImFont        *font_mono = nullptr;
	ImFontAtlas   *atlas;
	int32_t        i;
	int32_t        fi;

	atlas = ImGui::GetIO().Fonts;
	if (atlas->Fonts.Size > 0)
		font_body = atlas->Fonts[0];
	if (atlas->Fonts.Size > 1)
		font_head = atlas->Fonts[1];
	if (atlas->Fonts.Size > 2)
		font_mono = atlas->Fonts[2];

	for (i = 0; i < nblocks; i++) {
		const struct md_block *b = &blocks[i];

		switch (b->type) {
		case MD_BLOCK_HEADING: {
			/*
			 * Scale the default font size for h1/h2/h3.
			 * Use font_head if available; fall back to body.
			 */
			ImFont *f   = font_head ? font_head : font_body;
			float   mul = 1.0f;

			if (b->level == 1)
				mul = 1.6f;
			else if (b->level == 2)
				mul = 1.35f;
			else
				mul = 1.15f;

			if (f)
				ImGui::PushFont(f);
			ImGui::SetWindowFontScale(mul);
			ImGui::TextWrapped("%s", b->text);
			ImGui::SetWindowFontScale(1.0f);
			if (f)
				ImGui::PopFont();
			ImGui::Spacing();
			break;
		}
		case MD_BLOCK_LIST_ITEM:
			ImGui::Bullet();
			ImGui::SameLine();
			/*
			 * Emit styled inline spans if present; otherwise
			 * fall through to a plain TextWrapped call.
			 */
			if (b->span_count > 0) {
				uint32_t pos = 0;
				uint32_t len = (uint32_t)strlen(b->text);

				for (fi = 0; fi < (int32_t)b->span_count;
				     fi++) {
					const struct md_span *sp =
						&b->spans[fi];
					char buf[MD_TEXT_MAX];

					/* Plain text before this span. */
					if (sp->start > pos) {
						uint32_t n = sp->start - pos;

						if (n >= MD_TEXT_MAX)
							n = MD_TEXT_MAX - 1;
						memcpy(buf, b->text + pos, n);
						buf[n] = '\0';
						ImGui::TextUnformatted(buf);
						ImGui::SameLine(0.0f, 0.0f);
					}

					/* Styled span. */
					{
						uint32_t n = sp->end - sp->start;

						if (n >= MD_TEXT_MAX)
							n = MD_TEXT_MAX - 1;
						memcpy(buf,
						       b->text + sp->start, n);
						buf[n] = '\0';
					}

					if (sp->style & MD_SPAN_CODE) {
						ImFont *mf = font_mono
							? font_mono
							: font_body;
						if (mf)
							ImGui::PushFont(mf);
						ImGui::TextUnformatted(buf);
						if (mf)
							ImGui::PopFont();
					} else if (sp->style & MD_SPAN_BOLD) {
						ImGui::TextUnformatted(buf);
					} else {
						ImGui::TextUnformatted(buf);
					}
					ImGui::SameLine(0.0f, 0.0f);
					pos = sp->end;
				}

				/* Trailing plain text. */
				if (pos < len) {
					ImGui::TextUnformatted(
						b->text + pos);
				} else {
					ImGui::NewLine();
				}
			} else {
				ImGui::TextWrapped("%s", b->text);
			}
			break;

		case MD_BLOCK_CODE: {
			/*
			 * Code blocks: monospace font, subtle background.
			 * Each line is a separate MD_BLOCK_CODE entry.
			 */
			ImFont *mf = font_mono ? font_mono : font_body;

			ImGui::PushStyleColor(
				ImGuiCol_ChildBg,
				ImVec4(0.12f, 0.12f, 0.12f, 1.0f));
			if (mf)
				ImGui::PushFont(mf);
			ImGui::TextUnformatted(b->text);
			if (mf)
				ImGui::PopFont();
			ImGui::PopStyleColor();
			break;
		}
		default: /* MD_BLOCK_PARAGRAPH */
			if (b->span_count > 0) {
				uint32_t pos = 0;
				uint32_t len = (uint32_t)strlen(b->text);
				int      need_newline = 1;

				for (fi = 0; fi < (int32_t)b->span_count;
				     fi++) {
					const struct md_span *sp =
						&b->spans[fi];
					char buf[MD_TEXT_MAX];

					/* Plain prefix. */
					if (sp->start > pos) {
						uint32_t n = sp->start - pos;

						if (n >= MD_TEXT_MAX)
							n = MD_TEXT_MAX - 1;
						memcpy(buf, b->text + pos, n);
						buf[n] = '\0';
						ImGui::TextUnformatted(buf);
						ImGui::SameLine(0.0f, 0.0f);
					}

					/* Styled content. */
					{
						uint32_t n = sp->end - sp->start;

						if (n >= MD_TEXT_MAX)
							n = MD_TEXT_MAX - 1;
						memcpy(buf,
						       b->text + sp->start, n);
						buf[n] = '\0';
					}

					if (sp->style & MD_SPAN_CODE) {
						ImFont *mf = font_mono
							? font_mono
							: font_body;
						if (mf)
							ImGui::PushFont(mf);
						ImGui::TextUnformatted(buf);
						if (mf)
							ImGui::PopFont();
					} else {
						ImGui::TextUnformatted(buf);
					}
					ImGui::SameLine(0.0f, 0.0f);
					pos = sp->end;
					need_newline = 1;
				}

				/* Trailing plain text. */
				if (pos < len) {
					ImGui::TextWrapped(
						"%s", b->text + pos);
				} else if (need_newline) {
					ImGui::NewLine();
				}
			} else {
				ImGui::TextWrapped("%s", b->text);
			}
			ImGui::Spacing();
			break;
		}
	}
}

#endif /* __EMSCRIPTEN__ */
#endif /* MD_DRAW_H */
