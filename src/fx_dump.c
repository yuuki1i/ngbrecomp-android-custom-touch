/*
 * fx_dump.c -- Effects > Effect Capture (src/pause_tab.c): the effect draws of a scene recorded to
 * fx-dump.txt in the save folder, one pass per press, so the trails' and the charge glows' colour and
 * strength can be tuned from what the game actually draws rather than guessed
 * (src/d3d/hle.cpp calls ngb_fx_note at each draw).
 *
 * A line per draw: the D3D entry it came through, the stage-0 texture, the blend (enabled, source,
 * destination), the vertex count, the vertex shader's hash and the map piece being drawn. Additive draws
 * (destination ONE) are the trails, the glows and the Ninpo; the rest tells them apart.
 *
 * The pass ends by itself: FX_MAX_LINES or FX_MAX_MS, or Effects > Effect Capture turned off.
 */
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>

void ngb_fx_dump_done(void);   /* src/ingame_menu.c: turns the setting off (a pass is one scene) */
extern volatile int ngb_fx_dump_on;   /* and its cheap mirror, read at every draw */

#define FX_MAX_LINES 200000
#define FX_MAX_MS 10000

static FILE *s_f;
static int s_lines;
static uint32_t s_start;
static char s_path[512];

static void fx_close(void)
{
    if (s_f) {
        fclose(s_f);
        s_f = NULL;
    }
}

void ngb_fx_note(unsigned code, unsigned tex, unsigned blend, unsigned src, unsigned dst, unsigned count,
                 unsigned vs, unsigned node)
{
    if (!ngb_fx_dump_on) {
        fx_close();
        return;
    }
    /* The effects are the see-through and the additive draws; a scene draws thousands of pieces that would
     * bury them. D3DBLEND: destination ONE is the additive -- the trails, the glows, the Ninpo. */
    if (!blend && dst != 2u && src != 5u)
        return;
    if (!s_f) {
        if (!s_path[0]) {
            char *dir = SDL_GetPrefPath("NinjaGaidenBlack", "fx");
            if (!dir)
                return;
            snprintf(s_path, sizeof s_path, "%sfx-dump.txt", dir);
            SDL_free(dir);
        }
        s_f = fopen(s_path, "w");
        s_lines = 0;
        s_start = SDL_GetTicks();
        if (!s_f)
            return;
        fprintf(s_f, "# code tex blend src dst count vs node\n");
    }
    if (s_lines >= FX_MAX_LINES || SDL_GetTicks() - s_start > FX_MAX_MS) {
        fx_close();
        ngb_fx_dump_done();
        return;
    }
    fprintf(s_f, "%08x %08x %u %u %u %u %08x %08x\n", code, tex, blend, src, dst, count, vs, node);
    if (++s_lines % 128 == 0)
        fflush(s_f);
    if (s_lines >= FX_MAX_LINES) {
        fx_close();
        ngb_fx_dump_done();
    }
}
