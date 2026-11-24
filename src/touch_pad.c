/*
 * Touch controls (Android): an Xbox 360 pad laid over the screen -- the two sticks, the d-pad, A B X Y in their
 * colours, the bumpers and triggers, Back and Start and the stick clicks -- read from the screen's touches and
 * merged into the pad state the keyboard gives (src/keyboard.c, xbox_keyboard_map), which the game reads while
 * no controller is connected. With a controller connected they are hidden and their touches ignored.
 *
 * The picture is made here, at half the window's size, premultiplied BGRA, again whenever something on it
 * changes; the presenter blends it over each frame (xboxrecomp's present_sdl.c, nv2a_overlay_picture). The
 * buttons are the 360's own as the menus' prompts show them (Xelu's, CC0: assets/prompts/xbox), the sticks and
 * d-pad Kenney's Mobile Controls (CC0: assets/touch), all embedded (CMakeLists.txt) and drawn half see-through,
 * whole and with a gold edge while held.
 *
 * Places are in screen heights from the left or right edge (a tall screen keeps its controls in the corners;
 * a less wide one draws the d-pad and right stick in towards the middle).
 *
 * Threads: the touches arrive on Android's UI thread (SDL's event watch, run as they are sent); the pad state
 * is read wherever the input is polled; the picture is made by whichever thread presents (the PFIFO thread or
 * the renderer's), one at a time under the presenter's lock. s_lock keeps the state short-held: the picture is
 * drawn from a copy, outside it.
 * NGB_TOUCH_LOG=1 logs each control a finger lands on.
 */
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "platform/xbox_winnt.h"
#include "input/xinput_xbox.h"

int xbox_InputControllerPresent(void);
void xbox_InputTouchUsed(void);   /* xboxrecomp input/xinput_device.c */
unsigned char *stbi_load_from_memory(const unsigned char *buf, int len, int *x, int *y, int *comp, int req);
void stbi_image_free(void *p);
extern const struct { const char *name; const unsigned char *data; const unsigned int *len; } ngb_key_pngs[];
extern const int ngb_key_png_count;   /* (CMakeLists.txt: the embedded pictures by file name) */

enum Kind { BUTTON, STICK, DPAD };
enum Id { LSTICK, RSTICK, DPAD_ID, BA, BB, BX, BY, LB, RB, LT, RT, BACK, START, LS, RS, NCTRL };

typedef struct {
    enum Kind kind;
    int right;           /* x measured from the right edge */
    float x, y, r;       /* centre and radius, in screen heights */
    const char *name;    /* (the log's) */
    const char *sprite;  /* its picture (a stick's or the d-pad's: Kenney's) */
} Ctrl;

static const Ctrl s_ctrl[NCTRL] = {
    [LSTICK]  = { STICK,  0, 0.30f, 0.70f, 0.17f,  "left stick", "touch_pad" },
    [RSTICK]  = { STICK,  1, 0.72f, 0.66f, 0.12f,  "right stick", "touch_pad" },   /* (above the game's
                                                       prompts, at the picture's lower right) */
    [DPAD_ID] = { DPAD,   0, 0.74f, 0.84f, 0.12f,  "d-pad", "touch_dpad" },
    [BA]      = { BUTTON, 1, 0.28f, 0.70f, 0.07f,  "A", "360_A" },
    [BB]      = { BUTTON, 1, 0.16f, 0.58f, 0.07f,  "B", "360_B" },
    [BX]      = { BUTTON, 1, 0.40f, 0.58f, 0.07f,  "X", "360_X" },
    [BY]      = { BUTTON, 1, 0.28f, 0.46f, 0.07f,  "Y", "360_Y" },
    [LB]      = { BUTTON, 0, 0.30f, 0.28f, 0.075f, "LB", "360_LB" },
    [RB]      = { BUTTON, 1, 0.30f, 0.28f, 0.075f, "RB", "360_RB" },
    [LT]      = { BUTTON, 0, 0.13f, 0.14f, 0.085f, "LT", "360_LT" },
    [RT]      = { BUTTON, 1, 0.13f, 0.14f, 0.085f, "RT", "360_RT" },
    [BACK]    = { BUTTON, 0, 0.09f, 0.37f, 0.06f,  "BACK", "360_Back" },
    [START]   = { BUTTON, 1, 0.09f, 0.37f, 0.06f,  "START", "360_Start" },
    [LS]      = { BUTTON, 0, 0.09f, 0.90f, 0.055f, "LS", "360_Left_Stick_Click" },
    [RS]      = { BUTTON, 1, 0.09f, 0.90f, 0.055f, "RS", "360_Right_Stick_Click" },
};

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static float s_aspect = 16.0f / 9;
static float s_layout_x[NCTRL], s_layout_y[NCTRL], s_layout_r[NCTRL], s_layout_alpha[NCTRL];
static float d_layout_x[NCTRL], d_layout_y[NCTRL], d_layout_r[NCTRL], d_layout_alpha[NCTRL];
static bool s_layout_custom[NCTRL], d_layout_custom[NCTRL];
static unsigned s_layout_revision, d_layout_revision;
static bool s_layout_loaded, s_edit_mode, d_edit_mode;
static int s_selected = -1, d_selected = -1;
static char s_layout_path[512];
static bool s_down[NCTRL];
static float s_stick[2][2];                   /* each stick's knob, -1..1 (y down) */
static int s_dpad;                            /* XBOX_GAMEPAD_DPAD_* bits */
static unsigned s_serial = 1;                 /* +1 each change of what the picture shows */

#define MAX_FINGERS 10
static struct { SDL_FingerID id; int ctrl; bool used, moved, was_selected; float x0, y0; } s_finger[MAX_FINGERS];

static int ctrl_index(const Ctrl *c) { return (int)(c - s_ctrl); }

static void layout_defaults(void)
{
    for (int i = 0; i < NCTRL; i++) {
        s_layout_x[i] = s_ctrl[i].x;
        s_layout_y[i] = s_ctrl[i].y;
        s_layout_r[i] = s_ctrl[i].r;
        s_layout_alpha[i] = (i >= BA && i <= BY) ? 0.65f : 0.55f;
        s_layout_custom[i] = false;
    }
}
static void layout_load(void)
{
    if (s_layout_loaded) return;
    s_layout_loaded = true;
    layout_defaults();
    char *dir = SDL_GetPrefPath("NinjaGaidenBlack", "touch-controls");
    if (!dir) return;
    snprintf(s_layout_path, sizeof s_layout_path, "%s/touch-controls.cfg", dir);
    SDL_free(dir);
    FILE *fp = fopen(s_layout_path, "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof line, fp)) {
        int id, custom = 0; float x, y, r, a;
        int fields = sscanf(line, "%d %f %f %f %f %d", &id, &x, &y, &r, &a, &custom);
        if (fields < 5 || id < 0 || id >= NCTRL) continue;
        s_layout_x[id] = x; s_layout_y[id] = y; s_layout_r[id] = r; s_layout_alpha[id] = a;
        s_layout_custom[id] = fields >= 6 && custom != 0;
    }
    fclose(fp);
    for (int i = 0; i < NCTRL; i++) {
        s_layout_r[i] = fminf(0.25f, fmaxf(0.035f, s_layout_r[i]));
        s_layout_y[i] = fminf(0.97f, fmaxf(0.03f, s_layout_y[i]));
        s_layout_alpha[i] = fminf(1.0f, fmaxf(0.15f, s_layout_alpha[i]));
    }
}
static void layout_save(void)
{
    if (!s_layout_path[0]) {
        char *dir = SDL_GetPrefPath("NinjaGaidenBlack", "touch-controls");
        if (!dir) return;
        snprintf(s_layout_path, sizeof s_layout_path, "%s/touch-controls.cfg", dir);
        SDL_free(dir);
    }
    FILE *fp = fopen(s_layout_path, "w");
    if (!fp) return;
    for (int i = 0; i < NCTRL; i++)
        fprintf(fp, "%d %.5f %.5f %.5f %.5f %d\n", i, s_layout_x[i], s_layout_y[i], s_layout_r[i], s_layout_alpha[i], s_layout_custom[i] ? 1 : 0);
    fclose(fp);
}
static void clear_touch_state(void)
{
    memset(s_finger, 0, sizeof s_finger);
    memset(s_down, 0, sizeof s_down);
    memset(s_stick, 0, sizeof s_stick);
    s_dpad = 0;
}
static void toggle_editor(void)
{
    s_edit_mode = !s_edit_mode;
    s_selected = -1;
    clear_touch_state();
    if (!s_edit_mode) layout_save();
    s_serial++;
}
static float centre_x_at(const Ctrl *c, float aspect)
{
    int ci = ctrl_index(c);
    float x = s_layout_x[ci];
    if (!s_layout_custom[ci] && (c == &s_ctrl[DPAD_ID] || c == &s_ctrl[RSTICK])) {   /* (in from the middle on a less wide screen) */
        float most = aspect / 2 - 0.16f;
        x = x < most ? x : most;
    }
    return c->right ? aspect - x : x;
}

static float centre_x(const Ctrl *c) { return centre_x_at(c, s_aspect); }
static float centre_x_draw_at(const Ctrl *c, float aspect)
{
    int ci = ctrl_index(c);
    float x = d_layout_x[ci];
    if (!d_layout_custom[ci] && (c == &s_ctrl[DPAD_ID] || c == &s_ctrl[RSTICK])) {
        float most = aspect / 2 - 0.16f;
        x = x < most ? x : most;
    }
    return c->right ? aspect - x : x;
}

/* The control under (x, y) (screen heights), the sticks and pad found from a little way off */
static int hit(float x, float y, bool buttons_only)
{
    int best = -1;
    float best_d = 1e9f;
    for (int i = 0; i < NCTRL; i++) {
        const Ctrl *c = &s_ctrl[i];
        if (buttons_only && c->kind != BUTTON)
            continue;
        int ci = ctrl_index(c);
        float dx = x - centre_x(c), dy = y - s_layout_y[ci], d;
        float reach = c->kind == STICK ? s_layout_r[ci] * 1.7f : c->kind == DPAD ? s_layout_r[ci] * 1.4f : s_layout_r[ci] * 1.35f;
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
    const Ctrl *c = &s_ctrl[i];
    int ci = ctrl_index(c);
    float dx = (x - centre_x(c)) / s_layout_r[ci], dy = (y - s_layout_y[ci]) / s_layout_r[ci], m = sqrtf(dx * dx + dy * dy);
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
    const Ctrl *c = &s_ctrl[DPAD_ID];
    int ci = ctrl_index(c);
    float dx = x - centre_x(c), dy = y - s_layout_y[ci];
    if (dx * dx + dy * dy < (s_layout_r[ci] * 0.25f) * (s_layout_r[ci] * 0.25f))
        return 0;
    float a = atan2f(-dy, dx) * 180.0f / (float)M_PI;   /* 0 right, 90 up */
    int d = 0;
    if (a > -67.5f && a < 67.5f) d |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (a > 22.5f && a < 157.5f) d |= XBOX_GAMEPAD_DPAD_UP;
    if (a > 112.5f || a < -112.5f) d |= XBOX_GAMEPAD_DPAD_LEFT;
    if (a < -22.5f && a > -157.5f) d |= XBOX_GAMEPAD_DPAD_DOWN;
    return d;
}

static void finger_on(int ctrl, float x, float y)
{
    if (ctrl == LSTICK || ctrl == RSTICK)
        set_stick(ctrl, x, y);
    else if (ctrl == DPAD_ID)
        s_dpad = dpad_dirs(x, y);
    else
        s_down[ctrl] = true;
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

static void edit_tap(int c)
{
    if (c < 0 || c >= NCTRL) return;
    float base = s_ctrl[c].r;
    float ratio = s_layout_r[c] / base;
    s_layout_r[c] = base * (ratio < 0.88f ? 1.0f : ratio < 1.12f ? 1.25f : 0.75f);
    s_layout_revision++;
    static const float alpha[] = {0.35f, 0.50f, 0.65f, 0.80f, 1.0f};
    int next = 0;
    while (next < 4 && s_layout_alpha[c] >= alpha[next] - 0.01f) next++;
    s_layout_alpha[c] = alpha[next];
    layout_save();
}
static int SDLCALL touch_watch(void *unused, SDL_Event *ev)
{
    (void)unused;
    if (ev->type != SDL_FINGERDOWN && ev->type != SDL_FINGERMOTION && ev->type != SDL_FINGERUP)
        return 0;
    if (ev->type != SDL_FINGERUP && xbox_InputControllerPresent())
        return 0;
    layout_load();
    pthread_mutex_lock(&s_lock);
    {   /* (the window's own shape: the presenter's may not have come, where it cannot draw the picture) */
        SDL_Window *w = SDL_GetWindowFromID(ev->tfinger.windowID);
        int ww = 0, wh = 0;
        if (w)
            SDL_GetWindowSize(w, &ww, &wh);
        if (ww > 0 && wh > 0)
            s_aspect = (float)ww / (float)wh;
    }
    float x = ev->tfinger.x * s_aspect, y = ev->tfinger.y;
    if (ev->type == SDL_FINGERDOWN && x > s_aspect * 0.42f && x < s_aspect * 0.58f && y < 0.12f) {
        toggle_editor();
        pthread_mutex_unlock(&s_lock);
        return 0;
    }
    int f;
    for (f = 0; f < MAX_FINGERS && !(s_finger[f].used && s_finger[f].id == ev->tfinger.fingerId); f++)
        ;
    if (ev->type == SDL_FINGERDOWN && f == MAX_FINGERS) {
        int c = hit(x, y, false);
        for (f = 0; f < MAX_FINGERS && s_finger[f].used; f++)
            ;
        if (f < MAX_FINGERS && s_edit_mode) {
            s_finger[f].used = true; s_finger[f].id = ev->tfinger.fingerId; s_finger[f].ctrl = c;
            s_finger[f].x0 = x; s_finger[f].y0 = y; s_finger[f].was_selected = c == s_selected;
            if (c >= 0) s_selected = c;
            s_serial++;
        } else if (c >= 0 && f < MAX_FINGERS) {
            xbox_InputTouchUsed();
            s_finger[f].used = true; s_finger[f].id = ev->tfinger.fingerId; s_finger[f].ctrl = c;
            finger_on(c, x, y);
            if (s_down[BACK] && s_down[START]) {
                toggle_editor();
                pthread_mutex_unlock(&s_lock);
                return 0;
            }
            s_serial++;
            static int log = -1;
            if (log < 0)
                log = getenv("NGB_TOUCH_LOG") != NULL;
            if (log)
                fprintf(stderr, "[TOUCH] %s at %.2f, %.2f\n", s_ctrl[c].name, x, y);
        }
     } else if (ev->type == SDL_FINGERMOTION && f < MAX_FINGERS) {
        int c = s_finger[f].ctrl;
        if (s_edit_mode && c >= 0) {
            float dx = x - s_finger[f].x0, dy = y - s_finger[f].y0;
            if (fabsf(dx) + fabsf(dy) > 0.015f) s_finger[f].moved = true;
            if (s_finger[f].moved) {
                s_layout_x[c] += s_ctrl[c].right ? -dx : dx;
                s_layout_y[c] += dy;
                s_layout_custom[c] = true;
                s_layout_x[c] = fminf(s_aspect - 0.04f, fmaxf(0.04f, s_layout_x[c]));
                s_layout_y[c] = fminf(0.97f, fmaxf(0.03f, s_layout_y[c]));
                s_finger[f].x0 = x; s_finger[f].y0 = y;
                s_layout_revision++; s_serial++;
            }
        } else if (!s_edit_mode) {
            if (s_ctrl[c].kind == BUTTON && c >= BA && c <= BY) {   /* sliding across the face buttons */
            int to = hit(x, y, true);
            if (to >= BA && to <= BY && to != c) {
                s_finger[f].ctrl = to;
                finger_off(c);
                finger_on(to, x, y);
                s_serial++;
            }
        } else if (c == LSTICK || c == RSTICK || c == DPAD_ID) {   /* (the picture again only for a visible change) */
            int s = c == RSTICK, was_dpad = s_dpad;
            float was_x = c == DPAD_ID ? 0 : s_stick[s][0], was_y = c == DPAD_ID ? 0 : s_stick[s][1];
            finger_on(c, x, y);
            if (c == DPAD_ID ? s_dpad != was_dpad
                             : fabsf(s_stick[s][0] - was_x) + fabsf(s_stick[s][1] - was_y) > 1.0f / 32)
                s_serial++;
        }
     } else if (ev->type == SDL_FINGERUP && f < MAX_FINGERS) {
        int c = s_finger[f].ctrl, was_selected = s_finger[f].was_selected;
        bool tap = !s_finger[f].moved;
        s_finger[f].used = false;
        if (s_edit_mode) {
            if (tap && was_selected) edit_tap(c);
            if (s_finger[f].moved) layout_save();
            if (tap && c < 0 && s_finger[f].y0 > 0.86f &&
                s_finger[f].x0 > s_aspect * 0.42f && s_finger[f].x0 < s_aspect * 0.58f) {
                layout_defaults(); layout_save(); s_layout_revision++; s_selected = -1;
            }
        } else if (c >= 0) finger_off(c);
        s_serial++;
    }
    pthread_mutex_unlock(&s_lock);
    return 0;
}

/* A controller connected: everything held let go, so nothing stays down while the touches are ignored, nor
 * comes back down when it goes */
static void let_go(void)
{
    static int had;
    int now = xbox_InputControllerPresent();
    if (now == had)
        return;
    had = now;
    if (!now)
        return;
    pthread_mutex_lock(&s_lock);
    memset(s_finger, 0, sizeof s_finger);
    memset(s_down, 0, sizeof s_down);
    memset(s_stick, 0, sizeof s_stick);
    s_dpad = 0;
    s_serial++;
    pthread_mutex_unlock(&s_lock);
}

static void touch_init(void)
{
    static int done;
    if (__atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL)) return;
    layout_load();
    SDL_AddEventWatch(touch_watch, NULL);
}

/* Merged into the keyboard's pad state (src/keyboard.c) */
void ngb_touch_map(XBOX_INPUT_STATE *st)
{
    touch_init();
    let_go();
    pthread_mutex_lock(&s_lock);
    WORD b = st->Gamepad.wButtons | (WORD)s_dpad;
    if (s_down[BACK]) b |= XBOX_GAMEPAD_BACK;
    if (s_down[START]) b |= XBOX_GAMEPAD_START;
    if (s_down[LS]) b |= XBOX_GAMEPAD_LEFT_THUMB;
    if (s_down[RS]) b |= XBOX_GAMEPAD_RIGHT_THUMB;
    st->Gamepad.wButtons = b;
    static const struct { int ctrl, button; } analog[] = {
        { BA, XBOX_BUTTON_A }, { BB, XBOX_BUTTON_B }, { BX, XBOX_BUTTON_X }, { BY, XBOX_BUTTON_Y },
        { LB, XBOX_BUTTON_BLACK }, { RB, XBOX_BUTTON_WHITE },   /* (as a 360 pad's bumpers are: input/xinput_device.c) */
        { LT, XBOX_BUTTON_LTRIGGER }, { RT, XBOX_BUTTON_RTRIGGER },
    };
    for (size_t i = 0; i < sizeof analog / sizeof analog[0]; i++)
        if (s_down[analog[i].ctrl])
            st->Gamepad.bAnalogButtons[analog[i].button] = 255;
    if (s_down[LSTICK]) {
        st->Gamepad.sThumbLX = (SHORT)(s_stick[0][0] * 32767);
        st->Gamepad.sThumbLY = (SHORT)(-s_stick[0][1] * 32767);
    }
    if (s_down[RSTICK]) {
        st->Gamepad.sThumbRX = (SHORT)(s_stick[1][0] * 32767);
        st->Gamepad.sThumbRY = (SHORT)(-s_stick[1][1] * 32767);
    }
    pthread_mutex_unlock(&s_lock);
}

/* ---- the picture ---- */

static uint32_t *s_px;   /* the picture being drawn (the presenter's buffer), and what it shows */
static int s_w, s_h;
static bool d_down[NCTRL];
static float d_stick[2][2], d_aspect;
static int d_dpad;

typedef struct { uint8_t *px; int w, h; } Sprite;   /* straight-alpha RGBA */

/* An embedded picture by name, decoded the first time (drawing is one thread at a time: the presenter's lock) */
static const Sprite *sprite(const char *name)
{
    static struct { const char *name; Sprite s; } cache[32];
    int i;
    for (i = 0; i < 32 && cache[i].name && strcmp(cache[i].name, name); i++)
        ;
    if (i == 32)
        return NULL;
    if (!cache[i].name) {
        cache[i].name = name;
        for (int k = 0; k < ngb_key_png_count; k++)
            if (!strcmp(ngb_key_pngs[k].name, name)) {
                int n;
                cache[i].s.px = stbi_load_from_memory(ngb_key_pngs[k].data, (int)*ngb_key_pngs[k].len,
                                                      &cache[i].s.w, &cache[i].s.h, &n, 4);
                break;
            }
    }
    return &cache[i].s;
}

/* A picture over the box of size (bw, bh) centred at (cx, cy), at opacity a: each pixel sampled from it
 * bilinearly, blended premultiplied */
static void draw(const char *name, float cx, float cy, float bw, float bh, float a)
{
    const Sprite *sp = sprite(name);
    if (!sp || !sp->px || bw < 1 || bh < 1)
        return;
    int x0 = (int)floorf(cx - bw / 2), y0 = (int)floorf(cy - bh / 2), x1 = (int)ceilf(cx + bw / 2),
        y1 = (int)ceilf(cy + bh / 2);
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < s_h; y++)
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < s_w; x++) {
            float u = ((x + 0.5f) - (cx - bw / 2)) / bw * sp->w - 0.5f,
                  v = ((y + 0.5f) - (cy - bh / 2)) / bh * sp->h - 0.5f;
            if (u < -0.5f || v < -0.5f || u > sp->w - 0.5f || v > sp->h - 0.5f)
                continue;
            int iu = (int)floorf(u), iv = (int)floorf(v);
            float fu = u - iu, fv = v - iv, acc[4] = {0, 0, 0, 0};
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
            if (acc[3] <= 0)
                continue;
            uint32_t *p = &s_px[(size_t)y * s_w + x], c = *p;
            float k = 1 - acc[3] * a;
            float ob = (c & 0xFF) / 255.0f * k + acc[2] * a, og = (c >> 8 & 0xFF) / 255.0f * k + acc[1] * a,
                  orr = (c >> 16 & 0xFF) / 255.0f * k + acc[0] * a, oa = (c >> 24) / 255.0f * k + acc[3] * a;
            *p = (uint32_t)(ob * 255 + 0.5f) | (uint32_t)(og * 255 + 0.5f) << 8 | (uint32_t)(orr * 255 + 0.5f) << 16 |
                 (uint32_t)(oa * 255 + 0.5f) << 24;
        }
}

#define IDLE 0.55f   /* how see-through a control is, untouched */

/* The controls untouched, the sticks without their knobs: drawn once for each size, then each picture starts
 * from it and adds only the knobs and what is held (a stick dragged redraws a few small pictures, not all) */
static uint32_t *s_bg;
static int s_bg_w, s_bg_h;
static float s_bg_aspect;
static unsigned s_bg_layout_revision;

static void draw_idle(void)
{
    float u = (float)s_h;
    memset(s_px, 0, sizeof(uint32_t) * (size_t)s_w * s_h);
    for (int i = 0; i < NCTRL; i++) {
        const Ctrl *c = &s_ctrl[i];
        float cx = centre_x_draw_at(c, d_aspect) * u, cy = d_layout_y[i] * u, r = d_layout_r[i] * u;
        float alpha = d_layout_alpha[i];
        if (c->kind == STICK) draw("touch_pad", cx, cy, 2 * r, 2 * r, alpha);
        else if (c->kind == DPAD) draw("touch_dpad", cx, cy, 2.2f * r, 2.2f * r, alpha);
        else draw(c->sprite, cx, cy, 2.5f * r, 2.5f * r, alpha);
    }
}

static void draw_all(void)
{
    float u = (float)s_h;
    size_t px = (size_t)s_w * s_h;
    if (!s_bg || s_bg_w != s_w || s_bg_h != s_h || s_bg_aspect != d_aspect || s_bg_layout_revision != d_layout_revision) {
        free(s_bg);
        s_bg = NULL;
        draw_idle();
        s_bg = malloc(sizeof(uint32_t) * px);
        if (s_bg)
            memcpy(s_bg, s_px, sizeof(uint32_t) * px);
        s_bg_w = s_w;
        s_bg_h = s_h;
        s_bg_aspect = d_aspect;
        s_bg_layout_revision = d_layout_revision;
    } else {
        memcpy(s_px, s_bg, sizeof(uint32_t) * px);
    }
    for (int i = 0; i < NCTRL; i++) {
        const Ctrl *c = &s_ctrl[i];
        float cx = centre_x_draw_at(c, d_aspect) * u, cy = d_layout_y[i] * u, r = d_layout_r[i] * u;
        float alpha = d_layout_alpha[i];
        bool on = d_down[i];
        if (c->kind == STICK) {
            int s = i == RSTICK;
            float nx = cx + d_stick[s][0] * r * 0.55f, ny = cy + d_stick[s][1] * r * 0.55f;
            draw("touch_nub", nx, ny, r, r, on ? fmaxf(alpha, 0.85f) : fminf(1.0f, alpha + 0.2f));
            if (on) {
                draw("touch_pad_lit", cx, cy, 2 * r, 2 * r, fmaxf(alpha, 0.8f));
                draw("touch_nub_lit", nx, ny, r, r, fmaxf(alpha, 0.8f));
            }
        } else if (c->kind == DPAD && on) {
            static const struct { int bit; const char *lit; float dx, dy; } arms[] = {
                { XBOX_GAMEPAD_DPAD_UP, "touch_dpad_north_lit", 0, -1 },
                { XBOX_GAMEPAD_DPAD_DOWN, "touch_dpad_south_lit", 0, 1 },
                { XBOX_GAMEPAD_DPAD_LEFT, "touch_dpad_west_lit", -1, 0 },
                { XBOX_GAMEPAD_DPAD_RIGHT, "touch_dpad_east_lit", 1, 0 },
            };
            float size = 2.2f * r;   /* (the picture's arms are a third of it each) */
            draw("touch_dpad", cx, cy, size, size, fmaxf(alpha, 0.65f));
            for (int a = 0; a < 4; a++)
                if (d_dpad & arms[a].bit)
                    draw(arms[a].lit, cx + arms[a].dx * size / 3, cy + arms[a].dy * size / 3, size / 2, size / 2,
                         0.95f);
        } else if (c->kind == BUTTON && on) {
            draw(c->sprite, cx, cy, 2.7f * r, 2.7f * r, fmaxf(alpha, 0.8f));
            if (i >= BA && i <= BY)
                draw("touch_button_lit", cx, cy, 2.3f * r, 2.3f * r, fmaxf(alpha, 0.75f));
        }
        if (d_edit_mode && i == d_selected) {
            if (c->kind == STICK) draw("touch_pad_lit", cx, cy, 2.1f * r, 2.1f * r, 0.8f);
            else draw("touch_button_lit", cx, cy, 2.6f * r, 2.6f * r, 0.75f);
        }
    }
}

/* The presenter asks first whether there is a picture at all */
bool nv2a_layer_wanted(void)
{
    return !xbox_InputControllerPresent();
}

/* The presenter's call (xboxrecomp present_sdl.c), with the window's size: the picture drawn into px (at most
 * max pixels) when it changed since *serial (0: draw it whatever), its size; false while hidden (a controller
 * in use). */
bool nv2a_overlay_picture(uint32_t win_w, uint32_t win_h, uint32_t *px, size_t max, uint32_t *ow, uint32_t *oh,
                          unsigned *serial)
{
    if (xbox_InputControllerPresent() || !win_w || !win_h)
        return false;
    int w = (int)(win_w / 2), h = (int)(win_h / 2);
    if (!w || !h || (size_t)w * h > max)
        return false;
    pthread_mutex_lock(&s_lock);
    s_aspect = (float)win_w / (float)win_h;
    unsigned now = s_serial;
    bool changed = *serial != now;
    if (changed) {
        memcpy(d_down, s_down, sizeof d_down);
        memcpy(d_stick, s_stick, sizeof d_stick);
        d_dpad = s_dpad;
        memcpy(d_layout_x, s_layout_x, sizeof d_layout_x);
        memcpy(d_layout_y, s_layout_y, sizeof d_layout_y);
        memcpy(d_layout_r, s_layout_r, sizeof d_layout_r);
        memcpy(d_layout_alpha, s_layout_alpha, sizeof d_layout_alpha);
        memcpy(d_layout_custom, s_layout_custom, sizeof d_layout_custom);
        d_layout_revision = s_layout_revision;
        d_edit_mode = s_edit_mode; d_selected = s_selected;
        d_aspect = s_aspect;
    }
    pthread_mutex_unlock(&s_lock);
    *ow = (uint32_t)w;
    *oh = (uint32_t)h;
    if (changed) {
        s_px = px;
        s_w = w;
        s_h = h;
        draw_all();
        *serial = now;
    }
    return true;
}
