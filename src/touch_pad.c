/*
 * Touch controls (Android): an original Xbox pad laid over the screen -- the two sticks, the d-pad, A B X Y in
 * their colours, Black and White, the triggers, Back and Start and the stick clicks -- read from the screen's touches and
 * merged into the pad state the keyboard gives (src/keyboard.c, xbox_keyboard_map), which the game reads while
 * no controller is connected. With a controller connected they are hidden and their touches ignored.
 *
 * The picture is made here, at the window's own size (half its size, enlarged by the presenter, blurred every
 * piece: #284; NGB_TOUCH_HALF=1 makes it at half again, to compare), premultiplied BGRA, again whenever something
 * on it changes; the presenter blends it over each frame (xboxrecomp's present_sdl.c, nv2a_overlay_picture). The pieces
 * are made from the game's own button art (src/game_prompts.c: the game's gems, plates and Black and White
 * buttons at touch size, a d-pad drawn in the look of its icon, the sticks' wells; each held piece a picture of
 * its own), and saved in the save folder (<save folder>/touch-art), so a later start shows them from its first
 * frame. On a first start nothing is drawn until they are made (a second or so after the game's textures load).
 * Should they not come -- the game's textures not read after STANDIN_AFTER_MS of frames, or the arcade, which has no game_prompts.c --
 * stand-ins so a phone without a controller still has its controls: Kenney's Mobile Controls (CC0: assets/touch,
 * embedded by CMakeLists.txt) for the sticks and d-pad, the buttons dark discs with their letters in Noto Sans
 * (OFL: launcher_font.h), held: a gold ring round them. Half see-through untouched.
 *
 * Places are in screen heights from the left or right edge or the middle (a tall screen keeps its controls in
 * the corners; a less wide one draws the d-pad and right stick in towards the middle). The face buttons sit in
 * two offset columns with Black and White above them and LT (guard) beside them; RT (centre the camera) small
 * at the top left; Back and Start at the bottom middle.
 *
 * Threads: the touches arrive on Android's UI thread (SDL's event watch, run as they are sent); the pad state
 * is read wherever the input is polled; the picture is made by whichever thread presents (the PFIFO thread or
 * the renderer's), one at a time under the presenter's lock. s_lock keeps the state short-held: the picture is
 * drawn from a copy, outside it.
 * NGB_TOUCH_LOG=1 logs each control a finger lands on.
 */
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <SDL.h>
#if defined(__ANDROID__)
#include <SDL_system.h>
#include <jni.h>
#endif

#include "platform/xbox_winnt.h"
#include "input/xinput_xbox.h"
#include "bindings.h"
#include "game_prompts.h"

int xbox_InputControllerPresent(void);
void xbox_InputTouchUsed(void);   /* xboxrecomp input/xinput_device.c */
unsigned char *stbi_load_from_memory(const unsigned char *buf, int len, int *x, int *y, int *comp, int req);
void stbi_image_free(void *p);
extern const struct { const char *name; const unsigned char *data; const unsigned int *len; } ngb_touch_pngs[];
extern const int ngb_touch_png_count;   /* (CMakeLists.txt: the stand-in pieces by file name) */
#include "launcher_font.h"
#include "ui_zh.h"              /* (the stand-in buttons' letters) */
/* (weak: the arcade programs, built with this file, have no src/game_prompts.c) */
__attribute__((weak)) const NgbPic *ngb_gp_touch(const char *piece);

enum Kind { BUTTON, COMBO, STICK, DPAD };
enum Id { LSTICK, RSTICK, DPAD_ID, BA, BB, BX, BY, LB, RB, LT, RT, BACK, START, LS, RS, SWAP, NINPO, WINDRUN, CUSTOM1, CUSTOM2, CUSTOM3, CUSTOM4, OPACITY, NCTRL };

typedef struct {
    enum Kind kind;
    int edge;            /* x measured from: 0 the left edge, 1 the right, 2 the middle */
    float x, y, r;       /* centre and radius, in screen heights */
    const char *name;    /* (the log's) */
    const char *label;   /* a stand-in button's letters, and their colour */
    uint32_t colour;
    const char *piece;   /* a button's piece made from the game's art (src/game_prompts.c) */
} Ctrl;

static const Ctrl s_ctrl[NCTRL] = {
    [LSTICK]  = { STICK,  0, 0.27f,  0.56f,  0.14f,  "left stick", NULL, 0, NULL },
    [RSTICK]  = { STICK,  1, 0.720f, 0.780f, 0.100f,  "right stick", NULL, 0, NULL },   /* (below and inside the
                                                       face buttons, clear of the game's prompts) */
    [DPAD_ID] = { DPAD,   0, 0.37f,  0.84f,  0.12f,  "d-pad", NULL, 0, NULL },   /* (0.09 was small: #284) */
    [BA]      = { BUTTON, 1, 0.315f, 0.720f, 0.066f, "A", "A", 0xD8DDE4, "button_a" },   /* (the face buttons one
                                                       diamond, one size: Y high, B out, A low, X in -- the
                                                       plates of Winlator's pad) */
    [BB]      = { BUTTON, 1, 0.175f, 0.580f, 0.066f, "B", "B", 0xD8DDE4, "button_b" },
    [BX]      = { BUTTON, 1, 0.455f, 0.580f, 0.066f, "X", "X", 0xD8DDE4, "button_x" },
    [BY]      = { BUTTON, 1, 0.315f, 0.440f, 0.066f, "Y", "Y", 0xD8DDE4, "button_y" },
    [LB]      = { BUTTON, 1, 0.440f, 0.290f, 0.042f, "LB", "LB", 0xE6E6E6, "button_black" },
    [RB]      = { BUTTON, 1, 0.185f, 0.290f, 0.042f, "RB", "RB", 0xE6E6E6, "button_white" },
    [LT]      = { BUTTON, 1, 0.560f, 0.180f, 0.050f, "LT", "LT", 0xE6E6E6, "trigger_l" },   /* (guard: by the face
                                                       buttons) */
    [RT]      = { BUTTON, 0, 0.060f, 0.088f, 0.047f, "RT", "RT", 0xE6E6E6, "trigger_r" },   /* (the camera: small,
                                                       in the corner clear of the health emblem:
                                                       place_at) */
    [BACK]    = { BUTTON, 2, -0.075f, 0.925f, 0.050f, "BACK", "BACK", 0xE6E6E6, "back" },   /* (Back and Start off the */
    [START]   = { BUTTON, 2, 0.075f, 0.925f,  0.050f, "START", "START", 0xE6E6E6, "start" },   /* gesture bar; larger and
                                                       nearer each other, both pressed at once for a reset: #284) */
    [LS]      = { BUTTON, 0, 0.09f,  0.90f,  0.04f,  "LS", "LS", 0xE6E6E6, "click_l" },
    [RS]      = { BUTTON, 1, 0.09f,  0.90f,  0.045f, "RS", "RS", 0xE6E6E6, "click_r" },
    [SWAP]    = { BUTTON, 0, 0.05f,  0.36f,  0.035f, "layout", "<>", 0xE6E6E6, "swap" },
    [NINPO]   = { COMBO, 1, 0.52f, 0.46f, 0.060f, "NINPO", "N", 0xD8DDE4, "button_y" },
    [WINDRUN] = { COMBO, 1, 0.52f, 0.62f, 0.060f, "WIND RUN", "W", 0xD8DDE4, "button_x" },
    [CUSTOM1] = { COMBO, 1, 0.300f, 0.865f, 0.055f, "C1", "C1", 0xD8DDE4, "button_a" },   /* (under the face
                                                       buttons: where the thumb already is) */
    [CUSTOM2] = { COMBO, 1, 0.52f, 0.44f, 0.055f, "C2", "C2", 0xD8DDE4, "button_b" },
    [CUSTOM3] = { COMBO, 1, 0.52f, 0.56f, 0.055f, "C3", "C3", 0x4C9BEA, "button_x" },
    [CUSTOM4] = { COMBO, 1, 0.52f, 0.68f, 0.055f, "C4", "C4", 0x4C9BEA, "button_y" },
    [OPACITY] = { BUTTON, 0, 0.05f,  0.46f,  0.035f, "opacity", "%", 0xE6E6E6, "opacity" },   /* (upstream 1.0.3:
                                                       a step less solid each tap, under the switch) */
};

/* Classic: the places of 0.9.x's controls, in the same pictures (#285: Block held by a left finger at the top
 * left, the camera's RT at the top right with the camera toggle (RS) under it, the left stick low and large where
 * the thumb rests, the face buttons lower). Start and Back stay in the middle (#284). The switch beside the left
 * stick, at the left edge, changes between this and the default, kept in settings.ini (touch_layout). After 1.0.2
 * (#285 again): the left stick further in from the left edge (a roll left while blocking; the d-pad stays as near
 * Back as it can), the face buttons lower within the thumb's reach, Block larger and further in (the health bar
 * shows through it: the opacity button), the Black button beside it moved out of its way. Classic is the default
 * from then on. */
static const struct { int edge; float x, y, r; } s_classic[NCTRL] = {
    [LSTICK]  = { 0, 0.34f, 0.70f, 0.17f },
    [RSTICK]  = { 1, 0.72f, 0.66f, 0.12f },
    [DPAD_ID] = { 0, 0.74f, 0.84f, 0.12f },
    [BA]      = { 1, 0.28f, 0.78f, 0.075f },
    [BB]      = { 1, 0.16f, 0.66f, 0.07f },
    [BX]      = { 1, 0.40f, 0.66f, 0.075f },
    [BY]      = { 1, 0.28f, 0.54f, 0.075f },
    [LB]      = { 0, 0.33f, 0.30f, 0.05f },
    [RB]      = { 1, 0.30f, 0.28f, 0.05f },
    [LT]      = { 0, 0.19f, 0.15f, 0.10f },
    [RT]      = { 1, 0.13f, 0.14f, 0.085f },
    [BACK]    = { 2, -0.085f, 0.89f, 0.055f },
    [START]   = { 2, 0.085f, 0.89f, 0.055f },
    [LS]      = { 0, 0.09f, 0.90f, 0.04f },
    [RS]      = { 1, 0.09f, 0.30f, 0.045f },
    [SWAP]    = { 0, 0.05f, 0.41f, 0.035f },
    [OPACITY] = { 0, 0.05f, 0.52f, 0.035f },   /* (under the switch, clear of Block; a near miss of it not the
                                                 * switch, which lets go of all) */
};
/* Where a texture pack goes: <sd card>/NGB/textures when the phone has a card the app can write, else the
 * app's own files (src/d3d/gpu/texture.cpp dumps a texture there the first time it is drawn, and paints with
 * whatever PNG of the same name is found). */
static char s_tex_dir[512];

void ngb_texture_dir_forget(void) { s_tex_dir[0] = 0; }

const char *ngb_texture_dir(void)
{
    char *dir = s_tex_dir;
    if (dir[0]) return dir;
#if defined(__ANDROID__)
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = SDL_AndroidGetActivity();
    if (env && activity) {
        jclass cls = (*env)->GetObjectClass(env, activity);
        jmethodID method = cls ? (*env)->GetMethodID(env, cls, "sdCardFolder", "()Ljava/lang/String;") : NULL;
        jstring card = method ? (jstring)(*env)->CallObjectMethod(env, activity, method) : NULL;
        if (card) {
            const char *utf = (*env)->GetStringUTFChars(env, card, NULL);
            if (utf) {
                snprintf(dir, sizeof s_tex_dir, "%s/NGB/textures", utf);
                (*env)->ReleaseStringUTFChars(env, card, utf);
            }
            (*env)->DeleteLocalRef(env, card);
        }
        if (cls) (*env)->DeleteLocalRef(env, cls);
    }
    if (!dir[0]) {
        char *pref = SDL_GetPrefPath("NinjaGaidenBlack", "textures");
        if (pref) {
            snprintf(dir, sizeof s_tex_dir, "%s", pref);
            SDL_free(pref);
        }
    }
#endif
    if (!dir[0]) return NULL;
    /* both levels of the folder, then ready */
    for (char *slash = dir + 1; *slash; slash++) {
        if (*slash != '/' && *slash != '\\') continue;
        *slash = 0;
        mkdir(dir, 0777);
        *slash = '/';
    }
    return dir;
}

int ngb_pc_setting(int which);                    /* src/ingame_menu.c (48: the touch layout) */
void ngb_touch_editor_open(void);   /* the Android touch editor (NgbActivity), src/touch_pad.c below */
void ngb_settings_open(void);       /* the Android settings page: 画面 and 按键 */
void ngb_pc_set_setting(int which, int value);
int ngb_fx_trail(void);                            /* src/ingame_menu.c: 50 the trail and the weapon's ghost, 0..200 */
extern volatile int ngb_fx_hits;                   /* src/fx_dump.c: how many draws the trail's pass took */
typedef struct { unsigned key; int n; } NgFxBucket;  /* src/d3d/vk/backend.cpp: the frame's kinds of blended draw */
extern NgFxBucket ngb_fx_last[24];
extern int ngb_fx_last_n;
int ngb_fx_tint(int which);                        /* src/ingame_menu.c: the trail's r, g, b, alpha, glow */
int ngb_fx_on(void);                               /* src/ingame_menu.c: the trails' own switch */
void ngb_fx_tint_set(int which, int value);
int ngb_fx_dump(void);                             /* src/ingame_menu.c: 51 what the game draws, written out */
enum { NGB_SET_FXTRAIL = 50, NGB_SET_FXDUMP = 51 };   /* (src/pause_tab.c: the two settings' numbers) */
#pragma weak ngb_pc_setting
#pragma weak ngb_pc_set_setting
static int s_layout = -1, s_layout_save;          /* 0 the default, 1 Classic (under s_lock); to be saved */
static int s_opacity = -1, s_opacity_save;        /* the controls' opacity, percent (setting 49: under s_lock);
                                                     * to be saved */
static unsigned s_serial = 1;
/* (under s_lock, on the game's side: touch_init) the layout, read from the settings; the controls are drawn only
 * once it is known, so never in one layout and found in the other */
static void layout_known(void)
{
    if (s_layout < 0) {
        s_layout = !ngb_pc_setting || ngb_pc_setting(48) == 1;   /* (the arcade, no settings: Classic, the default) */
        s_serial++;
    }
    if (s_opacity < 0) {
        int v = ngb_pc_setting ? ngb_pc_setting(49) : 100;
        s_opacity = v >= 30 && v <= 100 ? v : 100;
        s_serial++;
    }
}

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static float s_aspect = 16.0f / 9;
static float s_layout_x[NCTRL], s_layout_y[NCTRL], s_layout_r[NCTRL], s_layout_alpha[NCTRL];
static float d_layout_x[NCTRL], d_layout_y[NCTRL], d_layout_r[NCTRL], d_layout_alpha[NCTRL];
/* Renderer-side eased positions: touch edits update targets immediately, while the picture follows smoothly. */
/* Picture: brightness, gamma and tint, kept with the pad's layout file and laid on the title's own gamma ramp
 * (src/d3d/hle.cpp). 1, 1, 0 is the picture as the title has it. */
float ngb_video_brightness = 1.f, ngb_video_gamma = 1.f;
int ngb_video_tint;
/* A filter the player brings: one PNG, a gradient across, read as the picture's colour curve (red, green
 * and blue down it), and mixed with the brightness. Like the colour filters of a PSP's plugins. */
unsigned char ngb_video_curve[3][256];
int ngb_video_curve_on;
int ngb_texture_dump_on;   /* 0: the pack is read, not written (the writes stalled the opening CG) */
float ngb_video_contrast = 1.f, ngb_video_saturation = 1.f;   /* the picture's own grading, 1 = as the title */
unsigned char *ngb_video_lut_rgba;                           /* a .cube the player brought: N*N by N tiles */
int ngb_video_lut_size;
int ngb_video_lut_serial;
static char s_filter_path[512];

static void filter_curve_build(void)
{
    ngb_video_curve_on = 0;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++) ngb_video_curve[c][i] = (unsigned char)i;
    if (!s_filter_path[0]) return;
    FILE *file = fopen(s_filter_path, "rb");
    if (!file) return;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    unsigned char *bytes = size >= 8 && size <= 32 * 1024 * 1024 ? malloc((size_t)size) : NULL;
    int w = 0, h = 0, channels = 0;
    unsigned char *px = NULL;
    if (bytes && fread(bytes, 1, (size_t)size, file) == (size_t)size)
        px = stbi_load_from_memory(bytes, (int)size, &w, &h, &channels, 4);
    free(bytes);
    fclose(file);
    if (!px || w < 2 || h < 1) { if (px) stbi_image_free(px); return; }
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++) {
            int x = (int)((long)i * (w - 1) / 255);
            int y = h / 2;
            ngb_video_curve[c][i] = px[(y * w + x) * 4 + c];
        }
    stbi_image_free(px);
    ngb_video_curve_on = 1;
}

/* A .cube (the 3D LUT Photoshop, Resolve and the console emulators' filters are written in): N*N*N triples, red
 * varying fastest, laid out for a 2D texture N tiles of N x N across (x = b*N + r, y = g). */
static void filter_cube_build(void)
{
    free(ngb_video_lut_rgba);
    ngb_video_lut_rgba = NULL;
    ngb_video_lut_size = 0;
    if (!s_filter_path[0]) { ngb_video_lut_serial++; return; }
    const char *dot = strrchr(s_filter_path, '.');
    char ext[8] = "";
    if (dot) snprintf(ext, sizeof ext, "%s", dot);
    for (char *q = ext; *q; q++) *q = (char)tolower((unsigned char)*q);
    if (strcmp(ext, ".cube")) { ngb_video_lut_serial++; return; }
    FILE *file = fopen(s_filter_path, "rb");
    if (!file) { ngb_video_lut_serial++; return; }
    char line[256];
    int n = 0, got = 0;
    unsigned char *data = NULL;
    while (fgets(line, sizeof line, file)) {
        if (line[0] == '#') continue;
        if (!n && strncmp(line, "LUT_3D_SIZE", 11) == 0) {
            n = atoi(line + 11);
            if (n < 2 || n > 64) { n = 0; break; }
            data = calloc((size_t)n * n * n, 4);
            if (!data) { n = 0; break; }
            continue;
        }
        if (!n) continue;
        float r, g, b;
        if (sscanf(line, "%f %f %f", &r, &g, &b) != 3) continue;
        if (got >= n * n * n) break;
        int ri = got % n, gi = (got / n) % n, bi = got / (n * n);
        size_t px = ((size_t)gi * n*n + (size_t)bi * n + (size_t)ri) * 4;
        data[px + 0] = (unsigned char)(fminf(fmaxf(b, 0.f), 1.f) * 255.f);
        data[px + 1] = (unsigned char)(fminf(fmaxf(g, 0.f), 1.f) * 255.f);
        data[px + 2] = (unsigned char)(fminf(fmaxf(r, 0.f), 1.f) * 255.f);
        data[px + 3] = 255;
        got++;
    }
    fclose(file);
    if (data && got == n * n * n) {
        ngb_video_lut_rgba = data;
        ngb_video_lut_size = n;
    } else
        free(data);
    ngb_video_lut_serial++;
    if (ngb_video_lut_size) ngb_video_curve_on = 0;   /* (a cube beats a curve: one filter at a time) */
}
void ngb_video_filter_refresh(void);   /* src/d3d/hle.cpp (nothing on the other platforms) */
void ngb_textures_rescan(void);        /* src/d3d/gpu/texture.cpp: forget which textures were dumped */
int ngb_textures_dumped(void);         /* src/d3d/gpu/texture.cpp: how many have been written so far */

static float d_anim_x[NCTRL], d_anim_y[NCTRL], d_anim_r[NCTRL];
static bool d_anim_ready;
static bool s_layout_custom[NCTRL], d_layout_custom[NCTRL], s_layout_loaded, s_edit_mode, d_edit_mode;
static float s_layout_asp[NCTRL], d_layout_asp[NCTRL];   /* how much wider than tall a key is drawn */
/* The pad's look, the same on every key and in the editor: 0 the hairline (no plate, a thin ring, the name),
 * 1 the glass (a dark plate under the name). (user 2026-11-27: all the keys in one of two styles, not each its
 * own.) */
static uint8_t s_pad_style;
/* Which console's pad the keys are drawn as: 0 Xbox (A B X Y where they are), 1 Switch (A at the right, B at
 * the bottom, L / R / ZL / ZR, - and +), 2 PlayStation (its four marks, L1 / R1 / L2 / R2, SHARE / OPT).
 * Held in the layout, like the style (user 2026-11-27: three glass themes, every key replaced). */
static int s_pad_theme;
/* Auto-fire: a key held repeats by itself, this many presses a second (0: held, as usual). In the layout as
 * "# TURBO <id> <hz>" lines, so the file still reads as V5 rows to everything else. */
static uint8_t s_turbo[NCTRL];
static int theme_action(int id, int act);   /* (drawn and sent further down) */
static uint8_t s_layout_color[NCTRL], d_layout_color[NCTRL], s_layout_shape[NCTRL], d_layout_shape[NCTRL], s_layout_skin[NCTRL], d_layout_skin[NCTRL], s_layout_combo[NCTRL], d_layout_combo[NCTRL], s_layout_enabled[NCTRL], d_layout_enabled[NCTRL];
/* The pad button a virtual button presses (PA_* below; PA_NONE: the one it was drawn for). Editable, so a
 * player can put Attack on the right-hand side and Guard where the thumb rests, as the phone suits them. */
enum { PA_A, PA_B, PA_X, PA_Y, PA_BLACK, PA_WHITE, PA_LT, PA_RT, PA_BACK, PA_START, PA_LS, PA_RS, PA_COUNT, PA_NONE = -1 };

/* What each virtual button presses of its own: its own place on the pad, before anything was rebound. */
static const int own[NCTRL] = {
    [BA]=PA_A, [BB]=PA_B, [BX]=PA_X, [BY]=PA_Y, [LB]=PA_BLACK, [RB]=PA_WHITE,
    [LT]=PA_LT, [RT]=PA_RT, [BACK]=PA_BACK, [START]=PA_START, [LS]=PA_LS, [RS]=PA_RS,
    [SWAP]=PA_NONE,
};
static int s_layout_action[NCTRL];
static Uint32 s_combo_started[NCTRL], s_combo_y_until[NCTRL];
static bool s_combo_active[NCTRL];
static bool s_haptics = true;
static bool d_haptics = true;
static bool s_camera_swipe = true;
static bool s_cam_hold;                 /* a finger on the empty screen turns the camera */
static float s_cam_px, s_cam_py;        /* how far that finger went since the last frame, screen heights */
static char s_skin_path[NCTRL][256], d_skin_path[NCTRL][256];   /* one picture per control */
static int s_selected = -1, d_selected = -1;
static int s_preset, d_preset;
static unsigned s_layout_revision, d_layout_revision;
static int s_menu_dim, d_menu_dim;   /* (a menu is up: the pad is drawn faint -- it still works) */
static char s_layout_path[512];
static bool s_down[NCTRL];
static float s_stick[2][2];                   /* each stick's knob, -1..1 (y down) */
static int s_dpad;                            /* XBOX_GAMEPAD_DPAD_* bits */
/* s_serial: +1 each change of what the picture shows (declared with the layout, above) */
static int s_art_shown;                       /* the controls drawn (the last picture's; any thread reads it) */

#define MAX_FINGERS 10
static struct { SDL_FingerID id; Uint32 down_timestamp; int ctrl; bool used, moved, was_selected; float x0, y0; } s_finger[MAX_FINGERS];
static void layout_defaults(void) {
    for (int i=0;i<NCTRL;i++) { s_layout_x[i]=s_ctrl[i].x; s_layout_y[i]=s_ctrl[i].y; s_layout_r[i]=s_ctrl[i].r; s_layout_asp[i]=1.f; s_layout_alpha[i]=0.58f; s_layout_custom[i]=false; s_layout_color[i]=s_layout_skin[i]=s_layout_combo[i]=0; s_layout_shape[i]= (s_ctrl[i].kind==BUTTON || s_ctrl[i].kind==COMBO) ? 6 : 0;   /* (plain plates: a dark see-through rectangle and a hairline, the name inside -- no art behind) */ s_layout_enabled[i]= i<=START || i==SWAP || i==OPACITY;   /* (1.0.3: the opacity button shows by default) */   /* (the pad as the hand wants it: no stick clicks, combos or badges unless the editor adds them) */ s_layout_action[i]= s_ctrl[i].kind==BUTTON ? own[i] : PA_NONE; }
    /* The combos the pad starts with: NINPO and WIND RUN (user 2026-11-27: the 360 sweep and its Y are gone). */
    s_layout_combo[NINPO]=4; s_layout_combo[WINDRUN]=1;
}
static void layout_load(void) {
    if(s_layout_loaded)return; s_layout_loaded=true; layout_defaults(); s_haptics=true; s_camera_swipe=true; memset(s_skin_path, 0, sizeof s_skin_path); ngb_video_brightness=1.f; ngb_video_gamma=1.f; ngb_video_tint=0;
    char *dir=SDL_GetPrefPath("NinjaGaidenBlack","touch-controls"); if(!dir)return;
    char active[512]; snprintf(active,sizeof active,"%s/touch-controls-active.cfg",dir);
    FILE *af=fopen(active,"r"); if(af){int p;if(fscanf(af,"%d",&p)==1&&p>=0&&p<=2)s_preset=p;fclose(af);}
    snprintf(s_layout_path,sizeof s_layout_path,"%s/touch-controls-preset-%d.cfg",dir,s_preset);
    SDL_free(dir);
    FILE *fp=fopen(s_layout_path,"r");
    if(!fp&&s_preset==0){char *legacy=SDL_GetPrefPath("NinjaGaidenBlack","touch-controls");if(legacy){snprintf(s_layout_path,sizeof s_layout_path,"%s/touch-controls-v1.cfg",legacy);SDL_free(legacy);fp=fopen(s_layout_path,"r");}}
    if(!fp)return; char line[192]; bool legacy_ids=true;
    if(!fgets(line,sizeof line,fp)||!strstr(line,"NGB_TOUCH_LAYOUT_V5")){fclose(fp);return;}   /* (a layout from the first editor: the hand-made default beats its scatter) */
    rewind(fp);
    memset(s_turbo, 0, sizeof s_turbo);
    while(fgets(line,sizeof line,fp)){if(line[0]=='#'){if(strstr(line,"NGB_TOUCH_LAYOUT_V5")){legacy_ids=false;int h=1,c=1;if(sscanf(line,"# NGB_TOUCH_LAYOUT_V5 %d %d",&h,&c)>=1){s_haptics=h!=0;s_camera_swipe=c!=0;}}else if(strstr(line,"NGB_TOUCH_LAYOUT_V3")){legacy_ids=false;int h=1,c=1;if(sscanf(line,"# NGB_TOUCH_LAYOUT_V3 %d %d",&h,&c)>=1){s_haptics=h!=0;s_camera_swipe=c!=0;}}else if(strstr(line,"NGB_TOUCH_LAYOUT_V2")){legacy_ids=false;int h;if(sscanf(line,"# NGB_TOUCH_LAYOUT_V2 %d",&h)==1)s_haptics=h!=0;}else if(strstr(line,"STYLE ")){int st=0;if(sscanf(line,"# STYLE %d",&st)==1)s_pad_style=(uint8_t)(st<0?0:st>3?3:st);}else if(strstr(line,"TURBO ")){int id=-1,hz=0;if(sscanf(line,"# TURBO %d %d",&id,&hz)==2&&id>=0&&id<NCTRL)s_turbo[id]=(uint8_t)(hz<0?0:hz>20?20:hz);}else if(strstr(line,"THEME ")){int th=0;if(sscanf(line,"# THEME %d",&th)==1)s_pad_theme=th<0?0:th>2?2:th;}else if(strstr(line,"SKIN ")){int sk=0;char sp[256]={0};if(sscanf(line,"# SKIN %d %255s",&sk,&sp[0])==2&&sk>=0&&sk<NCTRL)snprintf(s_skin_path[sk],sizeof s_skin_path[sk],"%s",sp);else if(sscanf(line,"# SKIN %255s",sp)==1)for(int q=0;q<NCTRL;q++)if(s_layout_skin[q])snprintf(s_skin_path[q],sizeof s_skin_path[q],"%s",sp);}else if(strstr(line,"TEXDUMP ")){sscanf(line,"# TEXDUMP %d",&ngb_texture_dump_on);}else if(strstr(line,"FILTER ")){sscanf(line,"# FILTER %511s",s_filter_path);}else if(strstr(line,"VIDEO ")){int b=100,g=100,t=0,co=100,sa=100;if(sscanf(line,"# VIDEO %d %d %d %d %d",&b,&g,&t,&co,&sa)>=2){ngb_video_brightness=b/100.f;ngb_video_gamma=g/100.f;ngb_video_tint=t;if(co>0)ngb_video_contrast=co/100.f;if(sa>=0)ngb_video_saturation=sa/100.f;}}continue;}int id,custom,color=0,shape=0,skin=0,combo=0,enabled=1,action=PA_NONE;float x,y,r,a,asp=1.f;int n=sscanf(line,"%d %f %f %f %f %d %d %d %d %d %d %d %f",&id,&x,&y,&r,&a,&custom,&color,&shape,&skin,&enabled,&combo,&action,&asp);if(n<6||id<0)continue;if(legacy_ids&&id>=15)id++;if(id>=NCTRL)continue;s_layout_x[id]=x;s_layout_y[id]=y;s_layout_r[id]=r;s_layout_alpha[id]=a;s_layout_custom[id]=custom!=0;if(n>=7)s_layout_color[id]=(uint8_t)(color<0?0:color>5?5:color);if(n>=8)s_layout_shape[id]=(uint8_t)(shape<0?0:shape>8?8:shape);if(n>=9)s_layout_skin[id]=(uint8_t)(skin!=0);if(n>=10)s_layout_enabled[id]=enabled!=0;if(n>=11)s_layout_combo[id]=(uint8_t)(combo<0?0:combo>5?5:combo);if(n>=12)s_layout_action[id]=(action>=0&&action<PA_COUNT)?action:PA_NONE;if(n>=13)s_layout_asp[id]=asp<0.4f?0.4f:asp>4.f?4.f:asp;if(id==CUSTOM1&&n>=11&&combo==6)s_layout_enabled[id]=false;}
    fclose(fp);
    filter_curve_build();
    filter_cube_build();
    for(int i=0;i<NCTRL;i++){s_layout_r[i]=fminf(.25f,fmaxf(.035f,s_layout_r[i]));s_layout_x[i]=fminf(8.f,fmaxf(s_ctrl[i].edge==2?-8.f:.04f,s_layout_x[i]));s_layout_y[i]=fminf(.97f,fmaxf(.03f,s_layout_y[i]));s_layout_alpha[i]=fminf(1.f,fmaxf(0.f,s_layout_alpha[i]));}
}
static void layout_save(void) {
    if(!s_layout_path[0]){char *dir=SDL_GetPrefPath("NinjaGaidenBlack","touch-controls");if(!dir)return;snprintf(s_layout_path,sizeof s_layout_path,"%s/touch-controls-preset-%d.cfg",dir,s_preset);SDL_free(dir);}
    char *dir=SDL_GetPrefPath("NinjaGaidenBlack","touch-controls");
    if(dir){char active[512];snprintf(active,sizeof active,"%s/touch-controls-active.cfg",dir);FILE *af=fopen(active,"w");if(af){fprintf(af,"%d\n",s_preset);fclose(af);}SDL_free(dir);}
    FILE *fp=fopen(s_layout_path,"w");if(!fp)return;
    fprintf(fp,"# NGB_TOUCH_LAYOUT_V5 %d %d\n",s_haptics?1:0,s_camera_swipe?1:0);
    fprintf(fp,"# STYLE %d\n",(int)s_pad_style);   /* (it was written as 1 for any of them: bold and solid came back as glass) */
    fprintf(fp,"# THEME %d\n",s_pad_theme);
    for (int i = 0; i < NCTRL; i++)
        if (s_turbo[i])
            fprintf(fp,"# TURBO %d %d\n", i, s_turbo[i]);
    for(int i=0;i<NCTRL;i++)if(s_skin_path[i][0])fprintf(fp,"# SKIN %d %s\n",i,s_skin_path[i]);
    if (s_filter_path[0]) fprintf(fp,"# FILTER %s\n", s_filter_path);
    fprintf(fp,"# TEXDUMP %d\n", ngb_texture_dump_on);
    fprintf(fp,"# VIDEO %d %d %d %d %d\n",(int)lroundf(ngb_video_brightness*100.f),
            (int)lroundf(ngb_video_gamma*100.f),ngb_video_tint,(int)lroundf(ngb_video_contrast*100.f),
            (int)lroundf(ngb_video_saturation*100.f));
    for(int i=0;i<NCTRL;i++)fprintf(fp,"%d %.5f %.5f %.5f %.5f %d %u %u %u %u %u %d %.3f\n",i,s_layout_x[i],s_layout_y[i],s_layout_r[i],s_layout_alpha[i],s_layout_custom[i]?1:0,s_layout_color[i],s_layout_shape[i],s_layout_skin[i],s_layout_enabled[i]?1:0,s_layout_combo[i],s_layout_action[i],s_layout_asp[i]);
    fclose(fp);
}
static void clear_touch_state(void){s_cam_hold=false;s_cam_px=s_cam_py=0;memset(s_finger,0,sizeof s_finger);memset(s_down,0,sizeof s_down);memset(s_stick,0,sizeof s_stick);memset(s_combo_active,0,sizeof s_combo_active);memset(s_combo_y_until,0,sizeof s_combo_y_until);s_dpad=0;}
static void layout_switch_preset(void) {
    layout_save();
    s_preset=(s_preset+1)%3;
    char *dir=SDL_GetPrefPath("NinjaGaidenBlack","touch-controls");
    if(dir){char active[512];snprintf(active,sizeof active,"%s/touch-controls-active.cfg",dir);FILE *af=fopen(active,"w");if(af){fprintf(af,"%d\n",s_preset);fclose(af);}SDL_free(dir);}
    s_layout_path[0]=0; s_layout_loaded=false;
    layout_load();
    s_layout = s_preset==1 ? 1 : 0; s_layout_save=1;
    s_selected=-1; clear_touch_state();
    s_layout_revision++; layout_save(); s_serial++;
}

static void edit_selected(Uint32 held){if(s_selected<0||s_selected>=NCTRL)return;if(held>=650){float q=s_layout_r[s_selected]/s_ctrl[s_selected].r;s_layout_r[s_selected]=s_ctrl[s_selected].r*(q<.88f?1.f:q<1.12f?1.25f:.75f);}else{static const float a[]={.25f,.4f,.55f,.7f,.85f,1.f};int n=0;while(n<6&&s_layout_alpha[s_selected]>=a[n]-.01f)n++;s_layout_alpha[s_selected]=a[n==6?0:n];}s_layout_revision++;layout_save();s_serial++;}
static void add_custom_combo(void) {
    for (int i=CUSTOM1;i<=CUSTOM4;i++) if(!s_layout_enabled[i]) {
        s_layout_enabled[i]=true; s_layout_x[i]=0.52f; s_layout_y[i]=0.32f+0.12f*(i-CUSTOM1);
        s_layout_r[i]=s_ctrl[i].r; s_layout_alpha[i]=0.72f; s_layout_custom[i]=true;
        s_layout_combo[i]=(uint8_t)((i-CUSTOM1)%6); s_selected=i;
        s_layout_revision++; layout_save(); s_serial++; return;
    }
}
static void delete_selected_combo(void) {
    if(s_selected<0||s_selected>=NCTRL||s_selected==SWAP)return;
    s_layout_enabled[s_selected]=false; s_down[s_selected]=false; s_selected=-1;
    s_layout_revision++; layout_save(); s_serial++;
}


/* Where a control is on a screen this wide: its place (in screen heights) and radius. On a squarer screen than
 * about 16:10 (4:3, 3:2, 5:4: below NARROW) there is no room beside the player for the right stick and LT, which
 * then land on him, and the Interact prompt comes in over the right stick's click: there LT is smaller, above
 * the face buttons and below the HUD's ammo; the right stick out to below and inside X, clear of his legs and
 * above the prompt; its click beside A, off the bottom row. The stand-ins (Kenney's, lettered LB and RB as a 360
 * pad's bumpers) swap Black and White's places, so LB is on the left. */
#define NARROW 1.55f

typedef struct { float x, y, r; } Place;

static int s_standin_places;   /* the stand-ins shown (under s_lock; d_standin the picture's copy) */

static Place place_at(const Ctrl *c, float aspect, int standin, int layout)
{
    if (standin && (c == &s_ctrl[LB] || c == &s_ctrl[RB]))
        c = &s_ctrl[c == &s_ctrl[LB] ? RB : LB];
    int ci=(int)(c-s_ctrl); bool custom=s_layout_custom[ci]; Place p=custom?(Place){s_layout_x[ci],s_layout_y[ci],s_layout_r[ci]}:(Place){c->x,c->y,c->r};
    int edge = c->edge;
    if (layout == 1 && !custom && ci <= SWAP) {
        int i = (int)(c - s_ctrl);
        p.x = s_classic[i].x;
        p.y = s_classic[i].y;
        p.r = s_classic[i].r;
        edge = s_classic[i].edge;
    }
    if (!custom && aspect < NARROW && layout != 1) {
        if (c == &s_ctrl[LT]) {
            p.x = 0.44f;
            p.y = 0.27f;
            p.r = 0.06f;
        } else if (c == &s_ctrl[RSTICK]) {
            p.x = 0.44f;
            p.y = 0.745f;
            p.r = 0.095f;
        } else if (c == &s_ctrl[RS]) {
            p.x = 0.06f;
            p.y = 0.79f;
        } else if (c >= &s_ctrl[BA] && c <= &s_ctrl[BY]) {
            p.y -= 0.08f;   /* (the face buttons where they were before #285: lower, X met the right stick here) */
        }
    }
    if (!custom && layout == 1 && aspect < 1.75f && (c == &s_ctrl[LSTICK] || c == &s_ctrl[DPAD_ID])) {   /* (1.75:
                                    * below it the left stick, further in since #285, met the d-pad) */
        /* Classic on a less wide screen (16:10, 4:3): the left stick a little smaller and further out, the d-pad
         * between it and Back, as large as fits there */
        float room0 = 0.25f + 0.14f, room1 = aspect / 2 - 0.14f, r = (room1 - room0) / 2.2f;
        if (c == &s_ctrl[LSTICK]) {
            p.x = 0.25f;
            p.r = 0.14f;
        } else {
            p.r = r < 0.06f ? 0.06f : r > 0.12f ? 0.12f : r;
            p.x = (room0 + room1) / 2;
        }
        p.x = edge == 2 ? aspect / 2 + p.x : edge ? aspect - p.x : p.x;
        return p;
    }
    if (!custom && c == &s_ctrl[DPAD_ID]) {   /* right of the game's quick menu (the items' cross, which it works): that is
                                    * at the lower left of a 16:9 picture, in the middle of a wider screen */
        float clear = (aspect - 16.0f / 9) / 2 + 0.50f, most = aspect / 2 - 0.28f;   /* (and left of Back) */
        p.x = p.x > clear ? p.x : clear;
        p.x = p.x < most ? p.x : most;
    }
    if (!custom && c == &s_ctrl[RSTICK] && aspect >= NARROW && layout != 1) {   /* (in from the middle on a less wide screen) */
        float most = aspect / 2 - 0.12f;   /* (at 3:2 the right stick clear of X) */
        p.x = p.x < most ? p.x : most;
    }
    if (!custom && c == &s_ctrl[RT] && aspect < 1.7f && layout != 1)
        p.y = 0.26f;   /* RT under the health emblem on a less wide screen (16:10, 4:3), where the HUD comes in
                        * over its corner */
    p.x = edge == 2 ? aspect / 2 + p.x : edge ? aspect - p.x : p.x;
    return p;
}

static Place place(const Ctrl *c)
{
    return place_at(c, s_aspect, s_standin_places, s_layout == 1);
}

/* The control under (x, y) (screen heights), the sticks and pad found from a little way off */
static int hit(float x, float y, bool buttons_only)
{
    int best = -1;
    float best_d = 1e9f;
    for (int i = 0; i < NCTRL; i++) {
        if (!s_layout_enabled[i]) continue;
        const Ctrl *c = &s_ctrl[i];
        if (buttons_only && c->kind != BUTTON)
            continue;
        Place at = place(c);
        float asp = s_layout_asp[i] > 0.01f ? s_layout_asp[i] : 1.f;
        float dx = (x - at.x) / asp, dy = y - at.y, d;
        float reach = c->kind == STICK ? at.r * 1.7f : c->kind == DPAD ? at.r * 1.4f : at.r * 1.35f;
        d = sqrtf(dx * dx + dy * dy);
        if (d > reach)
            continue;
        d /= reach;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

static void set_stick(int i, float x, float y)
{
    Place at = place(&s_ctrl[i]);
    float dx = (x - at.x) / at.r, dy = (y - at.y) / at.r, m = sqrtf(dx * dx + dy * dy);
    if (m > 1) {
        dx /= m;
        dy /= m;
    }
    int s = i == LSTICK ? 0 : 1;
    s_stick[s][0] = dx;
    s_stick[s][1] = dy;
}

static int dpad_dirs(float x, float y)
{
    Place at = place(&s_ctrl[DPAD_ID]);
    float dx = x - at.x, dy = y - at.y;
    if (dx * dx + dy * dy < (at.r * 0.25f) * (at.r * 0.25f))
        return 0;
    float a = atan2f(-dy, dx) * 180.0f / (float)M_PI;   /* 0 right, 90 up */
    int d = 0;
    /* (the diagonals 20 degrees wide, not 45: scrolling down a menu took Left or Right too and changed a row, #284) */
    if (a > -55.0f && a < 55.0f) d |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (a > 35.0f && a < 145.0f) d |= XBOX_GAMEPAD_DPAD_UP;
    if (a > 125.0f || a < -125.0f) d |= XBOX_GAMEPAD_DPAD_LEFT;
    if (a < -35.0f && a > -145.0f) d |= XBOX_GAMEPAD_DPAD_DOWN;
    return d;
}

static void touch_vibrate(void) {
#if defined(__ANDROID__)
    if (!s_haptics) return;
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = SDL_AndroidGetActivity();
    if (!env || !activity) return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID get_service = cls ? (*env)->GetMethodID(env, cls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;") : NULL;
    if (!get_service) { if (cls) (*env)->DeleteLocalRef(env, cls); return; }
    jstring service_name = (*env)->NewStringUTF(env, "vibrator");
    jobject vibrator = (*env)->CallObjectMethod(env, activity, get_service, service_name);
    if (service_name) (*env)->DeleteLocalRef(env, service_name);
    if (!vibrator) { if (cls) (*env)->DeleteLocalRef(env, cls); return; }
    jclass vcls = (*env)->GetObjectClass(env, vibrator);
    jmethodID vibrate = vcls ? (*env)->GetMethodID(env, vcls, "vibrate", "(J)V") : NULL;
    if (vibrate) (*env)->CallVoidMethod(env, vibrator, vibrate, (jlong)18);
    if (vcls) (*env)->DeleteLocalRef(env, vcls);
    (*env)->DeleteLocalRef(env, vibrator);
    if (cls) (*env)->DeleteLocalRef(env, cls);
#endif
}

static void finger_on(int ctrl, float x, float y)
{
    if (ctrl == LSTICK || ctrl == RSTICK)
        set_stick(ctrl, x, y);
    else if (ctrl == DPAD_ID)
        s_dpad = dpad_dirs(x, y);
    else {
        s_down[ctrl] = true;
        if (s_ctrl[ctrl].kind == BUTTON || s_ctrl[ctrl].kind == COMBO) touch_vibrate();
    }
    if (ctrl == LSTICK || ctrl == RSTICK || ctrl == DPAD_ID)
        s_down[ctrl] = true;
}

/* What the fingers hold now, from scratch (a button let go is up unless another finger holds it) */
static void finger_off(int ctrl)
{
    for (int f = 0; f < MAX_FINGERS; f++)
        if (s_finger[f].used && s_finger[f].ctrl == ctrl)
            return;
    s_down[ctrl] = false;
    if (ctrl == LSTICK || ctrl == RSTICK)
        s_stick[ctrl == LSTICK ? 0 : 1][0] = s_stick[ctrl == LSTICK ? 0 : 1][1] = 0;
    if (ctrl == DPAD_ID)
        s_dpad = 0;
}

static int s_skin_want = -1;   /* the control the album was opened for */

static void touch_pick_skin(void) {
    if (s_selected < 0 || s_selected >= NCTRL) return;
    s_layout_skin[s_selected] = true;
    s_skin_want = s_selected;
    s_layout_revision++; layout_save(); s_serial++;
#if defined(__ANDROID__)
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = SDL_AndroidGetActivity();
    if (!env || !activity) return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID method = cls ? (*env)->GetMethodID(env, cls, "pickTouchSkin", "()V") : NULL;
    if (method) (*env)->CallVoidMethod(env, activity, method);
    if (cls) (*env)->DeleteLocalRef(env, cls);
#endif
}
static void touch_poll_skin(void) {
#if defined(__ANDROID__)
    static Uint32 next_poll;
    Uint32 now = SDL_GetTicks();
    if ((Sint32)(now - next_poll) < 0) return;
    next_poll = now + 250;
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = SDL_AndroidGetActivity();
    if (!env || !activity) return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID method = cls ? (*env)->GetMethodID(env, cls, "takeTouchSkinPath", "()Ljava/lang/String;") : NULL;
    jstring result = method ? (jstring)(*env)->CallObjectMethod(env, activity, method) : NULL;
    char path[512] = {0};
    if (result) {
        const char *utf = (*env)->GetStringUTFChars(env, result, NULL);
        if (utf) { snprintf(path, sizeof path, "%s", utf); (*env)->ReleaseStringUTFChars(env, result, utf); }
        (*env)->DeleteLocalRef(env, result);
    }
    if (cls) (*env)->DeleteLocalRef(env, cls);
    if (path[0]) {
        pthread_mutex_lock(&s_lock);
        int id = s_skin_want >= 0 && s_skin_want < NCTRL ? s_skin_want : (s_selected >= 0 ? s_selected : 0);
        if (strcmp(path, s_skin_path[id])) {
            snprintf(s_skin_path[id], sizeof s_skin_path[id], "%s", path);
            s_layout_skin[id] = 1;
            s_layout_revision++; layout_save(); s_serial++;
        }
        s_skin_want = -1;
        pthread_mutex_unlock(&s_lock);
    }
#endif
}

extern int ngb_menu_touch_blocked(void);
static int SDLCALL touch_watch(void *unused, SDL_Event *ev)
{
    (void)unused;
    /* Do not consume menu touches here: let SDL queue them so the
     * in-game menu can route them through its native hit-testing path. */
    /* The menus take the touches themselves (their own hit-testing), and the controls must not press anything
     * there. Only the lift is ours: a stick left turned under a finger that went up inside a menu kept driving
     * the menu's tab left and right until another touch reset it -- and clearing everything on every touch
     * left the pad's dpad unable to walk the menu at all, which read as the menu being frozen (user 2026-11-27). */
    /* A menu does not switch the pad off: its dpad walks the rows, A chooses and B goes back -- and the page
     * under it is clicked too (ingame_menu.c turns the same fingers into its mouse). Only the empty-screen
     * swipe is off there, so a drag over the page does not turn the camera (user 2026-11-27). */
    int menu_up = ngb_menu_touch_blocked();
    if(ev->type!=SDL_FINGERDOWN&&ev->type!=SDL_FINGERMOTION&&ev->type!=SDL_FINGERUP)return 0;
    if(ev->type!=SDL_FINGERUP&&xbox_InputControllerPresent())return 0;
    if(ev->type==SDL_FINGERDOWN&&!__atomic_load_n(&s_art_shown,__ATOMIC_ACQUIRE))return 0;
    layout_load(); pthread_mutex_lock(&s_lock);
    SDL_Window *w=SDL_GetWindowFromID(ev->tfinger.windowID);int ww=0,wh=0;if(w)SDL_GetWindowSize(w,&ww,&wh);if(ww>0&&wh>0)s_aspect=(float)ww/(float)wh;
    float x=ev->tfinger.x*s_aspect,y=ev->tfinger.y;
    if(ev->type==SDL_FINGERDOWN&&hit(x,y,false)==SWAP){clear_touch_state();pthread_mutex_unlock(&s_lock);ngb_settings_open();return 0;}
    if(ev->type==SDL_FINGERDOWN&&hit(x,y,false)==OPACITY){   /* (upstream 1.0.3: how solid the pad is drawn) */
        s_opacity = s_opacity <= 40 ? 100 : s_opacity - 20;
        s_opacity_save = 1;
        pthread_mutex_unlock(&s_lock);
        return 0;
    }
    int f;for(f=0;f<MAX_FINGERS&&!(s_finger[f].used&&s_finger[f].id==ev->tfinger.fingerId);f++);
    if(ev->type==SDL_FINGERDOWN&&f==MAX_FINGERS){
        int hit_id=hit(x,y,false);for(f=0;f<MAX_FINGERS&&s_finger[f].used;f++);
        if(f<MAX_FINGERS&&s_edit_mode){s_finger[f].used=true;s_finger[f].id=ev->tfinger.fingerId;s_finger[f].down_timestamp=ev->tfinger.timestamp;s_finger[f].ctrl=hit_id;s_finger[f].x0=x;s_finger[f].y0=y;s_finger[f].moved=false;s_finger[f].was_selected=hit_id==s_selected;if(hit_id>=0)s_selected=hit_id;s_serial++;}
        else if(hit_id>=0&&f<MAX_FINGERS){xbox_InputTouchUsed();s_finger[f].used=true;s_finger[f].id=ev->tfinger.fingerId;s_finger[f].ctrl=hit_id;finger_on(hit_id,x,y);s_serial++;}
        else if(!s_edit_mode&&!menu_up&&hit_id<0&&f<MAX_FINGERS&&s_camera_swipe){s_finger[f].used=true;s_finger[f].id=ev->tfinger.fingerId;s_finger[f].ctrl=-2;s_finger[f].x0=x;s_finger[f].y0=y;s_cam_hold=true;s_cam_px=s_cam_py=0;}
    }else if(ev->type==SDL_FINGERMOTION&&f<MAX_FINGERS){
        int id=s_finger[f].ctrl;
        if(!s_edit_mode&&id==-2){float dx=x-s_finger[f].x0,dy=y-s_finger[f].y0;if(fabsf(dx)+fabsf(dy)>.001f){s_cam_px+=dx;s_cam_py+=dy;xbox_InputTouchUsed();s_finger[f].x0=x;s_finger[f].y0=y;s_serial++;}}
        else if(s_edit_mode){if(id>=0){int peer=-1;for(int k=0;k<MAX_FINGERS;k++)if(k!=f&&s_finger[k].used&&s_finger[k].ctrl==id){peer=k;break;}if(peer>=0){float ox=s_finger[f].x0-s_finger[peer].x0,oy=s_finger[f].y0-s_finger[peer].y0;float nx=x-s_finger[peer].x0,ny=y-s_finger[peer].y0;float before=hypotf(ox,oy),after=hypotf(nx,ny);if(before>.01f&&after>.01f){float ratio=after/before;s_layout_r[id]=fminf(.25f,fmaxf(.035f,s_layout_r[id]*ratio));s_layout_custom[id]=true;s_finger[f].moved=true;s_layout_revision++;s_serial++;}s_finger[f].x0=x;s_finger[f].y0=y;}else{float dx=x-s_finger[f].x0,dy=y-s_finger[f].y0;if(fabsf(dx)+fabsf(dy)>.012f)s_finger[f].moved=true;if(s_finger[f].moved){s_layout_x[id]+=s_ctrl[id].edge==1?-dx:dx;s_layout_y[id]+=dy;s_layout_custom[id]=true;if(s_ctrl[id].edge==2)s_layout_x[id]=fminf(s_aspect/2-.04f,fmaxf(-s_aspect/2+.04f,s_layout_x[id]));else s_layout_x[id]=fminf(s_aspect-.04f,fmaxf(.04f,s_layout_x[id]));s_layout_y[id]=fminf(.97f,fmaxf(.03f,s_layout_y[id]));s_finger[f].x0=x;s_finger[f].y0=y;s_layout_revision++;s_serial++;}}}}
        else if(id>=0&&s_ctrl[id].kind==BUTTON&&id>=BA&&id<=BY){int to=hit(x,y,true);if(to>=BA&&to<=BY&&to!=id){s_finger[f].ctrl=to;finger_off(id);finger_on(to,x,y);s_serial++;}}
        else if(id==LSTICK||id==RSTICK||id==DPAD_ID){int si=id==RSTICK,wd=s_dpad;float wx=id==DPAD_ID?0:s_stick[si][0],wy=id==DPAD_ID?0:s_stick[si][1];finger_on(id,x,y);if(id==DPAD_ID?s_dpad!=wd:fabsf(s_stick[si][0]-wx)+fabsf(s_stick[si][1]-wy)>1.f/32)s_serial++;}
    }else if(ev->type==SDL_FINGERUP&&f<MAX_FINGERS){int id=s_finger[f].ctrl;bool moved=s_finger[f].moved,was=s_finger[f].was_selected;Uint32 held=ev->tfinger.timestamp-s_finger[f].down_timestamp;s_finger[f].used=false;if(s_edit_mode){if(!moved&&was&&id>=0)edit_selected(held);if(moved){s_layout_revision++;layout_save();s_serial++;}if(!moved&&id<0&&y>.86f){layout_defaults();layout_save();s_layout_revision++;s_selected=-1;}}else if(id==-2){s_cam_hold=false;s_cam_px=s_cam_py=0;finger_off(RSTICK);}else if(id>=0)finger_off(id);s_serial++;}
    pthread_mutex_unlock(&s_lock);return 0;
}

static void let_go(void)
{
    static int had;
    int now = xbox_InputControllerPresent();
    if (__atomic_exchange_n(&had, now, __ATOMIC_ACQ_REL) == now || !now)   /* (two readers: play, the quick menu) */
        return;
    pthread_mutex_lock(&s_lock);
    memset(s_finger, 0, sizeof s_finger);
    memset(s_down, 0, sizeof s_down);
    memset(s_stick, 0, sizeof s_stick);
    memset(s_combo_active, 0, sizeof s_combo_active);
    s_dpad = 0;
    s_serial++;
    pthread_mutex_unlock(&s_lock);
}

static void touch_init(void)
{
    layout_load();
    static int done;
    if (__atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL))
        return;
    pthread_mutex_lock(&s_lock);
    layout_known();
    pthread_mutex_unlock(&s_lock);
    SDL_AddEventWatch(touch_watch, NULL);
}

/* Merged into the keyboard's pad state (src/keyboard.c), and read alone by the quick menu (src/quick_ui.c) */
void ngb_touch_map(XBOX_INPUT_STATE *st)
{
    touch_init();
    let_go();
    {   /* (a menu up: the pad goes faint, and comes back when it closes) */
        int md = ngb_menu_touch_blocked() ? 1 : 0;
        pthread_mutex_lock(&s_lock);
        if (md != s_menu_dim) {
            s_menu_dim = md;
            s_layout_revision++;
            s_serial++;
        }
        pthread_mutex_unlock(&s_lock);
    }
    /* the layout and the opacity written here, on the game's side, not on Android's UI thread; one caller at a time
     * (play and the quick menu both come here), each writing what is current, so a later tap is never overwritten
     * by an earlier one */
    static pthread_mutex_t save_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&save_lock);
    pthread_mutex_lock(&s_lock);
    int save = s_layout_save ? s_layout : -1, opacity = s_opacity_save ? s_opacity : -1;
    s_layout_save = s_opacity_save = 0;
    pthread_mutex_unlock(&s_lock);
    if (save >= 0 && ngb_pc_set_setting)
        ngb_pc_set_setting(48, save);
    if (opacity >= 0 && ngb_pc_set_setting)
        ngb_pc_set_setting(49, opacity);
    pthread_mutex_unlock(&save_lock);
    pthread_mutex_lock(&s_lock);
    WORD b = st->Gamepad.wButtons | (WORD)s_dpad;
    /* Each button pressed as the layout has it bound: its own place on the pad unless the player moved the action
     * to another button (src/menu, the touch editor). A button no action is bound to presses nothing. */
    static const int analog_of[8] = {
        XBOX_BUTTON_A, XBOX_BUTTON_B, XBOX_BUTTON_X, XBOX_BUTTON_Y,
        XBOX_BUTTON_BLACK, XBOX_BUTTON_WHITE,   /* (as a 360 pad's bumpers are: input/xinput_device.c) */
        XBOX_BUTTON_LTRIGGER, XBOX_BUTTON_RTRIGGER,
    };
    static const WORD digital_of[4] = {
        XBOX_GAMEPAD_BACK, XBOX_GAMEPAD_START, XBOX_GAMEPAD_LEFT_THUMB, XBOX_GAMEPAD_RIGHT_THUMB,
    };
    for (int i = 0; i < NCTRL; i++) {
        if (!s_down[i] || s_layout_action[i] == PA_NONE)
            continue;
        if (s_turbo[i] && (((Uint64)SDL_GetTicks64() * s_turbo[i] / 1000) & 1))   /* (its off half) */
            continue;
        int act = theme_action(i, s_layout_action[i]);
        if (act < 8)
            st->Gamepad.bAnalogButtons[analog_of[act]] = 255;
        else if (act < PA_COUNT)
            b |= digital_of[act - 8];
    }
    st->Gamepad.wButtons = b;
    static const int combo_buttons[6][2] = {
        {XBOX_BUTTON_A,XBOX_BUTTON_B},{XBOX_BUTTON_A,XBOX_BUTTON_X},{XBOX_BUTTON_A,XBOX_BUTTON_Y},
        {XBOX_BUTTON_B,XBOX_BUTTON_X},{XBOX_BUTTON_B,XBOX_BUTTON_Y},{XBOX_BUTTON_X,XBOX_BUTTON_Y}
    };
    if (s_down[LSTICK]) {
        st->Gamepad.sThumbLX = (SHORT)(s_stick[0][0] * 32767);
        st->Gamepad.sThumbLY = (SHORT)(-s_stick[0][1] * 32767);
    }
    if (s_cam_hold) {   /* (the drag of a finger on the empty screen: the camera follows it, eased) */
        float gx = s_cam_px * 5.5f, gy = s_cam_py * 5.5f;
        s_cam_px = s_cam_py = 0;
        gx = gx > 1.f ? 1.f : gx < -1.f ? -1.f : gx;
        gy = gy > 1.f ? 1.f : gy < -1.f ? -1.f : gy;
        s_stick[1][0] += (gx - s_stick[1][0]) * 0.55f;
        s_stick[1][1] += (gy - s_stick[1][1]) * 0.55f;
        if (fabsf(s_stick[1][0]) < 0.02f) s_stick[1][0] = 0;
        if (fabsf(s_stick[1][1]) < 0.02f) s_stick[1][1] = 0;
    }
    if (s_down[RSTICK] || s_cam_hold) {   /* (a drag turns the stick, not the RS button under it) */
        st->Gamepad.sThumbRX = (SHORT)(s_stick[1][0] * 32767);
        st->Gamepad.sThumbRY = (SHORT)(-s_stick[1][1] * 32767);
    }
    Uint32 combo_now = SDL_GetTicks();
    for (int i=NINPO;i<NCTRL;i++) {
        if (s_layout_combo[i] < 6) {
            if (s_down[i]) { int p=s_layout_combo[i]; st->Gamepad.bAnalogButtons[combo_buttons[p][0]]=255; st->Gamepad.bAnalogButtons[combo_buttons[p][1]]=255; }
        } else if (s_combo_active[i]) {
            if (!s_down[i]) {
                s_combo_active[i] = false;
                s_combo_y_until[i] = 0;
            }
        }
    }

    pthread_mutex_unlock(&s_lock);
}

/* ---- the picture ---- */

static uint32_t *s_px;   /* the picture being drawn (the presenter's buffer), and what it shows */
static int s_w, s_h;
static bool d_down[NCTRL];
static float d_stick[2][2], d_aspect;
static int d_layout;
static float d_opacity = 1;       /* Touch Opacity: every piece's opacity times this */
static int d_dpad;

typedef struct { uint8_t *px; int w, h; unsigned generation; } Sprite;   /* straight-alpha RGBA */
static Sprite s_skin_image[NCTRL];
static char s_skin_loaded[NCTRL][256];

static const Sprite *user_skin(int id) {
    if (id < 0 || id >= NCTRL || !d_skin_path[id][0]) return NULL;
    Sprite *slot = &s_skin_image[id];
    if (strcmp(s_skin_loaded[id], d_skin_path[id])) {
        if (slot->px) { stbi_image_free(slot->px); slot->px = NULL; }
        slot->w = slot->h = 0;
        slot->generation++;
        snprintf(s_skin_loaded[id], sizeof s_skin_loaded[id], "%s", d_skin_path[id]);
        int channels = 0;
        FILE *file = fopen(s_skin_loaded[id], "rb");
        if (file && !fseek(file, 0, SEEK_END)) {
            long size = ftell(file);
            if (size >= 8 && size <= 16 * 1024 * 1024 && !fseek(file, 0, SEEK_SET)) {
                unsigned char *bytes = malloc((size_t)size);
                if (bytes) {
                    if (fread(bytes, 1, (size_t)size, file) == (size_t)size)
                        slot->px = stbi_load_from_memory(bytes, (int)size, &slot->w, &slot->h, &channels, 4);
                    free(bytes);
                }
            }
        }
        if (file) fclose(file);
        if (slot->px && (slot->w > 2048 || slot->h > 2048 || slot->w < 8 || slot->h < 8)) {
            stbi_image_free(slot->px);
            slot->px = NULL;
            slot->w = slot->h = 0;
        }
    }
    return slot->px ? slot : NULL;
}


/* A stand-in piece by name (assets/touch), decoded the first time (drawing is one thread at a time: the
 * presenter's lock) */
static const Sprite *sprite(const char *name)
{
    static struct { char name[40]; Sprite s; } cache[32];
    int i;
    for (i = 0; i < 32 && cache[i].name[0] && strcmp(cache[i].name, name); i++)
        ;
    if (i == 32)
        return NULL;
    if (!cache[i].name[0]) {
        snprintf(cache[i].name, sizeof cache[i].name, "%s", name);
        for (int k = 0; k < ngb_touch_png_count; k++)
            if (!strcmp(ngb_touch_pngs[k].name, name)) {
                int n;
                cache[i].s.px = stbi_load_from_memory(ngb_touch_pngs[k].data, (int)*ngb_touch_pngs[k].len,
                                                      &cache[i].s.w, &cache[i].s.h, &n, 4);
                break;
            }
    }
    return &cache[i].s;
}

/* What the controls are drawn with: nothing yet, the pieces made from the game's art, or the stand-ins */
enum Art { ART_NONE, ART_GAME, ART_STANDIN };
static enum Art s_art;   /* (the picture being drawn's) */

/* (user 2026-11-27: the pad has to be there as the game opens -- in the hurricane build the art never reads,
 * and twenty seconds with nothing on the glass looked like the controls were broken) */
#define STANDIN_AFTER_MS 200

/* The presenter's, each frame it asks for the picture: the game's pieces once there (saved by an earlier start, or
 * made); the stand-ins after STANDIN_AFTER_MS of frames without them (each frame counted at most 0.1 s, so a long
 * load does not count) -- or at once in the arcade, which has no game art -- until the game's come */
static enum Art art_now(void)
{
    static int standin;
    static uint64_t last;
    static uint32_t waited;
    /* The game's pieces only when the buttons read as well: a translation or a mod pack (x2.1) moves the code
     * they are taken from, and a half-read table draws empty plates where the pad should be. */
    if (ngb_gp_touch && ngb_gp_touch("pad") && ngb_gp_touch("button_a") && ngb_gp_touch("button_b") &&
        ngb_gp_touch("button_x") && ngb_gp_touch("button_y"))
        return ART_GAME;
    if (standin)
        return ART_STANDIN;
    uint64_t now = SDL_GetTicks64();
    if (last)
        waited += (uint32_t)(now - last < 100 ? now - last : 100);
    last = now;
    if (!ngb_gp_touch || waited >= STANDIN_AFTER_MS) {
        standin = 1;
        fprintf(stderr, "touch: the game's art not read, the stand-in controls shown\n");
        return ART_STANDIN;
    }
    return ART_NONE;
}

/* A stand-in button: a dark disc with the button's letters (launcher_font.h) in its colour, 128 x 128, made the
 * first time */
static const Sprite *standin_button(int id)
{
    static Sprite made[NCTRL];
    static bool tried[NCTRL];
    const Ctrl *c = &s_ctrl[id];
    if (tried[id] || !c->label)
        return made[id].px ? &made[id] : NULL;
    tried[id] = true;
    const int N = 128;
    const float R = 52, rim = 3.5f;
    float *ink = calloc((size_t)N * N, sizeof(float));
    uint8_t *px = calloc((size_t)N * N, 4);
    if (!ink || !px) {
        free(ink);
        free(px);
        return NULL;
    }
    /* the letters at k times the font's size: as tall as fits, no wider than the disc's middle */
    int n = (int)strlen(c->label), tw = 0;
    for (int i = 0; i < n; i++)
        tw += c->label[i] >= 32 && c->label[i] < 127 ? launcher_font_w[c->label[i] - 32] : 0;
    float k = n == 1 ? 2.3f : n == 2 ? 1.75f : 1.2f;
    if (tw * k > 1.6f * R)
        k = 1.6f * R / tw;
    float gh = LAUNCHER_FONT_H_PX * k;
    int x0 = N, y0 = N, x1 = -1, y1 = -1;
    float pen = 0;
    for (int i = 0; i < n; i++) {
        int g = c->label[i] - 32;
        if (g < 0 || g >= 95)
            continue;
        int w = launcher_font_w[g];
        const uint8_t *src = launcher_font_px + launcher_font_off[g];
        for (int Y = 0; Y < (int)ceilf(gh) && Y < N; Y++)
            for (int X = (int)floorf(pen); X < (int)ceilf(pen + w * k) && X < N; X++) {
                float u = (X + 0.5f - pen) / k - 0.5f, v = (Y + 0.5f) / k - 0.5f;   /* (bilinear) */
                int iu = (int)floorf(u), iv = (int)floorf(v);
                float fu = u - iu, fv = v - iv, a = 0;
                for (int q = 0; q < 4; q++) {
                    int su = iu + (q & 1), sv = iv + (q >> 1);
                    if (su < 0 || sv < 0 || su >= w || sv >= LAUNCHER_FONT_H_PX)
                        continue;
                    a += src[sv * w + su] / 255.0f * ((q & 1) ? fu : 1 - fu) * ((q >> 1) ? fv : 1 - fv);
                }
                if (a > ink[Y * N + X])
                    ink[Y * N + X] = a;
                if (a > 0.05f) {
                    if (X < x0) x0 = X;
                    if (X > x1) x1 = X;
                    if (Y < y0) y0 = Y;
                    if (Y > y1) y1 = Y;
                }
            }
        pen += w * k;
    }
    int dx = x1 < 0 ? 0 : N / 2 - (x0 + x1 + 1) / 2, dy = y1 < 0 ? 0 : N / 2 - (y0 + y1 + 1) / 2;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float d = hypotf(x + 0.5f - N / 2.0f, y + 0.5f - N / 2.0f);
            float a = fminf(1, fmaxf(0, R - d + 0.5f));
            if (a <= 0)
                continue;
            float t = fminf(1, fmaxf(0, d - (R - rim) + 0.5f));    /* the rim, lighter */
            float r = 0x2A + (0x58 - 0x2A) * t, g = 0x2A + (0x58 - 0x2A) * t, b = 0x33 + (0x66 - 0x33) * t;
            int sx = x - dx, sy = y - dy;
            float l = sx >= 0 && sy >= 0 && sx < N && sy < N ? ink[sy * N + sx] : 0;
            r += ((c->colour >> 16 & 255) - r) * l;
            g += ((c->colour >> 8 & 255) - g) * l;
            b += ((c->colour & 255) - b) * l;
            uint8_t *p = &px[4 * (y * N + x)];
            p[0] = (uint8_t)lroundf(r);
            p[1] = (uint8_t)lroundf(g);
            p[2] = (uint8_t)lroundf(b);
            p[3] = (uint8_t)lroundf(a * 255);
        }
    free(ink);
    made[id].px = px;
    made[id].w = made[id].h = N;
    return &made[id];
}

/* A piece: made from the game's art ("pad", "dpad_east_lit"...), else the embedded one of that name with
 * "touch_" before it (Kenney's) */
static const Sprite *piece(const char *name)
{
    static Sprite s;
    if (s_art == ART_GAME) {
        const NgbPic *p = ngb_gp_touch(name);
        if (!p)
            return NULL;
        s.px = p->px;
        s.w = p->w;
        s.h = p->h;
        return &s;
    }
    char n[48];
    snprintf(n, sizeof n, "touch_%s", name);
    return sprite(n);
}

/* What was drawn over the untouched controls in each of the presenter's buffers (it hands a few in turn, each
 * still holding the picture drawn into it last): only those boxes are put back before the next picture, not the
 * whole of it copied, which at the window's own size is ten megabytes a frame while a stick is dragged. */
#define OVER_BUFS 4
#define OVER_BOXES 24
typedef struct { uint32_t *px; int w, h, n; unsigned bg; int box[OVER_BOXES][4]; } Over;
static Over s_over[OVER_BUFS];
static Over *s_rec;      /* the buffer being drawn over (NULL: the untouched picture itself) */
static unsigned s_bg_gen;   /* +1 each untouched picture made */
static int s_chg[2 * OVER_BOXES][4], s_chg_n = -1;   /* the last picture against the one before (-1: all of it) */

static void over_box(int x0, int y0, int x1, int y1)
{
    Over *o = s_rec;
    if (!o || o->n < 0)
        return;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > s_w ? s_w : x1;
    y1 = y1 > s_h ? s_h : y1;
    if (x0 >= x1 || y0 >= y1)
        return;
    if (o->n == OVER_BOXES) {
        o->n = -1;       /* (more than kept: the whole picture put back next time) */
        return;
    }
    int *b = o->box[o->n++];
    b[0] = x0; b[1] = y0; b[2] = x1; b[3] = y1;
}

/* A picture at the size it is drawn, premultiplied BGRA: scaled once and kept (scaled at each drawing, a few
 * held buttons and a dragged stick took milliseconds of every frame). Each pixel is the picture's texels under it
 * averaged by how much of each it covers (the game's pieces are made larger than drawn), or sampled bilinearly
 * where it is drawn larger. ox, oy: where the picture's corner lies in its first pixel (0..1). */
typedef struct { const uint8_t *src; int sw, sh, w, h; float bw, bh, ox, oy; unsigned generation; uint32_t *px; } Scaled;
#define SCALED_N 96
static Scaled s_scaled[SCALED_N];
static int s_nscaled;

static const Scaled *scaled(const Sprite *sp, int w, int h, float bw, float bh, float ox, float oy)
{
    for (int i = 0; i < s_nscaled; i++) {
        const Scaled *c = &s_scaled[i];
        if (c->src == sp->px && c->sw == sp->w && c->sh == sp->h && c->generation == sp->generation && c->w == w && c->h == h &&
            fabsf(c->bw - bw) < 1e-3f && fabsf(c->bh - bh) < 1e-3f && fabsf(c->ox - ox) < 1e-3f &&
            fabsf(c->oy - oy) < 1e-3f)
            return c;
    }
    if (s_nscaled == SCALED_N) {      /* (other sizes' left from a window since resized: all made again) */
        for (int i = 0; i < SCALED_N; i++)
            free(s_scaled[i].px);
        s_nscaled = 0;
    }
    uint32_t *px = malloc(sizeof(uint32_t) * (size_t)w * h);
    if (!px)
        return NULL;
    float kx = sp->w / bw, ky = sp->h / bh;
    bool area = kx > 1.0f || ky > 1.0f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float acc[4] = {0, 0, 0, 0};
            if (area) {
                float u0 = (x - ox) * kx, v0 = (y - oy) * ky, u1 = u0 + kx, v1 = v0 + ky;
                int iu0 = (int)floorf(u0), iv0 = (int)floorf(v0), iu1 = (int)ceilf(u1), iv1 = (int)ceilf(v1);
                for (int v = iv0 < 0 ? 0 : iv0; v < iv1 && v < sp->h; v++) {
                    float wv = fminf(v1, v + 1.0f) - fmaxf(v0, (float)v);
                    for (int u = iu0 < 0 ? 0 : iu0; u < iu1 && u < sp->w; u++) {
                        float wgt = wv * (fminf(u1, u + 1.0f) - fmaxf(u0, (float)u));
                        const uint8_t *t = &sp->px[((size_t)v * sp->w + u) * 4];
                        float ta = t[3] / 255.0f * wgt;
                        acc[0] += t[0] / 255.0f * ta;
                        acc[1] += t[1] / 255.0f * ta;
                        acc[2] += t[2] / 255.0f * ta;
                        acc[3] += ta;
                    }
                }
                for (int k = 0; k < 4; k++)
                    acc[k] /= kx * ky;
            } else {
                float u = (x + 0.5f - ox) / bw * sp->w - 0.5f, v = (y + 0.5f - oy) / bh * sp->h - 0.5f;
                if (!(u < -0.5f || v < -0.5f || u > sp->w - 0.5f || v > sp->h - 0.5f)) {
                    int iu = (int)floorf(u), iv = (int)floorf(v);
                    float fu = u - iu, fv = v - iv;
                    for (int k = 0; k < 4; k++) {   /* (premultiplied before filtering: no dark fringes) */
                        int su = iu + (k & 1), sv = iv + (k >> 1);
                        su = su < 0 ? 0 : su >= sp->w ? sp->w - 1 : su;
                        sv = sv < 0 ? 0 : sv >= sp->h ? sp->h - 1 : sv;
                        const uint8_t *t = &sp->px[((size_t)sv * sp->w + su) * 4];
                        float wgt = ((k & 1) ? fu : 1 - fu) * ((k >> 1) ? fv : 1 - fv), ta = t[3] / 255.0f;
                        acc[0] += wgt * t[0] / 255.0f * ta;
                        acc[1] += wgt * t[1] / 255.0f * ta;
                        acc[2] += wgt * t[2] / 255.0f * ta;
                        acc[3] += wgt * ta;
                    }
                }
            }
            for (int k = 0; k < 4; k++)
                acc[k] = acc[k] < 0 ? 0 : acc[k] > 1 ? 1 : acc[k];
            px[(size_t)y * w + x] = (uint32_t)(acc[2] * 255 + 0.5f) | (uint32_t)(acc[1] * 255 + 0.5f) << 8 |
                                    (uint32_t)(acc[0] * 255 + 0.5f) << 16 | (uint32_t)(acc[3] * 255 + 0.5f) << 24;
        }
    Scaled *c = &s_scaled[s_nscaled++];
    *c = (Scaled){ sp->px, sp->w, sp->h, w, h, bw, bh, ox, oy, sp->generation, px };
    return c;
}

/* A picture over the box of size (bw, bh) centred at (cx, cy), at opacity a, blended premultiplied */
static void draw_sprite(const Sprite *sp, float cx, float cy, float bw, float bh, float a)
{
    if (!sp || !sp->px || bw < 1 || bh < 1 || bw > 4096 || bh > 4096)
        return;
    int x0 = (int)floorf(cx - bw / 2), y0 = (int)floorf(cy - bh / 2), x1 = (int)ceilf(cx + bw / 2),
        y1 = (int)ceilf(cy + bh / 2);
    const Scaled *sc = scaled(sp, x1 - x0, y1 - y0, bw, bh, cx - bw / 2 - x0, cy - bh / 2 - y0);
    if (!sc)
        return;
    over_box(x0, y0, x1, y1);
    a *= d_opacity;
    uint32_t A = (uint32_t)((a < 0 ? 0 : a > 1 ? 1 : a) * 256 + 0.5f);
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < s_h; y++) {
        const uint32_t *q = sc->px + (size_t)(y - y0) * sc->w - x0;
        uint32_t *row = s_px + (size_t)y * s_w;
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < s_w; x++) {
            uint32_t t = q[x];
            if (!(t >> 24))
                continue;
            uint32_t c = row[x], k = 255 * 256 - (t >> 24) * A, o = 0;   /* (what of the picture under it is kept) */
            for (int sh = 0; sh < 32; sh += 8)
                o |= (((c >> sh & 0xFF) * k + (t >> sh & 0xFF) * A * 255 + 255 * 128) / (255 * 256)) << sh;
            row[x] = o;
        }
    }
}

static void draw(const char *name, float cx, float cy, float bw, float bh, float a)
{
    draw_sprite(sprite(name), cx, cy, bw, bh, a);
}

static void draw_piece(const char *name, float cx, float cy, float bw, float bh, float a)
{
    draw_sprite(piece(name), cx, cy, bw, bh, a);
}

/* A button's picture, idle or held */
/* ---------- the plain pad: plates and letters, as Winlator's ----------
 * Shapes 6 and up are not outlines over the game's art: the control is a plate of its own -- a dark see-through
 * fill, a hairline in the button's colour, its name inside. 6 a wide rectangle, 7 a circle, 8 a rounded square.
 * The default layout uses 6 on every button and combo: one plate, one size, aligned, nothing behind them. */
static void badge_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);   /* (drawn further down) */
static void draw_style_outline(float cx, float cy, float r, float asp, int color, int shape, bool selected, float a);

static bool plain_shape(int shape) { return shape >= 6; }

/* Its half size: the wide rectangle is the one the thumb wants, 1.55 as wide as it is tall. */
static void plain_size(int shape, float r, float *hw, float *hh)
{
    *hh = r;
    *hw = shape == 6 ? r * 1.55f : r;
}

/* palette as the editor's (TouchLayoutEditor.java COLOUR_RGB): white, blue, red, green, gold, violet */
static void plain_tint(int colour, uint8_t *r, uint8_t *g, uint8_t *b)
{
    static const uint8_t tint[6][3] = {
        {255, 255, 255}, {76, 155, 234}, {224, 90, 90}, {87, 199, 122}, {227, 179, 65}, {142, 111, 216},
    };
    int ci = colour < 0 || colour > 5 ? 0 : colour;
    *r = tint[ci][0];
    *g = tint[ci][1];
    *b = tint[ci][2];
}

/* The name inside the plate, from the stand-in buttons' letters (launcher_font.h), scaled to fit. */
static void plain_letter(const char *t, float cx, float cy, float maxw, float maxh, uint8_t cr, uint8_t cg,
                         uint8_t cb, float a)
{
    if (!t || !*t || a <= 0.02f)
        return;
    /* In the same face as the rest of the recomp's words (user 2026-11-27: the dot matrix, a key's size on a
     * phone, came out ragged). Drawn once into a coverage box, then placed by its own bounds. */
    {
        static uint8_t cov[256 * 256];
        float px = maxh * 1.16f, w = ngb_ui_text(NULL, 0, 0, t, px, 1.f, 0, 0);
        if (w <= 0)
            goto dotted;
        if (maxw > 0 && w > maxw) {
            px *= maxw / w;
            w = maxw;
        }
        memset(cov, 0, sizeof cov);
        ngb_ui_text(cov, 256, 256, t, px, 1.f, 16.f, 210.f);
        int bx0 = 256, by0 = 256, bx1 = -1, by1 = -1;
        for (int y = 0; y < 256; y++)
            for (int x = 0; x < 256; x++)
                if (cov[y * 256 + x]) {
                    if (x < bx0) bx0 = x;
                    if (y < by0) by0 = y;
                    if (x > bx1) bx1 = x;
                    if (y > by1) by1 = y;
                }
        if (bx1 < 0)
            return;
        float bw = (float)(bx1 - bx0 + 1), bh = (float)(by1 - by0 + 1);
        float k = fminf(1.f, fminf(maxw > 0 ? maxw / bw : 1.f, maxh / bh));
        float ow = bw * k, oh = bh * k;
        float ox = cx - ow / 2 - bx0 * k, oy = cy - oh / 2 - by0 * k;   /* (its own box, centred) */
        for (int Y = (int)floorf(oy + by0 * k); Y < (int)ceilf(oy + (by1 + 1) * k); Y++)
            for (int X = (int)floorf(ox + bx0 * k); X < (int)ceilf(ox + (bx1 + 1) * k); X++) {
                int sx = (int)floorf((X + 0.5f - ox) / k), sy = (int)floorf((Y + 0.5f - oy) / k);
                if (sx < 0 || sy < 0 || sx > 255 || sy > 255)
                    continue;
                uint8_t v = cov[sy * 256 + sx];
                if (v)
                    badge_pixel(X, Y, cr, cg, cb, (uint8_t)lroundf(v * a));
            }
        return;
    }
dotted:;
    int n = (int)strlen(t), total = 0;
    for (int i = 0; i < n; i++) {
        int g = t[i] - 32;
        if (g >= 0 && g < 95)
            total += launcher_font_w[g];
    }
    if (!total)
        return;
    float k = maxh / LAUNCHER_FONT_H_PX;
    if (total * k > maxw)
        k = maxw / total;
    float gh = LAUNCHER_FONT_H_PX * k, pen = cx - total * k / 2, top = cy - gh / 2;
    for (int i = 0; i < n; i++) {
        int g = t[i] - 32;
        if (g < 0 || g >= 95) {
            pen += 2 * k;
            continue;
        }
        int gw = launcher_font_w[g];
        const uint8_t *src = launcher_font_px + launcher_font_off[g];
        for (int Y = 0; Y < (int)ceilf(gh); Y++)
            for (int X = (int)floorf(pen); X < (int)ceilf(pen + gw * k); X++) {
                float u = (X + 0.5f - pen) / k - 0.5f, v = (Y + 0.5f) / k - 0.5f;    /* (bilinear) */
                int iu = (int)floorf(u), iv = (int)floorf(v);
                float fu = u - iu, fv = v - iv, in = 0;
                for (int q = 0; q < 4; q++) {
                    int su = iu + (q & 1), sv = iv + (q >> 1);
                    if (su < 0 || sv < 0 || su >= gw || sv >= LAUNCHER_FONT_H_PX)
                        continue;
                    in += src[sv * gw + su] / 255.0f * ((q & 1) ? fu : 1 - fu) * ((q >> 1) ? fv : 1 - fv);
                }
                if (in > 0.02f)
                    badge_pixel(X, (int)floorf(top) + Y, cr, cg, cb,
                                (uint8_t)lroundf(fminf(1.f, in * a) * 255));
            }
        pen += gw * k;
    }
}

/* The button a key presses, as the theme names it: on Switch the two pairs trade, unless the player bound
 * that key himself (then his binding is the one that stands). */
static int theme_action(int id, int act)
{
    if (s_pad_theme != 1 || act != own[id]) return act;
    switch (act) {
    case PA_A: return PA_B;
    case PA_B: return PA_A;
    case PA_X: return PA_Y;
    case PA_Y: return PA_X;
    }
    return act;
}

/* The word a key wears for the theme: the pad's own on Xbox, the console's on the others. */
static const char *theme_label(int id, const char *own)
{
    if (s_pad_theme == 1) {
        switch (id) {   /* (Switch's A is where the pad has B, its X where the pad has Y) */
        case BA: return "B";
        case BB: return "A";
        case BX: return "Y";
        case BY: return "X";
        case LB: return "L";
        case RB: return "R";
        case LT: return "ZL";
        case RT: return "ZR";
        case BACK: return "-";
        case START: return "+";
        }
    } else if (s_pad_theme == 2) {
        switch (id) {
        case LB: return "L1";
        case RB: return "R1";
        case LT: return "L2";
        case RT: return "R2";
        case BACK: return "SHARE";
        case START: return "OPT";
        }
    }
    return own;
}

/* PlayStation's four marks, drawn: a cross, a circle, a square, a triangle (ids as the pad has them). */
static void mark_shape(int id, float cx, float cy, float r, uint8_t cr, uint8_t cg, uint8_t cb, float a)
{
    float th = fmaxf(1.4f, r * 0.26f);
    int x0 = (int)floorf(cx - r - 2), x1 = (int)ceilf(cx + r + 2);
    int y0 = (int)floorf(cy - r - 2), y1 = (int)ceilf(cy + r + 2);
    over_box(x0, y0, x1 + 1, y1 + 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d, band;
            if (id == BA) {                       /* the cross */
                if (fmaxf(fabsf(dx), fabsf(dy)) > r) continue;
                d = fabsf(fabsf(dx) - fabsf(dy)) * 0.70710678f;
                band = th * 0.5f;
            } else if (id == BB) {                /* the circle */
                d = fabsf(hypotf(dx, dy) - r * 0.82f);
                band = th * 0.45f;
            } else if (id == BX) {                /* the square */
                d = fabsf(fmaxf(fabsf(dx), fabsf(dy)) - r * 0.76f);
                band = th * 0.45f;
            } else {                              /* the triangle, point up */
                float ay = cy - r, by = cy + r * 0.72f;
                float t = (y + 0.5f - ay) / (by - ay);
                if (t < 0.f || t > 1.f || t > 0.92f) continue;
                d = fabsf(fabsf(dx) - r * 1.05f * t);
                band = th * 0.45f;
            }
            if (d <= band)
                badge_pixel(x, y, cr, cg, cb, (uint8_t)lroundf(fminf(1.f, a) * 255));
        }
}

static void draw_plain(int id, float cx, float cy, float r, float a, bool lit)
{
    const Ctrl *c = &s_ctrl[id];
    int shape = plain_shape(d_layout_shape[id]) ? d_layout_shape[id] : 6;
    float hw, hh;
    plain_size(shape, r, &hw, &hh);
    hw *= d_layout_asp[id];   /* (as long as the editor stretched it) */
    float rad = shape == 6 || shape == 8 ? fminf(hw, hh) * 0.22f : hw;
    /* (the pad's look: 0 hairline, 1 glass, 2 bold ring, 3 solid -- user 2026-11-27) */
    float edge_k = s_pad_style == 2 ? 0.100f : s_pad_style == 3 ? 0.050f : s_pad_style == 1 ? 0.075f : 0.040f;
    float edge = fmaxf(1.5f, r * edge_k);
    uint8_t cr, cg, cb;
    plain_tint(d_layout_color[id], &cr, &cg, &cb);
    int x0 = (int)floorf(cx - hw - 2), x1 = (int)ceilf(cx + hw + 2);
    int y0 = (int)floorf(cy - hh - 2), y1 = (int)ceilf(cy + hh + 2);
    over_box(x0, y0, x1 + 1, y1 + 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d;
            if (shape == 7) {
                d = hypotf(dx, dy) - hw;
            } else {
                float qx = fabsf(dx) - hw + rad, qy = fabsf(dy) - hh + rad;
                d = hypotf(fmaxf(qx, 0.f), fmaxf(qy, 0.f)) + fminf(fmaxf(qx, qy), 0.f) - rad;
            }
            if (d > 0)
                continue;
            /* (the hairline has no plate at all, the glass a dark one -- user 2026-11-27) */
            float plate = s_pad_style == 3 ? (lit ? 0.97f : 0.92f)
                        : s_pad_style == 1 ? (lit ? 0.78f : 0.62f)
                        : s_pad_style == 2 ? 0.f
                        : (lit ? 0.18f : 0.09f);
            badge_pixel(x, y, 10, 11, 14, (uint8_t)lroundf(plate * a * 255));
            if (d > -edge)                                                                    /* its ring */
                badge_pixel(x, y, cr, cg, cb, (uint8_t)lroundf((lit ? 0.95f : 0.72f) * a * 255));
            else if (d > -edge * 3.0f)     /* (a thin light inside the ring: a pad's keys sit in a dish) */
                badge_pixel(x, y, 255, 255, 255, (uint8_t)lroundf(0.10f * a * 255));
        }
    /* (white on the dark plate, as the console's own keys have it -- the key's colour stays on its ring) */
    if (s_pad_theme == 2 && id >= BA && id <= BY)
        mark_shape(id, cx, cy, fminf(hw, hh) * 0.36f, 240, 243, 249, fminf(1.f, a + 0.2f));
    else if (c->label)
        plain_letter(theme_label(id, c->label), cx, cy, hw * 1.15f, hh * 0.46f, 240, 243, 249,
                     fminf(1.f, a + 0.25f));   /* (small again: a key's name is a mark, not a poster -- user) */
}

static void draw_badge_text(const char *label, int centre, int y0, int bw);
static const char *combo_first(int id) {
    static const char *v[6] = {"button_a","button_a","button_a","button_b","button_b","button_x"};
    return v[d_layout_combo[id] % 6];
}
static const char *combo_second(int id) {
    static const char *v[6] = {"button_b","button_x","button_y","button_x","button_y","button_y"};
    return v[d_layout_combo[id] % 6];
}
static void draw_combo(const Ctrl *c, int id, float cx, float cy, float r, float a) {
    float ar = d_layout_asp[id];
    if (s_art == ART_GAME && !d_layout_skin[id]) {
        draw_piece(combo_first(id), cx - 0.38f*r*ar, cy, 1.65f*r*ar, 1.65f*r, a);
        draw_piece(combo_second(id), cx + 0.38f*r*ar, cy, 1.65f*r*ar, 1.65f*r, a);
    } else if (d_layout_skin[id] && user_skin(id)) { draw_sprite(user_skin(id), cx, cy, 2.5f*r*ar, 2.5f*r, a);
        if (c->label && c->label[0] && strlen(c->label) <= 4)
            plain_letter(c->label, cx, cy, 2.75f*r*ar, 1.9f*r, 255, 255, 255, fminf(1.f, a * 0.9f)); }
    else {
        draw_sprite(standin_button(id), cx, cy, 2.5f*r, 2.5f*r, a);
        if (c->label && c->label[0] && strlen(c->label) <= 5)
            plain_letter(c->label, cx, cy, 2.85f*r, 1.8f*r, 255, 255, 255, fminf(1.f, a + 0.3f));
    }
}

static void draw_button(const Ctrl *c, bool lit, float cx, float cy, float r, float a)
{
    if (s_art == ART_GAME && !d_layout_skin[(int)(c - s_ctrl)]) {
        char n[40];
        snprintf(n, sizeof n, lit ? "%s_lit" : "%s", c->piece);
        draw_piece(n, cx, cy, 2.5f * r, 2.5f * r, a);
    } else {   /* (the disc about the button's size) */
        int id = (int)(c - s_ctrl);
        const Sprite *skin = d_layout_skin[id] ? user_skin(id) : NULL;
        if (skin) {
            float h = (lit ? 2.6f : 2.5f) * r, w = h * d_layout_asp[id];
            draw_sprite(skin, cx, cy, w, h, a);
            /* (the name stays readable on the picture, faint) */
            if (c->label && c->label[0] && strlen(c->label) <= 4)
                plain_letter(theme_label(id, c->label), cx, cy, w * 1.1f, h * 0.75f, 255, 255, 255,
                             fminf(1.f, a * 0.9f));
        } else {
            /* the stand-in picture: the name on it, or A, B, X and Y would be one picture (user 2026-11-27) */
            float h = (lit ? 2.6f : 2.5f) * r, w = h * d_layout_asp[id];
            draw_sprite(standin_button(id), cx, cy, w, h, a);
            if (c->label && c->label[0] && strlen(c->label) <= 5)
                plain_letter(theme_label(id, c->label), cx, cy, w * 1.15f, h * 0.72f, 255, 255, 255,
                             fminf(1.f, a + 0.3f));
        }
        if (lit)
            draw("touch_button_lit", cx, cy, 2.3f * r, 2.3f * r, 0.9f);
    }
}

#define IDLE 0.55f   /* how see-through a control is, untouched */

/* The controls untouched, the sticks without their knobs: drawn once for each size, then each picture starts
 * from it and adds only the knobs and what is held (a stick dragged redraws a few small pictures, not all) */
static uint32_t *s_bg;
static int s_bg_w, s_bg_h;
static unsigned s_bg_layout_revision;
static float s_bg_aspect;
static enum Art s_bg_art;
static int s_bg_layout = -1;
static float s_bg_opacity = -1;

static void draw_idle(void)
{
    float u = (float)s_h;
    /* (a menu or the settings page is up: the keys go faint so its words read through them, and they are still
     * there to be pressed -- user 2026-11-27) */
    float dim = d_menu_dim ? 0.26f : 1.f;
    memset(s_px, 0, sizeof(uint32_t) * (size_t)s_w * s_h);
    for (int i = 0; i < NCTRL; i++) {
        if (!d_layout_enabled[i]) continue;
        const Ctrl *c = &s_ctrl[i];
        Place at = place_at(c, d_aspect, s_art == ART_STANDIN, d_layout);
        if (d_layout_custom[i]) { at.x=c->edge==2?d_aspect/2+d_anim_x[i]:c->edge?d_aspect-d_anim_x[i]:d_anim_x[i]; at.y=d_anim_y[i]; at.r=d_anim_r[i]; }
        float cx = at.x * u, cy = at.y * u, r = at.r * u;
        if (plain_shape(d_layout_shape[i]) && (c->kind == BUTTON || c->kind == COMBO)) {
            float a1 = (d_edit_mode && i == d_selected ? 0.98f : d_layout_alpha[i]) * dim;
            const Sprite *sk = d_layout_skin[i] ? user_skin(i) : NULL;
            if (sk) {                                  /* (the picture the player picked wins over the plate) */
                float h2 = 2.5f * r, w2 = h2 * d_layout_asp[i];
                draw_sprite(sk, cx, cy, w2, h2, a1);
                if (c->label && c->label[0] && strlen(c->label) <= 4)
                    plain_letter(theme_label(i, c->label), cx, cy, w2 * 1.1f, h2 * 0.75f, 255, 255, 255,
                                 fminf(1.f, a1 * 0.9f));
            } else
                draw_plain(i, cx, cy, r, a1, false);
        }
        else if (c->kind == STICK) {
            /* (the same ring the keys wear, in the same colour -- and the key's own opacity, which the rings
             * used to ignore: 58% is the pad's default, 0 leaves nothing, user 2026-11-27) */
            float la = fminf(1.f, fmaxf(0.f, d_layout_alpha[i] / 0.58f));
            draw_style_outline(cx, cy, r, 1.f, 0, 7, false, dim * la);
            draw_style_outline(cx, cy, r * 0.66f, 1.f, 0, 7, false, dim * 0.45f * la);   /* (one look for both) */
            draw_style_outline(cx, cy, r * 0.12f, 1.f, d_layout_color[i], 7, false, dim * la);
        } else if (c->kind == DPAD) {
            float la = fminf(1.f, fmaxf(0.f, d_layout_alpha[i] / 0.58f));
            draw_style_outline(cx, cy, r, 1.f, d_layout_color[i], 7, false, dim * la);
            for (int q = 0; q < 4; q++) {
                float dx = q == 0 ? 0 : q == 1 ? 0 : q == 2 ? -0.55f : 0.55f;
                float dy = q == 2 || q == 3 ? 0 : q == 0 ? -0.55f : 0.55f;
                draw_style_outline(cx + dx * r, cy + dy * r, r * 0.16f, 1.f, d_layout_color[i], 7, false, dim * la);
            }
        }
        else if (c->kind == COMBO)
            draw_combo(c, i, cx, cy, r, (d_edit_mode && i == d_selected ? 0.98f : d_layout_alpha[i]) * dim);
        else
            draw_button(c, false, cx, cy, r, (d_edit_mode && i == d_selected ? 0.98f : d_layout_alpha[i]) * dim);   /* (the face buttons
                                                       a little less see-through: X and A were faint) */
    }
}

static void badge_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (x < 0 || y < 0 || x >= s_w || y >= s_h || !a) return;
    uint32_t *p = &s_px[(size_t)y * s_w + x];
    unsigned inv = 255 - a;
    uint32_t old = *p;
    uint8_t ob = (uint8_t)(old & 255), og = (uint8_t)(old >> 8), orr = (uint8_t)(old >> 16), oa = (uint8_t)(old >> 24);
    uint8_t nb = (uint8_t)((b * a + ob * inv + 127) / 255);
    uint8_t ng = (uint8_t)((g * a + og * inv + 127) / 255);
    uint8_t nr = (uint8_t)((r * a + orr * inv + 127) / 255);
    uint8_t na = (uint8_t)(a + (oa * inv + 127) / 255);
    *p = (uint32_t)nb | (uint32_t)ng << 8 | (uint32_t)nr << 16 | (uint32_t)na << 24;
}


static void draw_style_outline(float cx, float cy, float r, float asp, int color, int shape, bool selected, float a)
{
    if (!selected && color == 0 && shape == 0) return;
    /* (the editor's own palette: a ring that stays the colour the player picked -- it was a fixed blue, and
     * the key looked blue in the game and gold on the editor's page -- user 2026-11-27) */
    uint8_t cr, cg, cb;
    if (selected) { cr = 255; cg = 255; cb = 255; }
    else plain_tint(color, &cr, &cg, &cb);
    float outer = r * 1.20f, inner = outer - fmaxf(1.5f, r * 0.055f);
    int x0 = (int)floorf(cx - outer * asp - 1), x1 = (int)ceilf(cx + outer * asp + 1);
    int y0 = (int)floorf(cy - outer - 1), y1 = (int)ceilf(cy + outer + 1);
    over_box(x0, y0, x1 + 1, y1 + 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = fabsf(x + 0.5f - cx) / (asp > 0.01f ? asp : 1.f), dy = fabsf(y + 0.5f - cy), d;
            if (shape == 1) d = fmaxf(dx, dy);                                   /* square */
            else if (shape == 2) d = (dx + dy) * 0.70710678f;                    /* diamond */
            else if (shape == 3) d = powf(dx * dx * dx * dx + dy * dy * dy * dy, 0.25f);   /* rounded square */
            else if (shape == 4) d = fmaxf(dx * 0.866025f + dy * 0.5f, dy);       /* hexagon */
            else if (shape == 5) d = fmaxf(fmaxf(dx, dy), (dx + dy) * 0.70710678f);/* octagon */
            else if (shape == 6) d = fmaxf(dx / 1.55f, dy);                      /* wide rectangle */
            else if (shape == 8) d = powf(dx * dx * dx * dx + dy * dy * dy * dy, 0.25f);   /* rounded square */
            else d = hypotf(dx, dy);
            if (d >= inner && d <= outer)
                badge_pixel(x, y, cr, cg, cb, (uint8_t)(230.f * a));
        }
}


static void fill_disc(float cx, float cy, float r, uint8_t cr, uint8_t cg, uint8_t cb, float a)
{
    if (r < 0.5f)
        return;
    int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1);
    int y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
    over_box(x0, y0, x1 + 1, y1 + 1);
    float r2 = (r + 0.5f) * (r + 0.5f);
    uint8_t aa = (uint8_t)lroundf(fminf(1.f, a) * 255.f);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (dx * dx + dy * dy <= r2)
                badge_pixel(x, y, cr, cg, cb, aa);
        }
}

static void draw_badge_text(const char *label, int centre, int y0, int bw)
{
    const int bh = 18, x0 = centre - bw / 2;
    over_box(x0, y0, x0 + bw, y0 + bh);
    for (int y = y0; y < y0 + bh; y++)
        for (int x = x0; x < x0 + bw; x++) {
            bool edge = x == x0 || x == x0 + bw - 1 || y == y0 || y == y0 + bh - 1;
            badge_pixel(x, y, edge ? 55 : 12, edge ? 166 : 24, edge ? 255 : 42, edge ? 238 : 218);
        }
    int total = 0, n = (int)strlen(label);
    for (int i = 0; i < n; i++) total += launcher_font_w[label[i] - 32];
    float scale = fminf(0.62f, fminf((bw - 12.0f) / (total ? total : 1), 10.0f / LAUNCHER_FONT_H_PX));
    float pen = centre - total * scale / 2.0f;
    int top = y0 + (bh - (int)ceilf(LAUNCHER_FONT_H_PX * scale)) / 2;
    for (int i = 0; i < n; i++) {
        int g = label[i] - 32, gw = launcher_font_w[g];
        const uint8_t *src = launcher_font_px + launcher_font_off[g];
        for (int sy = 0; sy < LAUNCHER_FONT_H_PX; sy++)
            for (int sx = 0; sx < gw; sx++) {
                uint8_t a = src[sy * gw + sx];
                if (a) badge_pixel((int)floorf(pen + sx * scale), top + (int)floorf(sy * scale), 248, 248, 250, a);
            }
        pen += gw * scale;
    }
}


/* The picture is drawn in memory of our own and its changed boxes copied into the presenter's buffer: blended
 * in that buffer itself (the GPU's, mapped: slow to read on a phone) a dragged stick took a Pixel 4a from 60 fps
 * to 22 at the window's own size. */
static uint32_t *s_cv;
static Over s_cvrec;

static void box_copy(uint32_t *to, const uint32_t *from, const Over *o)
{
    for (int b = 0; b < o->n; b++)
        for (int y = o->box[b][1]; y < o->box[b][3]; y++)
            memcpy(to + (size_t)y * s_w + o->box[b][0], from + (size_t)y * s_w + o->box[b][0],
                   sizeof(uint32_t) * (size_t)(o->box[b][2] - o->box[b][0]));
}

static void draw_all(void)
{
    float u = (float)s_h;
    size_t px = (size_t)s_w * s_h;
    bool fresh = false;
    uint32_t *out = s_px;
    if (!s_bg || !s_cv || s_bg_w != s_w || s_bg_h != s_h || s_bg_aspect != d_aspect || s_bg_art != s_art ||
        s_bg_layout != d_layout || s_bg_layout_revision != d_layout_revision || s_bg_opacity != d_opacity) {
        free(s_bg);
        free(s_cv);
        s_bg = malloc(sizeof(uint32_t) * px);
        s_cv = malloc(sizeof(uint32_t) * px);
        if (!s_bg || !s_cv) {
            free(s_bg);
            free(s_cv);
            s_bg = s_cv = NULL;
            s_bg_gen++;
            s_chg_n = -1;
            memset(out, 0, sizeof(uint32_t) * px);   /* (no memory: nothing shown) */
            return;
        }
        s_px = s_cv;
        draw_idle();
        memcpy(s_bg, s_cv, sizeof(uint32_t) * px);
        s_bg_w = s_w;
        s_bg_h = s_h;
        s_bg_aspect = d_aspect;
        s_bg_art = s_art;
        s_bg_layout = d_layout;
        s_bg_layout_revision = d_layout_revision;
        s_bg_opacity = d_opacity;
        s_bg_gen++;
        fresh = true;
    } else if (s_cvrec.n >= 0) {
        box_copy(s_cv, s_bg, &s_cvrec);       /* (what was over the untouched controls: taken away) */
    } else {
        memcpy(s_cv, s_bg, sizeof(uint32_t) * px);
    }
    s_px = s_cv;
    Over was = s_cvrec;                       /* (the picture before this one) */
    bool was_ok = !fresh && was.n >= 0;
    s_cvrec.n = 0;
    s_rec = &s_cvrec;
    for (int i = 0; i < NCTRL; i++) {
        if (!d_layout_enabled[i]) continue;
        const Ctrl *c = &s_ctrl[i];
        Place at = place_at(c, d_aspect, s_art == ART_STANDIN, d_layout);
        if (d_layout_custom[i]) { at.x=c->edge==2?d_aspect/2+d_anim_x[i]:c->edge?d_aspect-d_anim_x[i]:d_anim_x[i]; at.y=d_anim_y[i]; at.r=d_anim_r[i]; }
        float cx = at.x * u, cy = at.y * u, r = at.r * u;
        bool on = d_down[i];
        if (on && plain_shape(d_layout_shape[i]) && (c->kind == BUTTON || c->kind == COMBO) &&
            !(d_layout_skin[i] && user_skin(i))) {
            draw_plain(i, cx, cy, r, fmaxf(d_layout_alpha[i], 0.78f), true);   /* (the plate lit under the thumb) */
        } else if (c->kind == STICK) {
            /* (the ring is in the idle picture; only the knob moves, a disc in the key's own colour -- the grey
             * Kenney pad and nub were the one thing on the pad that did not match, user 2026-11-27) */
            int s = i == RSTICK;
            float nx = cx + d_stick[s][0] * r * 0.52f, ny = cy + d_stick[s][1] * r * 0.52f;
            /* (the centre was a pinhead -- user 2026-11-27: a stick's cap is most of its inside) */
            float la = fminf(1.f, fmaxf(0.f, d_layout_alpha[i] / 0.58f));   /* the cap follows the key too */
            fill_disc(nx, ny, r * (on ? 0.42f : 0.36f), 232, 236, 243, fminf(1.f, (on ? 0.95f : 0.70f) * la));
            if (on)
                draw_style_outline(cx, cy, r * 0.66f, 1.f, 0, 7, false, fminf(1.f, 0.90f * la));
        } else if (c->kind == DPAD && on) {
            static const struct { int bit; float dx, dy; } arms[] = {
                { XBOX_GAMEPAD_DPAD_UP, 0, -1 },
                { XBOX_GAMEPAD_DPAD_DOWN, 0, 1 },
                { XBOX_GAMEPAD_DPAD_LEFT, -1, 0 },
                { XBOX_GAMEPAD_DPAD_RIGHT, 1, 0 },
            };
            draw_style_outline(cx, cy, r, 1.f, 0, 7, false, 0.55f);   /* (white, as the sticks: one look) */
            for (int a = 0; a < 4; a++)
                if (d_dpad & arms[a].bit)
                    fill_disc(cx + arms[a].dx * r * 0.55f, cy + arms[a].dy * r * 0.55f, r * 0.17f, 232, 236, 243,
                              0.95f);
        } else if (c->kind == COMBO && on) {
            draw_combo(c, i, cx, cy, r, fmaxf(d_layout_alpha[i], 0.85f));
            draw("touch_button_lit", cx, cy, 2.1f*r, 2.1f*r, 0.9f);
        } else if (c->kind == BUTTON && on) {
            draw_button(c, true, cx, cy, r, 1.0f);   /* (the game's pieces: their own held picture) */
        }
    }
    for (int i=0;i<NCTRL;i++) { if(!d_layout_enabled[i]) continue; const Ctrl *c=&s_ctrl[i];
        if(!plain_shape(d_layout_shape[i])&&d_layout_skin[i]&&c->kind==BUTTON) { Place p=place_at(c,d_aspect,s_art==ART_STANDIN,d_layout); draw("touch_button_lit",p.x*u,p.y*u,2.2f*p.r*u,2.2f*p.r*u,0.30f); }
        /* (the rings live in the idle picture: drawing them again every frame cost the pad real time -- user
         * 2026-11-27. Only the one the editor has selected is drawn here, over the picture) */
        if (d_edit_mode && i == d_selected) { Place p=place_at(c,d_aspect,s_art==ART_STANDIN,d_layout);
            draw_style_outline(p.x*u,p.y*u,p.r*u,d_layout_asp[i],d_layout_color[i],d_layout_shape[i],true,1.f); }
    }
    s_rec = NULL;
    s_px = out;
    /* Into the presenter's buffer: where it differs from the picture that buffer holds (drawn some frames ago) */
    Over *o = NULL;
    for (int i = 0; i < OVER_BUFS; i++)
        if (s_over[i].px == out)
            o = &s_over[i];
    if (!o) {            /* (a buffer not seen before: in the place of the one least lately drawn) */
        static int next;
        o = &s_over[next++ % OVER_BUFS];
        o->px = out;
        o->n = -1;
    }
    if (o->n >= 0 && s_cvrec.n >= 0 && o->bg == s_bg_gen && o->w == s_w && o->h == s_h) {
        box_copy(out, s_cv, o);
        box_copy(out, s_cv, &s_cvrec);
    } else {
        memcpy(out, s_cv, sizeof(uint32_t) * px);
    }
    o->bg = s_bg_gen;
    o->w = s_w;
    o->h = s_h;
    o->n = s_cvrec.n;
    if (s_cvrec.n > 0)
        memcpy(o->box, s_cvrec.box, sizeof(int[4]) * (size_t)s_cvrec.n);
    /* Where this picture differs from the one before it: what either has over the untouched controls */
    if (was_ok && s_cvrec.n >= 0) {
        memcpy(s_chg, was.box, sizeof(int[4]) * (size_t)was.n);
        memcpy(s_chg + was.n, s_cvrec.box, sizeof(int[4]) * (size_t)s_cvrec.n);
        s_chg_n = was.n + s_cvrec.n;
    } else {
        s_chg_n = -1;
    }
}

/* The presenter's, after a picture it was given: the boxes (x0, y0, x1, y1) where it differs from the one given
 * before, so only those go to the GPU (the whole of it, ten megabytes at a phone's size, on each step of a
 * dragged stick took the game to 22 fps on a Pixel 4a). -1: all of it. */
int nv2a_overlay_changed(int (*box)[4], int max)
{
    if (s_chg_n < 0 || s_chg_n > max)
        return -1;
    memcpy(box, s_chg, sizeof(int[4]) * (size_t)s_chg_n);
    return s_chg_n;
}

/* Settings-menu bridge for the dedicated Android touch-control centre. */
static int touch_menu_combo(int id) { return s_layout_combo[id]; }
int ngb_touch_menu_get(int item)
{
    int value=0;
    pthread_mutex_lock(&s_lock);
    layout_load();
    int id=s_selected>=0&&s_selected<NCTRL?s_selected:LSTICK;
    switch(item) {
    case 1:value=s_preset;break;
    case 2:value=s_haptics;break;
    case 3:value=s_camera_swipe;break;
    case 4:if(s_selected<0)s_selected=LSTICK;value=s_selected;break;
    case 5:value=s_layout_enabled[id];break;
    case 6:value=(int)lroundf(s_layout_r[id]/s_ctrl[id].r*100.f);break;
    case 7:value=(int)lroundf(s_layout_alpha[id]*100.f);break;
    case 8:value=s_layout_color[id];break;
    case 9:value=s_layout_shape[id];break;
    case 10:value=s_layout_skin[id];break;
    case 11:value=touch_menu_combo(NINPO);break;
    case 12:value=touch_menu_combo(WINDRUN);break;
    case 13:value=touch_menu_combo(CUSTOM1);break;
    case 14:value=touch_menu_combo(CUSTOM2);break;
    case 15:value=touch_menu_combo(CUSTOM3);break;
    case 16:value=touch_menu_combo(CUSTOM4);break;
    default:break;
    }
    pthread_mutex_unlock(&s_lock);
    return value;
}
void ngb_touch_menu_change(int item,int dir)
{
    pthread_mutex_lock(&s_lock);
    layout_load();
    int id=s_selected>=0&&s_selected<NCTRL?s_selected:LSTICK;
    switch(item) {
    case 1: layout_switch_preset(); break;
    case 2: s_haptics=!s_haptics; break;
    case 3: s_camera_swipe=!s_camera_swipe; break;
    case 4: s_selected=(id+(dir<0?NCTRL-1:1))%NCTRL; break;
    case 5: s_layout_enabled[id]=!s_layout_enabled[id]; break;
    case 6: s_layout_r[id]=s_ctrl[id].r*fminf(2.f,fmaxf(.5f,s_layout_r[id]/s_ctrl[id].r+(dir<0?-.1f:.1f)));s_layout_custom[id]=true;break;
    case 7: s_layout_alpha[id]=fminf(1.f,fmaxf(0.f,s_layout_alpha[id]+(dir<0?-.05f:.05f)));break;
    case 8: s_layout_color[id]=(uint8_t)((s_layout_color[id]+(dir<0?5:1))%6);break;
    case 9: s_layout_shape[id]=(uint8_t)((s_layout_shape[id]+(dir<0?8:1))%9);break;
    case 10: s_layout_skin[id]^=1;break;
    case 11:s_layout_combo[NINPO]=(uint8_t)((s_layout_combo[NINPO]+(dir<0?6:1))%7);break;
    case 12:s_layout_combo[WINDRUN]=(uint8_t)((s_layout_combo[WINDRUN]+(dir<0?6:1))%7);break;
    case 13:case 14:case 15:case 16:{int ci=CUSTOM1+item-13;s_layout_combo[ci]=(uint8_t)((s_layout_combo[ci]+(dir<0?6:1))%7);break;}
    default:break;
    }
    if(item!=1){layout_save();s_layout_revision++;s_serial++;}
    pthread_mutex_unlock(&s_lock);
}
void ngb_touch_menu_set(int item,int value)
{
    pthread_mutex_lock(&s_lock);
    layout_load();
    int id=s_selected>=0&&s_selected<NCTRL?s_selected:LSTICK;
    if(item==1){
        int target=(value%3+3)%3;
        for(int n=0;n<3&&s_preset!=target;n++)layout_switch_preset();
    }else if(item==2)s_haptics=value!=0;
    else if(item==3)s_camera_swipe=value!=0;
    else if(item==4)s_selected=value<0?0:value>=NCTRL?NCTRL-1:value;
    else if(item==5)s_layout_enabled[id]=value!=0;
    else if(item==6){s_layout_r[id]=s_ctrl[id].r*fminf(2.f,fmaxf(.5f,(float)value/100.f));s_layout_custom[id]=true;}
    else if(item==7)s_layout_alpha[id]=fminf(1.f,fmaxf(0.f,(float)value/100.f));
    else if(item==8)s_layout_color[id]=(uint8_t)((value%6+6)%6);
    else if(item==9)s_layout_shape[id]=(uint8_t)((value%9+9)%9);
    else if(item==10)s_layout_skin[id]=value!=0;
    else if(item>=11&&item<=16){int ci=item==11?NINPO:item==12?WINDRUN:CUSTOM1+item-13;s_layout_combo[ci]=(uint8_t)((value%7+7)%7);}
    layout_save();s_layout_revision++;s_serial++;
    pthread_mutex_unlock(&s_lock);
}

void ngb_touch_menu_activate(int item)
{
    pthread_mutex_lock(&s_lock);
    if(item==0){                                   /* the settings page's first row: hand it to the editor */
        pthread_mutex_unlock(&s_lock);
        ngb_touch_editor_open();
        return;
    }
    layout_load();
    int id=s_selected>=0&&s_selected<NCTRL?s_selected:LSTICK;
    if(item==17){s_selected=id;touch_pick_skin();}
    else if(item==18) add_custom_combo();
    else if(item==19) delete_selected_combo();
    else if(item==10) s_layout_skin[id]^=1;
    else if(item==5) s_layout_enabled[id]=!s_layout_enabled[id];
    if(item==5||item==10||item==17){layout_save();s_layout_revision++;s_serial++;}
    pthread_mutex_unlock(&s_lock);
}

/* The layout as the phone's editor reads it and writes it back: a "V4 <aspect>" line, then one line per control,
 * "<id> <x> <y> <r> <alpha> <colour> <shape> <action> <combo> <shown> <skin>", x and y its centre across and down
 * the screen (0..1 of the width and of the height), r its radius down the screen (0..1 of the height). */
void ngb_touch_layout_get(char *out, size_t n)
{
    if (!n)
        return;
    pthread_mutex_lock(&s_lock);
    layout_load();
    layout_known();
    float aspect = s_aspect > 0.1f ? s_aspect : 16.0f / 9;
    int k = snprintf(out, n, "V4 %.4f %u\n", aspect, s_pad_style);
    if (k >= 0 && k < (int)n)
        k += snprintf(out + k, n - (size_t)k, "# THEME %d\n", s_pad_theme);   /* (the editor draws what the game does) */
    for (int i = 0; i < NCTRL; i++)
        if (s_turbo[i] && k >= 0 && k < (int)n)
            k += snprintf(out + k, n - (size_t)k, "# TURBO %d %d\n", i, s_turbo[i]);
    for (int i = 0; i < NCTRL && k > 0; i++) {
        if ((size_t)k >= n)
            break;
        Place at = place(&s_ctrl[i]);
        k += snprintf(out + k, n - (size_t)k, "%d %.5f %.5f %.5f %.5f %u %u %d %u %u %u %.3f\n",
                      i, at.x / aspect, at.y, at.r, s_layout_alpha[i], s_layout_color[i], s_layout_shape[i],
                      s_layout_action[i], s_layout_combo[i], s_layout_enabled[i] ? 1 : 0, s_layout_skin[i] ? 1 : 0,
                      s_layout_asp[i]);
    }
    /* the pictures themselves, so the editor can show and keep them */
    for (int i = 0; i < NCTRL && k > 0 && (size_t)k < n; i++)
        if (s_skin_path[i][0])
            k += snprintf(out + k, n - (size_t)k, "# SKIN %d %s\n", i, s_skin_path[i]);
    pthread_mutex_unlock(&s_lock);
}

/* Everything the editor changed, in one go: the controls it sent keep the places it gave them. */
void ngb_touch_layout_apply(const char *text)
{
    if (!text)
        return;
    pthread_mutex_lock(&s_lock);
    layout_load();
    layout_known();
    /* (the editor sends the whole auto-fire picture under one marker: a key it no longer lists is off) */
    if (strstr(text, "# TURBO-ALL"))
        memset(s_turbo, 0, sizeof s_turbo);
    float aspect = s_aspect > 0.1f ? s_aspect : 16.0f / 9;
    const char *line = text;
    while (line && *line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len > 6 && line[0] == '#') {
            int sk; char sp[300];
            if (sscanf(line, "# SKIN %d %299s", &sk, &sp[0]) == 2 && sk >= 0 && sk < NCTRL)
                snprintf(s_skin_path[sk], sizeof s_skin_path[sk], "%s", sp);
            {   /* auto-fire: a key held repeats by itself */
                int tid = -1, hz = 0;
                if (sscanf(line, "# TURBO %d %d", &tid, &hz) == 2 && tid >= 0 && tid < NCTRL)
                    s_turbo[tid] = (uint8_t)(hz < 0 ? 0 : hz > 20 ? 20 : hz);
            }
            line = end ? end + 1 : NULL;
            continue;
        }
        if (len && line[0] == 'V') {
            int st = 0;
            if (sscanf(line, "V4 %*f %d", &st) == 1) s_pad_style = (uint8_t)(st < 0 ? 0 : st > 3 ? 3 : st);
        }
        if (len && line[0] != 'V' && line[0] != 'M') {
            char row[256];
            if (len >= sizeof row)
                len = sizeof row - 1;
            memcpy(row, line, len);
            row[len] = 0;
            int id, colour, shape, action, combo, shown, skin;
            float x, y, r, alpha, asp = 1.f;
            int read = sscanf(row, "%d %f %f %f %f %d %d %d %d %d %d %f", &id, &x, &y, &r, &alpha, &colour,
                              &shape, &action, &combo, &shown, &skin, &asp);
            if (read >= 11 && id >= 0 && id < NCTRL) {
                const Ctrl *c = &s_ctrl[id];
                float across = x * aspect;   /* (as the height: what place_at works in) */
                s_layout_x[id] = c->edge == 2 ? across - aspect / 2 : c->edge == 1 ? aspect - across : across;
                if (c->edge == 2)
                    s_layout_x[id] = fminf(aspect / 2 - 0.04f, fmaxf(-aspect / 2 + 0.04f, s_layout_x[id]));
                else
                    s_layout_x[id] = fminf(aspect - 0.04f, fmaxf(0.04f, s_layout_x[id]));
                s_layout_y[id] = fminf(.97f, fmaxf(.03f, y));
                s_layout_r[id] = fminf(.25f, fmaxf(.012f, r));
                s_layout_alpha[id] = fminf(1.f, fmaxf(0.f, alpha));
                s_layout_color[id] = (uint8_t)(colour < 0 ? 0 : colour > 5 ? 5 : colour);
                s_layout_shape[id] = (uint8_t)(shape < 0 ? 0 : shape > 8 ? 8 : shape);
                s_layout_action[id] = action >= 0 && action < PA_COUNT ? action : PA_NONE;
                s_layout_combo[id] = (uint8_t)(combo < 0 ? 0 : combo > 5 ? 5 : combo);
                s_layout_enabled[id] = shown != 0;
                s_layout_skin[id] = skin != 0;
                if (read >= 12) s_layout_asp[id] = asp < 0.4f ? 0.4f : asp > 4.f ? 4.f : asp;
                s_layout_custom[id] = true;
            }
        }
        line = end ? end + 1 : NULL;
    }
    layout_save();
    s_layout_revision++;
    s_serial++;
    pthread_mutex_unlock(&s_lock);
}

#if defined(__ANDROID__)
extern int ngb_menu_row_get(int row);
extern void ngb_menu_row_change(int row,int dir);
extern void ngb_menu_row_value(int row,char *out,size_t n);
extern int ngb_touch_bind_count(void);
extern int ngb_touch_bind_slot(void);
extern void ngb_touch_bind_slot_change(int dir);
extern const char *ngb_touch_bind_name(int action);
extern int ngb_touch_bind_value(int action);
extern void ngb_touch_bind_capture(int action);
extern int ngb_touch_bind_capturing(void);
JNIEXPORT jint JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchGameGet(JNIEnv *env,jobject activity,jint row) {
    (void)env;(void)activity;return ngb_menu_row_get(row);
}
JNIEXPORT jstring JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchGameValue(JNIEnv *env,jobject activity,jint row) {
    (void)activity;char value[96]={0};ngb_menu_row_value(row,value,sizeof value);return (*env)->NewStringUTF(env,value);
}
JNIEXPORT void JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchGameChange(JNIEnv *env,jobject activity,jint row,jint dir) {
    (void)env;(void)activity;ngb_menu_row_change(row,dir);
}
JNIEXPORT jint JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchBindCount(JNIEnv *env,jobject activity) {
    (void)env;(void)activity;return ngb_touch_bind_count();
}
JNIEXPORT jint JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchBindSlot(JNIEnv *env,jobject activity) {
    (void)env;(void)activity;return ngb_touch_bind_slot();
}
JNIEXPORT void JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchBindSlotChange(JNIEnv *env,jobject activity,jint dir) {
    (void)env;(void)activity;ngb_touch_bind_slot_change(dir);
}
JNIEXPORT jstring JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchBindName(JNIEnv *env,jobject activity,jint action) {
    (void)activity;return (*env)->NewStringUTF(env,ngb_touch_bind_name(action));
}
JNIEXPORT jstring JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchBindValue(JNIEnv *env,jobject activity,jint action) {
    (void)activity;char value[64]={0};ngb_bind_input_name(ngb_touch_bind_value(action),value,sizeof value);return (*env)->NewStringUTF(env,value);
}
JNIEXPORT void JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchBindCapture(JNIEnv *env,jobject activity,jint action) {
    (void)env;(void)activity;ngb_touch_bind_capture(action);
}
JNIEXPORT jint JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchBindCapturing(JNIEnv *env,jobject activity) {
    (void)env;(void)activity;return ngb_touch_bind_capturing();
}
JNIEXPORT jint JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchGet(
    JNIEnv *env, jobject activity, jint item) { (void)env; (void)activity; return ngb_touch_menu_get(item); }
JNIEXPORT void JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchChange(
    JNIEnv *env, jobject activity, jint item, jint direction) { (void)env; (void)activity; ngb_touch_menu_change(item,direction); }
JNIEXPORT void JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchActivate(
    JNIEnv *env, jobject activity, jint item) { (void)env; (void)activity; ngb_touch_menu_activate(item); }
JNIEXPORT void JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchEditorRequest(
    JNIEnv *env, jobject activity) { (void)env; (void)activity; extern void ngb_menu_request_touch_editor(void); ngb_menu_request_touch_editor(); }
JNIEXPORT jstring JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchLayoutGet(
    JNIEnv *env, jobject activity) {
    (void)activity;
    char *text = malloc(4096);
    if (!text)
        return NULL;
    ngb_touch_layout_get(text, 4096);
    jstring result = (*env)->NewStringUTF(env, text);
    free(text);
    return result;
}
JNIEXPORT void JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchLayoutDefaults(
    JNIEnv *env, jobject activity) {
    (void)env; (void)activity;
    pthread_mutex_lock(&s_lock);
    layout_load();
    layout_known();
    layout_defaults();
    s_preset = 0;
    s_selected = -1;
    layout_save();
    s_layout_revision++;
    s_serial++;
    pthread_mutex_unlock(&s_lock);
}
JNIEXPORT void JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchSkinPick(
    JNIEnv *env, jobject activity, jint id) {
    (void)env; (void)activity;
    pthread_mutex_lock(&s_lock);
    layout_load();
    if (id >= 0 && id < NCTRL) {
        s_selected = id;
        touch_pick_skin();
    }
    pthread_mutex_unlock(&s_lock);
}
JNIEXPORT void JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchSkinSetPath(
    JNIEnv *env, jobject activity, jint id, jstring path) {
    (void)activity;
    pthread_mutex_lock(&s_lock);
    layout_load();
    if (id >= 0 && id < NCTRL && path) {
        const char *utf = (*env)->GetStringUTFChars(env, path, NULL);
        if (utf) {
            snprintf(s_skin_path[id], sizeof s_skin_path[id], "%s", utf);   /* (empty: no picture) */
            s_layout_skin[id] = utf[0] != 0;
            (*env)->ReleaseStringUTFChars(env, path, utf);
        }
        layout_save();
        s_layout_revision++;
        s_serial++;
    }
    pthread_mutex_unlock(&s_lock);
}
JNIEXPORT void JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTouchLayoutApply(
    JNIEnv *env, jobject activity, jstring text) {
    (void)activity;
    if (!text)
        return;
    const char *utf = (*env)->GetStringUTFChars(env, text, NULL);
    if (!utf)
        return;
    ngb_touch_layout_apply(utf);
    (*env)->ReleaseStringUTFChars(env, text, utf);
}
/* The editor on the phone (NgbActivity's TouchLayoutEditor): the layout button on the pad, the game's own Touch
 * row and the editor's own request all come here. */
void ngb_touch_editor_open(void)
{
    JNIEnv *env=(JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity=SDL_AndroidGetActivity();
    if(!env||!activity)return;
    jclass cls=(*env)->GetObjectClass(env,activity);
    jmethodID method=cls?(*env)->GetMethodID(env,cls,"showTouchEditor","()V"):NULL;
    if(method)(*env)->CallVoidMethod(env,activity,method);
    if(cls)(*env)->DeleteLocalRef(env,cls);
}
/* The settings page: 画面 and 按键, as the phone's own. */
void ngb_settings_open(void)
{
    JNIEnv *env=(JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity=SDL_AndroidGetActivity();
    if(!env||!activity)return;
    jclass cls=(*env)->GetObjectClass(env,activity);
    jmethodID method=cls?(*env)->GetMethodID(env,cls,"showSettingsMenu","()V"):NULL;
    if(method)(*env)->CallVoidMethod(env,activity,method);
    if(cls)(*env)->DeleteLocalRef(env,cls);
}
JNIEXPORT jint JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativePadThemeGet(
    JNIEnv *env, jobject activity) {
    (void)env;
    (void)activity;
    pthread_mutex_lock(&s_lock);
    layout_load();   /* (the page asked before the pad was ever drawn: read what was saved) */
    int theme = s_pad_theme;
    pthread_mutex_unlock(&s_lock);
    return (jint)theme;
}

JNIEXPORT void JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativePadThemeSet(
    JNIEnv *env, jobject activity, jint value) {
    (void)env;
    (void)activity;
    pthread_mutex_lock(&s_lock);
    layout_load();
    s_pad_theme = value < 0 ? 0 : value > 2 ? 2 : (int)value;
    layout_save();
    s_layout_revision++;
    s_serial++;
    pthread_mutex_unlock(&s_lock);
}

JNIEXPORT jint JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeVideoGet(
    JNIEnv *env, jobject activity, jint which) {
    (void)env; (void)activity;
    if (which == 0) return (jint)lroundf(ngb_video_brightness * 100.f);
    if (which == 1) return (jint)lroundf(ngb_video_gamma * 100.f);
    if (which == 3) return (jint)lroundf(ngb_video_contrast * 100.f);
    if (which == 4) return (jint)lroundf(ngb_video_saturation * 100.f);
    if (which == 5) return (jint)ngb_texture_dump_on;
    return ngb_video_tint;
}
JNIEXPORT void JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeVideoSet(
    JNIEnv *env, jobject activity, jint which, jint value) {
    (void)env; (void)activity;
    pthread_mutex_lock(&s_lock);
    layout_load();
    if (which == 0)
        ngb_video_brightness = fminf(1.6f, fmaxf(.5f, value / 100.f));
    else if (which == 1)
        ngb_video_gamma = fminf(2.f, fmaxf(.4f, value / 100.f));
    else if (which == 3)
        ngb_video_contrast = fminf(1.5f, fmaxf(.5f, value / 100.f));
    else if (which == 4)
        ngb_video_saturation = fminf(2.f, fmaxf(0.f, value / 100.f));
    else if (which == 5) {
        int was = ngb_texture_dump_on;
        ngb_texture_dump_on = value != 0;
        if (!was && ngb_texture_dump_on) {
            ngb_textures_rescan();   /* (a fresh count: the ones already looked at are packed in, if on) */
        }
    }
    else
        ngb_video_tint = value < 0 ? 0 : value > 2 ? 2 : (int)value;
    layout_save();
    pthread_mutex_unlock(&s_lock);
    ngb_video_filter_refresh();
}
/* The effects page of the phone's settings menu: the trail as a percentage, the capture as a switch. */
JNIEXPORT jint JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeFxGet(
    JNIEnv *env, jobject activity, jint which) {
    (void)env; (void)activity;
    if (which == 0) return (jint)ngb_fx_trail();
    if (which == 1) return (jint)ngb_fx_dump();
    if (which == 7) return (jint)ngb_fx_hits;
    if (which == 8) return (jint)ngb_fx_on();
    return (jint)ngb_fx_tint((int)which - 2);   /* 2 R, 3 G, 4 B, 5 alpha, 6 glow */
}
JNIEXPORT void JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeFxSet(
    JNIEnv *env, jobject activity, jint which, jint value) {
    (void)env; (void)activity;
    if (which == 0) {
        int v = value < 0 ? 0 : value > 200 ? 200 : (int)value;
        ngb_pc_set_setting(NGB_SET_FXTRAIL, v);
    } else if (which == 1) {
        ngb_pc_set_setting(NGB_SET_FXDUMP, value != 0);
    } else if (which == 8) {
        extern int ngb_fx_on_set(int on);
        ngb_fx_on_set(value != 0);
    } else {
        ngb_fx_tint_set((int)which - 2, (int)value);
    }
}
/* 诊断: every kind of blended draw the last frame made, in words the menu can show. Read-only, best effort:
   the renderer owns the buckets, and a torn count only makes a line odd for one press. */
static const char *fx_blend_name(unsigned v)
{
    switch (v) {
    case 0: return "ZERO";
    case 1: return "ONE";
    case 2: return "SRCCOLOR";
    case 3: return "INVSRCCOLOR";
    case 4: return "SRCALPHA";
    case 5: return "INVSRCALPHA";
    default: return "?";
    }
}
JNIEXPORT jstring JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeFxStats(
    JNIEnv *env, jobject activity) {
    (void)activity;
    char b[1024];
    int off = 0, n = ngb_fx_last_n < 24 ? ngb_fx_last_n : 24;
    off += snprintf(b + off, sizeof b - (size_t)off, "本帧混合绘制 %d 类", n);
    int order[24];
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 0; i < n; i++)          /* most vertices first */
        for (int j = i + 1; j < n; j++)
            if (ngb_fx_last[order[j]].n > ngb_fx_last[order[i]].n) { int t = order[i]; order[i] = order[j]; order[j] = t; }
    for (int i = 0; i < n && i < 6; i++) {
        unsigned key = ngb_fx_last[order[i]].key;
        off += snprintf(b + off, sizeof b - (size_t)off, "\n· 混合 %s/%s%s%s%s · 顶点 %d",
                        fx_blend_name(key & 0xFu), fx_blend_name((key >> 4) & 0xFu),   /* source / destination */
                        (key & 0x100u) ? "" : " · 不混合", (key & 0x200u) ? " · 目标贴图" : "",
                        (key & 0x400u) ? " · 2D" : " · 3D", ngb_fx_last[order[i]].n);
        if (key & 0x800u) off += snprintf(b + off, sizeof b - (size_t)off, " · 特效尺寸");
    }
    return (*env)->NewStringUTF(env, b);
}
JNIEXPORT jstring JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTextureDir(
    JNIEnv *env, jobject activity) {
    (void)activity;
    const char *dir = ngb_texture_dir();
    return (*env)->NewStringUTF(env, dir ? dir : "");
}
JNIEXPORT void JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTextureRescan(
    JNIEnv *env, jobject activity) {
    (void)env; (void)activity;
    ngb_texture_dir_forget();   /* (a folder permission given after start: the path is worked out again) */
    ngb_textures_rescan();
}
static int ends_png(const char *s) {
    size_t n = strlen(s);
    if (n < 5) return 0;
    const char *e = s + n - 4;
    return e[-1] == '.' && (e[0] == 'p' || e[0] == 'P') && (e[1] == 'n' || e[1] == 'N')
        && (e[2] == 'g' || e[2] == 'G');
}

/* The folder a picture belongs to, worked out from its own name ("512x1024_..."), so a pack dumped before the
 * buckets existed can be put where the eye expects it. */
static const char *bucket_for_name(const char *name) {
    unsigned w = 0, h = 0;
    if (sscanf(name, "%ux%u_", &w, &h) != 2) return NULL;
    unsigned m = w > h ? w : h;
    return m >= 512 ? "512_plus" : m >= 256 ? "256" : m >= 128 ? "128" : "small";
}

static int write_section(FILE *fp, const char *root, const char *sub, const char *heading, int *total) {
    char folder[620];
    if (sub && sub[0]) snprintf(folder, sizeof folder, "%s/%s", root, sub);
    else snprintf(folder, sizeof folder, "%s", root);
    DIR *dp = opendir(folder);
    if (!dp) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (!ends_png(e->d_name)) continue;
        if (n == 0)
            fprintf(fp, "<h3 style='margin:18px 0 6px;color:#cfd8e6'>%s</h3>"
                        "<div style='display:flex;flex-wrap:wrap;gap:12px'>", heading);
        char href[640];
        if (sub && sub[0]) snprintf(href, sizeof href, "%s/%s", sub, e->d_name);
        else snprintf(href, sizeof href, "%s", e->d_name);
        fprintf(fp, "<figure style='margin:0;width:148px'>"
                    "<a href='%s' target=_blank><img src='%s' loading=lazy "
                    "style='width:148px;background:#000;image-rendering:pixelated'></a>"
                    "<figcaption style='font-size:11px;color:#9aa7b8;word-break:break-all'>%s"
                    "</figcaption></figure>", href, href, e->d_name);
        n++;
        (*total)++;
    }
    if (n) fprintf(fp, "</div>");
    closedir(dp);
    return n;
}

/* A sheet of every picture in the folder, so a player looking for one texture among hundreds can see them
 * all at once in the phone's browser instead of opening files one by one. The pictures are grouped the way
 * the folders are grouped: by their own size. */
JNIEXPORT jstring JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTextureIndex(
    JNIEnv *env, jobject activity) {
    (void)activity;
    const char *dir = ngb_texture_dir();
    if (!dir || !dir[0]) return (*env)->NewStringUTF(env, "还没有贴图文件夹");
    char path[620], msg[256];
    snprintf(path, sizeof path, "%s/index.html", dir);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        snprintf(msg, sizeof msg, "写不出 index.html：%s", dir);
        return (*env)->NewStringUTF(env, msg);
    }
    fprintf(fp, "<!doctype html><meta charset=utf-8><title>NGB 贴图</title>"
                "<body style='background:#12141a;color:#dfe4ec;font:13px sans-serif;padding:10px'>"
                "<h2 style='margin:6px 0'>NGB 贴图包</h2>"
                "<p style='color:#9aa7b8'>点图看原图。同名 PNG 放回原来的尺寸文件夹就顶替游戏贴图（DXT1/3/5 "
                "也一样，会按原尺寸重新压回 DXT）；改完回游戏按「重新读取贴图」。</p>");
    int total = 0;
    write_section(fp, dir, "512_plus", "512 及更大", &total);
    write_section(fp, dir, "256", "256", &total);
    write_section(fp, dir, "128", "128", &total);
    write_section(fp, dir, "small", "小图（128 以下）", &total);
    write_section(fp, dir, "", "未分类", &total);
    fprintf(fp, "</body>");
    fclose(fp);
    snprintf(msg, sizeof msg, "索引好了：%d 张 → %s/index.html", total, dir);
    return (*env)->NewStringUTF(env, msg);
}

/* Put the pictures already in the root where they belong: "512x1024_..." into 512_plus/ and so on. */
JNIEXPORT jstring JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTextureTidy(
    JNIEnv *env, jobject activity) {
    (void)activity;
    const char *dir = ngb_texture_dir();
    if (!dir || !dir[0]) return (*env)->NewStringUTF(env, "还没有贴图文件夹");
    int moved = 0;
    DIR *dp = opendir(dir);
    if (dp) {
        struct dirent *e;
        while ((e = readdir(dp))) {
            if (!ends_png(e->d_name)) continue;
            const char *bucket = bucket_for_name(e->d_name);
            if (!bucket) continue;
            char sub[640], from[700], to[720];
            snprintf(sub, sizeof sub, "%s/%s", dir, bucket);
            mkdir(sub, 0777);
            snprintf(from, sizeof from, "%s/%s", dir, e->d_name);
            snprintf(to, sizeof to, "%s/%s", sub, e->d_name);
            if (rename(from, to) == 0) moved++;
        }
        closedir(dp);
    }
    char msg[256];
    snprintf(msg, sizeof msg, "整理好了：%d 张按尺寸放进了各自文件夹", moved);
    return (*env)->NewStringUTF(env, msg);
}

JNIEXPORT jint JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTextureCount(
    JNIEnv *env, jobject activity) {
    (void)env; (void)activity;
    return (jint)ngb_textures_dumped();
}
JNIEXPORT void JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeTextureDirForget(
    JNIEnv *env, jobject activity) {
    (void)env; (void)activity;
    ngb_texture_dir_forget();
}
JNIEXPORT jstring JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeVideoFilterSet(
    JNIEnv *env, jobject activity, jstring path) {
    (void)activity;
    pthread_mutex_lock(&s_lock);
    layout_load();
    if (path) {
        const char *utf = (*env)->GetStringUTFChars(env, path, NULL);
        if (utf) {
            snprintf(s_filter_path, sizeof s_filter_path, "%s", utf);
            (*env)->ReleaseStringUTFChars(env, path, utf);
        }
    } else
        s_filter_path[0] = 0;
    filter_curve_build();
    filter_cube_build();
    layout_save();
    pthread_mutex_unlock(&s_lock);
    ngb_video_filter_refresh();
    return (*env)->NewStringUTF(env, s_filter_path[0] ? s_filter_path : "");
}
JNIEXPORT jstring JNICALL Java_io_github_gamboozino_ngbrecomp_NgbActivity_nativeVideoFilterName(
    JNIEnv *env, jobject activity) {
    (void)activity;
    const char *slash = strrchr(s_filter_path, '/');
    return (*env)->NewStringUTF(env, s_filter_path[0] ? (slash ? slash + 1 : s_filter_path) : "");
}
#else
void ngb_touch_editor_open(void) { }
void ngb_settings_open(void) { }
#endif

/* The presenter asks first whether there is a picture at all */
bool nv2a_layer_wanted(void)
{
    return !xbox_InputControllerPresent();
}

/* The touch controls on screen (src/d3d/vk/backend.cpp: the Interact prompt kept off them) */
int ngb_touch_shown(void)
{
    return !xbox_InputControllerPresent() && __atomic_load_n(&s_art_shown, __ATOMIC_ACQUIRE);
}

/* The presenter's call (xboxrecomp present_sdl.c), with the window's size: the picture drawn into px (at most
 * max pixels) when it changed since *serial (0: draw it whatever), its size; false while hidden (a controller
 * in use, or nothing to draw them with yet). */
bool nv2a_overlay_picture(uint32_t win_w, uint32_t win_h, uint32_t *px, size_t max, uint32_t *ow, uint32_t *oh,
                          unsigned *serial)
{
    if (xbox_InputControllerPresent() || !win_w || !win_h)
        return false;
    /* (the window's own size; one of more pixels than the presenter's buffer holds at three quarters, or half) */
    static int half = -1;
    if (half < 0)
        { const char *e = getenv("NGB_TOUCH_HALF"); half = e && *e == '1'; }
    int w = (int)win_w, h = (int)win_h;
    if (!half && (size_t)w * h > max) {
        w = (int)(win_w * 3 / 4);
        h = (int)(win_h * 3 / 4);
    }
    if (half || (size_t)w * h > max) {
        w = (int)(win_w / 2);
        h = (int)(win_h / 2);
    }
    if (!w || !h || (size_t)w * h > max)
        return false;
    touch_poll_skin();
    pthread_mutex_lock(&s_lock);
    s_aspect = (float)win_w / (float)win_h;
    enum Art art = art_now();
    s_standin_places = art == ART_STANDIN;   /* (the touches found where the picture shows them) */
    if (art != s_art)
        s_serial++;               /* (the game's pieces made meanwhile: all again, a picture the presenter takes) */
    __atomic_store_n(&s_art_shown, art != ART_NONE, __ATOMIC_RELEASE);
    if (art == ART_NONE) {        /* (none yet: nothing over the game) */
        s_art = art;
        pthread_mutex_unlock(&s_lock);
        return false;
    }
    if (s_layout < 0) {           /* (not drawn before the game's side has read the layout: the settings are its) */
        pthread_mutex_unlock(&s_lock);
        return false;
    }
    unsigned now = s_serial;
    bool changed = *serial != now;
    if (changed) {
        memcpy(d_down, s_down, sizeof d_down);
        memcpy(d_stick, s_stick, sizeof d_stick);
        d_dpad = s_dpad;
        memcpy(d_layout_x,s_layout_x,sizeof d_layout_x); memcpy(d_layout_y,s_layout_y,sizeof d_layout_y); memcpy(d_layout_r,s_layout_r,sizeof d_layout_r); memcpy(d_layout_alpha,s_layout_alpha,sizeof d_layout_alpha); memcpy(d_layout_custom,s_layout_custom,sizeof d_layout_custom); memcpy(d_layout_color,s_layout_color,sizeof d_layout_color); memcpy(d_layout_shape,s_layout_shape,sizeof d_layout_shape); memcpy(d_layout_asp,s_layout_asp,sizeof d_layout_asp); memcpy(d_layout_skin,s_layout_skin,sizeof d_layout_skin); memcpy(d_layout_combo,s_layout_combo,sizeof d_layout_combo); memcpy(d_layout_enabled,s_layout_enabled,sizeof d_layout_enabled);
        d_edit_mode=s_edit_mode; d_haptics=s_haptics; memcpy(d_skin_path,s_skin_path,sizeof d_skin_path); d_selected=s_selected; d_preset=s_preset; d_layout_revision=s_layout_revision; d_menu_dim=s_menu_dim;
        d_aspect = s_aspect;
        d_layout = s_layout == 1;   /* (known: layout_known above) */
        if (!d_anim_ready) {
            memcpy(d_anim_x, d_layout_x, sizeof d_anim_x);
            memcpy(d_anim_y, d_layout_y, sizeof d_anim_y);
            memcpy(d_anim_r, d_layout_r, sizeof d_anim_r);
            d_anim_ready = true;
        }
        d_opacity = (s_opacity < 0 ? 100 : s_opacity) / 100.0f;
    }
    pthread_mutex_unlock(&s_lock);
    /* Ease each edited control toward its new target over several presented frames. */
    bool animating = false;
    for (int i = 0; i < NCTRL; i++) {
        float dx = d_layout_x[i] - d_anim_x[i];
        float dy = d_layout_y[i] - d_anim_y[i];
        float dr = d_layout_r[i] - d_anim_r[i];
        if (fabsf(dx) + fabsf(dy) + fabsf(dr) > 0.0008f) {
            d_anim_x[i] += dx * 0.34f;
            d_anim_y[i] += dy * 0.34f;
            d_anim_r[i] += dr * 0.34f;
            animating = true;
        } else {
            d_anim_x[i] = d_layout_x[i];
            d_anim_y[i] = d_layout_y[i];
            d_anim_r[i] = d_layout_r[i];
        }
    }
    if (animating) {
        d_layout_revision++;
        changed = true;
    }
    *ow = (uint32_t)w;
    *oh = (uint32_t)h;
    if (changed) {
        s_px = px;
        s_w = w;
        s_h = h;
        s_art = art;
        draw_all();
        *serial = now;
    }
    return true;
}
