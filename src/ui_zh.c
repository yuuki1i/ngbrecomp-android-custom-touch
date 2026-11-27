/*
 * ui_zh.c -- the face the Chinese pages are drawn with: Noto Sans CJK SC (Black), cut to the characters
 * this program's own words use (assets/fonts/NgbUiZh.ttf, embedded by CMake as ngb_ui_font).
 *
 * The game's Latin face sets a capital at px tall; a hanzi at the same number would come out small, so the
 * em is scaled by EM_K as src/hurricane_zh.c does for the game's own Chinese.
 */
#include <math.h>
#include <string.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "ui_zh.h"

extern const unsigned char ngb_ui_font[];

#define EM_K 0.86f

static stbtt_fontinfo s_f;
static int s_ok = -1;

static int ready(void)
{
    if (s_ok < 0)
        s_ok = stbtt_InitFont(&s_f, ngb_ui_font, stbtt_GetFontOffsetForIndex(ngb_ui_font, 0)) ? 1 : 0;
    return s_ok;
}

static int utf8_next(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    int c = s[0];
    if (c < 0x80) {
        *ps += 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *ps += 2;
        return ((c & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *ps += 3;
        return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        int cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        *ps += 4;
        return cp;
    }
    *ps += 1;                       /* (a stray byte: skipped, never a crash) */
    return 0;
}

float ngb_ui_text(uint8_t *m, int w, int h, const char *s, float px, float sx, float x, float base)
{
    if (!s || !*s || !ready())
        return 0;
    float scale = stbtt_ScaleForMappingEmToPixels(&s_f, px * EM_K), x0 = x;
    static uint8_t g[256 * 256];
    while (*s) {
        int cp = utf8_next(&s);
        if (!cp)
            continue;
        int adv = 0, lsb = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        stbtt_GetCodepointHMetrics(&s_f, cp, &adv, &lsb);
        float fx = x - floorf(x);
        if (m) {
            stbtt_GetCodepointBitmapBoxSubpixel(&s_f, cp, scale * sx, scale, fx, 0, &x1, &y1, &x2, &y2);
            int gw = x2 - x1, gh = y2 - y1;
            if (gw > 0 && gh > 0 && gw <= 256 && gh <= 256) {
                stbtt_MakeCodepointBitmapSubpixel(&s_f, g, gw, gh, gw, scale * sx, scale, fx, 0, cp);
                for (int yy = 0; yy < gh; yy++)
                    for (int xx = 0; xx < gw; xx++) {
                        int X = (int)floorf(x) + x1 + xx, Y = (int)lroundf(base) + y1 + yy;
                        if (X >= 0 && Y >= 0 && X < w && Y < h && g[yy * gw + xx] > m[Y * w + X])
                            m[Y * w + X] = g[yy * gw + xx];
                    }
            }
        }
        x += adv * scale * sx;
    }
    return x - x0;
}

int ngb_ui_wide(const char *s)
{
    for (; s && *s; s++)
        if ((unsigned char)*s >= 0x80)
            return 1;
    return 0;
}
