/* ui_zh.h -- the recomp's own pages in Chinese: the words (ui_zh_text.c, a table) and the face they are
 * drawn with (ui_zh.c, an embedded cut of Noto Sans CJK, SIL Open Font License).
 *
 * The pages' Latin face cannot draw a character above 0x7F, so a word that is Chinese goes whole to
 * ngb_ui_text: its ASCII is drawn from the CJK face too, and the two never mix within one word. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* en's Chinese, or en itself when the table has none (so every call site may just pass its word). */
const char *ngb_ui_zh(const char *en);
/* en's Chinese in a context ("" or e.g. "cat"), or NULL. */
const char *ngb_ui_zh_ctx(const char *ctx, const char *en);
/* Any byte above 0x7F: the word is Chinese already. */
int ngb_ui_wide(const char *s);

/* UTF-8 text in the embedded CJK face: coverage into m (w x h, max), the baseline at (x, base), px as
 * stb_truetype's pixel height of the Latin faces beside it, squeezed sideways by sx. Returns the advance.
 * m NULL: only measures. */
float ngb_ui_text(uint8_t *m, int w, int h, const char *s, float px, float sx, float x, float base);
