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
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#if defined(__ANDROID__)
#include <SDL_system.h>
#include <jni.h>
#endif

#include "platform/xbox_winnt.h"
#include "input/xinput_xbox.h"
#include "game_prompts.h"

int xbox_InputControllerPresent(void);
void xbox_InputTouchUsed(void);   /* xboxrecomp input/xinput_device.c */
unsigned char *stbi_load_from_memory(const unsigned char *buf, int len, int *x, int *y, int *comp, int req);
void stbi_image_free(void *p);
extern const struct { const char *name; const unsigned char *data; const unsigned int *len; } ngb_touch_pngs[];
extern const int ngb_touch_png_count;   /* (CMakeLists.txt: the stand-in pieces by file name) */
#include "launcher_font.h"              /* (the stand-in buttons' letters) */
/* (weak: the arcade programs, built with this file, have no src/game_prompts.c) */
__attribute__((weak)) const NgbPic *ngb_gp_touch(const char *piece);

enum Kind { BUTTON, COMBO, STICK, DPAD };
enum Id { LSTICK, RSTICK, DPAD_ID, BA, BB, BX, BY, LB, RB, LT, RT, BACK, START, LS, RS, SWAP, NINPO, WINDRUN, CUSTOM1, CUSTOM2, CUSTOM3, CUSTOM4, NCTRL };

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
    [RSTICK]  = { STICK,  1, 0.66f,  0.68f,  0.12f,  "right stick", NULL, 0, NULL },   /* (below and inside the
                                                       face buttons, clear of the game's prompts) */
    [DPAD_ID] = { DPAD,   0, 0.37f,  0.84f,  0.12f,  "d-pad", NULL, 0, NULL },   /* (0.09 was small: #284) */
    [BA]      = { BUTTON, 1, 0.193f, 0.675f, 0.075f, "A", "A", 0x7CC84E, "button_a" },
    [BB]      = { BUTTON, 1, 0.193f, 0.51f,  0.07f,  "B", "B", 0xE8574F, "button_b" },
    [BX]      = { BUTTON, 1, 0.347f, 0.59f,  0.075f, "X", "X", 0x4C9BEA, "button_x" },
    [BY]      = { BUTTON, 1, 0.347f, 0.43f,  0.075f, "Y", "Y", 0xF2C83A, "button_y" },
    [LB]      = { BUTTON, 1, 0.123f, 0.35f,  0.042f, "LB", "LB", 0xE6E6E6, "button_black" },
    [RB]      = { BUTTON, 1, 0.278f, 0.28f,  0.042f, "RB", "RB", 0xE6E6E6, "button_white" },
    [LT]      = { BUTTON, 1, 0.52f,  0.40f,  0.08f,  "LT", "LT", 0xE6E6E6, "trigger_l" },   /* (guard: by the face
                                                       buttons) */
    [RT]      = { BUTTON, 0, 0.060f, 0.088f, 0.047f, "RT", "RT", 0xE6E6E6, "trigger_r" },   /* (the camera: small,
                                                       in the corner clear of the health emblem:
                                                       place_at) */
    [BACK]    = { BUTTON, 2, -0.085f, 0.89f, 0.055f, "BACK", "BACK", 0xE6E6E6, "back" },   /* (Back and Start off the */
    [START]   = { BUTTON, 2, 0.085f, 0.89f,  0.055f, "START", "START", 0xE6E6E6, "start" },   /* gesture bar; larger and
                                                       nearer each other, both pressed at once for a reset: #284) */
    [LS]      = { BUTTON, 0, 0.09f,  0.90f,  0.04f,  "LS", "LS", 0xE6E6E6, "click_l" },
    [RS]      = { BUTTON, 1, 0.09f,  0.90f,  0.045f, "RS", "RS", 0xE6E6E6, "click_r" },
    [SWAP]    = { BUTTON, 0, 0.05f,  0.36f,  0.035f, "layout", "<>", 0xE6E6E6, "swap" },
    [NINPO]   = { COMBO, 1, 0.52f, 0.46f, 0.060f, "NINPO", "N", 0x4C9BEA, "button_y" },
    [WINDRUN] = { COMBO, 1, 0.52f, 0.62f, 0.060f, "WIND RUN", "W", 0x4C9BEA, "button_x" },
    [CUSTOM1] = { COMBO, 1, 0.52f, 0.32f, 0.055f, "C1", "C1", 0x4C9BEA, "button_a" },
    [CUSTOM2] = { COMBO, 1, 0.52f, 0.44f, 0.055f, "C2", "C2", 0x4C9BEA, "button_b" },
    [CUSTOM3] = { COMBO, 1, 0.52f, 0.56f, 0.055f, "C3", "C3", 0x4C9BEA, "button_x" },
    [CUSTOM4] = { COMBO, 1, 0.52f, 0.68f, 0.055f, "C4", "C4", 0x4C9BEA, "button_y" },
};

/* Classic: the places of 0.9.x's controls, in the same pictures (#285: Block held by a left finger at the top
 * left, the camera's RT at the top right with the camera toggle (RS) under it, the left stick low and large where
 * the thumb rests, the face buttons lower). Start and Back stay in the middle (#284). The switch beside the left
 * stick, at the left edge, changes between this and the default, kept in settings.ini (touch_layout). */
static const struct { int edge; float x, y, r; } s_classic[NCTRL] = {
    [LSTICK]  = { 0, 0.30f, 0.70f, 0.17f },
    [RSTICK]  = { 1, 0.72f, 0.66f, 0.12f },
    [DPAD_ID] = { 0, 0.74f, 0.84f, 0.12f },
    [BA]      = { 1, 0.28f, 0.70f, 0.075f },
    [BB]      = { 1, 0.16f, 0.58f, 0.07f },
    [BX]      = { 1, 0.40f, 0.58f, 0.075f },
    [BY]      = { 1, 0.28f, 0.46f, 0.075f },
    [LB]      = { 0, 0.30f, 0.28f, 0.05f },
    [RB]      = { 1, 0.30f, 0.28f, 0.05f },
    [LT]      = { 0, 0.13f, 0.14f, 0.085f },
    [RT]      = { 1, 0.13f, 0.14f, 0.085f },
    [BACK]    = { 2, -0.085f, 0.89f, 0.055f },
    [START]   = { 2, 0.085f, 0.89f, 0.055f },
    [LS]      = { 0, 0.09f, 0.90f, 0.04f },
    [RS]      = { 1, 0.09f, 0.30f, 0.045f },
    [SWAP]    = { 0, 0.05f, 0.40f, 0.035f },
};
int ngb_pc_setting(int which);                    /* src/ingame_menu.c (48: the touch layout) */
void ngb_pc_set_setting(int which, int value);
#pragma weak ngb_pc_setting
#pragma weak ngb_pc_set_setting
static int s_layout = -1, s_layout_save;          /* 0 the default, 1 Classic (under s_lock); to be saved */
static unsigned s_serial = 1;
/* (under s_lock, on the game's side: touch_init) the layout, read from the settings; the controls are drawn only
 * once it is known, so never in one layout and found in the other */
static void layout_known(void)
{
    if (s_layout < 0) {
        s_layout = ngb_pc_setting && ngb_pc_setting(48) == 1;
        s_serial++;
    }
}

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static float s_aspect = 16.0f / 9;
static float s_layout_x[NCTRL], s_layout_y[NCTRL], s_layout_r[NCTRL], s_layout_alpha[NCTRL];
static float d_layout_x[NCTRL], d_layout_y[NCTRL], d_layout_r[NCTRL], d_layout_alpha[NCTRL];
static bool s_layout_custom[NCTRL], d_layout_custom[NCTRL], s_layout_loaded, s_edit_mode, d_edit_mode;
static uint8_t s_layout_color[NCTRL], d_layout_color[NCTRL], s_layout_shape[NCTRL], d_layout_shape[NCTRL], s_layout_skin[NCTRL], d_layout_skin[NCTRL], s_layout_combo[NCTRL], d_layout_combo[NCTRL], s_layout_enabled[NCTRL], d_layout_enabled[NCTRL];
static Uint32 s_combo_started[NCTRL];
static bool s_combo_active[NCTRL];
static bool s_haptics = true;
static bool d_haptics = true;
static char s_skin_path[512], d_skin_path[512];
static int s_selected = -1, d_selected = -1;
static int s_preset, d_preset;
static unsigned s_layout_revision, d_layout_revision;
static char s_layout_path[512];
static bool s_down[NCTRL];
static float s_stick[2][2];                   /* each stick's knob, -1..1 (y down) */
static int s_dpad;                            /* XBOX_GAMEPAD_DPAD_* bits */
/* s_serial: +1 each change of what the picture shows (declared with the layout, above) */
static int s_art_shown;                       /* the controls drawn (the last picture's; any thread reads it) */

#define MAX_FINGERS 10
static struct { SDL_FingerID id; Uint32 down_timestamp; int ctrl; bool used, moved, was_selected; float x0, y0; } s_finger[MAX_FINGERS];
static void layout_defaults(void) {
    for (int i=0;i<NCTRL;i++) { s_layout_x[i]=s_ctrl[i].x; s_layout_y[i]=s_ctrl[i].y; s_layout_r[i]=s_ctrl[i].r; s_layout_alpha[i]=(i>=BA&&i<=BY)?0.72f:0.55f; s_layout_custom[i]=false; s_layout_color[i]=s_layout_shape[i]=s_layout_skin[i]=s_layout_combo[i]=0; s_layout_enabled[i]=i<CUSTOM1; } s_layout_combo[NINPO]=4; s_layout_combo[WINDRUN]=1;
}
static void layout_load(void) {
    if(s_layout_loaded)return; s_layout_loaded=true; layout_defaults(); s_skin_path[0]=0;
    char *dir=SDL_GetPrefPath("NinjaGaidenBlack","touch-controls"); if(!dir)return;
    char active[512]; snprintf(active,sizeof active,"%s/touch-controls-active.cfg",dir);
    FILE *af=fopen(active,"r"); if(af){int p;if(fscanf(af,"%d",&p)==1&&p>=0&&p<=2)s_preset=p;fclose(af);}
    snprintf(s_layout_path,sizeof s_layout_path,"%s/touch-controls-preset-%d.cfg",dir,s_preset);
    SDL_free(dir);
    FILE *fp=fopen(s_layout_path,"r");
    if(!fp&&s_preset==0){char *legacy=SDL_GetPrefPath("NinjaGaidenBlack","touch-controls");if(legacy){snprintf(s_layout_path,sizeof s_layout_path,"%s/touch-controls-v1.cfg",legacy);SDL_free(legacy);fp=fopen(s_layout_path,"r");}}
    if(!fp)return; char line[192]; bool legacy_ids=true;
    while(fgets(line,sizeof line,fp)){if(line[0]=='#'){if(strstr(line,"NGB_TOUCH_LAYOUT_V2")){legacy_ids=false;int h;if(sscanf(line,"# NGB_TOUCH_LAYOUT_V2 %d",&h)==1)s_haptics=h!=0;}else if(strstr(line,"SKIN "))sscanf(line,"# SKIN %511s",s_skin_path);continue;}int id,custom,color=0,shape=0,skin=0,combo=0,enabled=1;float x,y,r,a;int n=sscanf(line,"%d %f %f %f %f %d %d %d %d %d %d",&id,&x,&y,&r,&a,&custom,&color,&shape,&skin,&enabled,&combo);if(n<6||id<0)continue;if(legacy_ids&&id>=15)id++;if(id>=NCTRL)continue;s_layout_x[id]=x;s_layout_y[id]=y;s_layout_r[id]=r;s_layout_alpha[id]=a;s_layout_custom[id]=custom!=0;if(n>=7)s_layout_color[id]=(uint8_t)(color<0?0:color>5?5:color);if(n>=8)s_layout_shape[id]=(uint8_t)(shape<0?0:shape>2?2:shape);if(n>=9)s_layout_skin[id]=(uint8_t)(skin!=0);if(n>=10)s_layout_enabled[id]=enabled!=0;if(n>=11)s_layout_combo[id]=(uint8_t)(combo<0?0:combo>6?6:combo);}
    fclose(fp);
    for(int i=0;i<NCTRL;i++){s_layout_r[i]=fminf(.25f,fmaxf(.035f,s_layout_r[i]));s_layout_x[i]=fminf(8.f,fmaxf(s_ctrl[i].edge==2?-8.f:.04f,s_layout_x[i]));s_layout_y[i]=fminf(.97f,fmaxf(.03f,s_layout_y[i]));s_layout_alpha[i]=fminf(1.f,fmaxf(.15f,s_layout_alpha[i]));}
}
static void layout_save(void) {
    if(!s_layout_path[0]){char *dir=SDL_GetPrefPath("NinjaGaidenBlack","touch-controls");if(!dir)return;snprintf(s_layout_path,sizeof s_layout_path,"%s/touch-controls-preset-%d.cfg",dir,s_preset);SDL_free(dir);}
    char *dir=SDL_GetPrefPath("NinjaGaidenBlack","touch-controls");
    if(dir){char active[512];snprintf(active,sizeof active,"%s/touch-controls-active.cfg",dir);FILE *af=fopen(active,"w");if(af){fprintf(af,"%d\\n",s_preset);fclose(af);}SDL_free(dir);}
    FILE *fp=fopen(s_layout_path,"w");if(!fp)return;
    fprintf(fp,"# NGB_TOUCH_LAYOUT_V2 %d\n",s_haptics?1:0);
    if(s_skin_path[0])fprintf(fp,"# SKIN %s\n",s_skin_path);
    for(int i=0;i<NCTRL;i++)fprintf(fp,"%d %.5f %.5f %.5f %.5f %d %u %u %u %u %u\n",i,s_layout_x[i],s_layout_y[i],s_layout_r[i],s_layout_alpha[i],s_layout_custom[i]?1:0,s_layout_color[i],s_layout_shape[i],s_layout_skin[i],s_layout_enabled[i]?1:0,s_layout_combo[i]);
    fclose(fp);
}
static void clear_touch_state(void){memset(s_finger,0,sizeof s_finger);memset(s_down,0,sizeof s_down);memset(s_stick,0,sizeof s_stick);memset(s_combo_active,0,sizeof s_combo_active);s_dpad=0;}
static void layout_switch_preset(void) {
    layout_save();
    s_preset=(s_preset+1)%3;
    s_layout_path[0]=0; s_layout_loaded=false;
    layout_load();
    s_layout = s_preset==1 ? 1 : 0; s_layout_save=1;
    s_selected=-1; clear_touch_state();
    s_layout_revision++; layout_save(); s_serial++;
}
static void toggle_editor(void){s_edit_mode=!s_edit_mode;s_selected=-1;clear_touch_state();if(!s_edit_mode)layout_save();s_layout_revision++;s_serial++;}
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

static void editor_toolbar_action(float x){float q=x/s_aspect;if(q>.12f&&q<.22f){s_haptics=!s_haptics;layout_save();s_serial++;return;}if(q<.105f){add_custom_combo();return;}if(q>.895f){delete_selected_combo();return;}if(q>.455f&&q<.545f){toggle_editor();return;}if(s_selected<0||s_selected>=NCTRL)return;if(q>.245f&&q<.335f){float v=s_layout_r[s_selected]/s_ctrl[s_selected].r;s_layout_r[s_selected]=s_ctrl[s_selected].r*(v<.88f?1.f:v<1.12f?1.25f:.75f);}else if(q>.355f&&q<.445f){static const float a[]={.25f,.4f,.55f,.7f,.85f,1.f};int n=0;while(n<6&&s_layout_alpha[s_selected]>=a[n]-.01f)n++;s_layout_alpha[s_selected]=a[n==6?0:n];}else if(q>.555f&&q<.645f)s_layout_color[s_selected]=(s_layout_color[s_selected]+1)%6;else if(q>.665f&&q<.755f)s_layout_shape[s_selected]=(s_layout_shape[s_selected]+1)%3;else if(q>.775f&&q<.865f){if(s_ctrl[s_selected].kind==COMBO)s_layout_combo[s_selected]=(s_layout_combo[s_selected]+1)%7;else s_layout_skin[s_selected]^=1;}else return;s_layout_revision++;layout_save();s_serial++;}

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
        }
    }
    if (!custom && layout == 1 && aspect < 1.7f && (c == &s_ctrl[LSTICK] || c == &s_ctrl[DPAD_ID])) {
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
        float dx = x - at.x, dy = y - at.y, d;
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
        if (ctrl >= NINPO && s_layout_combo[ctrl] == 6) { s_combo_active[ctrl] = true; s_combo_started[ctrl] = SDL_GetTicks(); }
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

static void touch_pick_skin(void) {
    if (s_selected < 0 || s_selected >= NCTRL) return;
    s_layout_skin[s_selected] = true;
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
        if (strcmp(path, s_skin_path)) { snprintf(s_skin_path, sizeof s_skin_path, "%s", path); s_layout_revision++; layout_save(); s_serial++; }
        pthread_mutex_unlock(&s_lock);
    }
#endif
}

static int SDLCALL touch_watch(void *unused, SDL_Event *ev)
{
    (void)unused;
    if(ev->type!=SDL_FINGERDOWN&&ev->type!=SDL_FINGERMOTION&&ev->type!=SDL_FINGERUP)return 0;
    if(ev->type!=SDL_FINGERUP&&xbox_InputControllerPresent())return 0;
    if(ev->type==SDL_FINGERDOWN&&!__atomic_load_n(&s_art_shown,__ATOMIC_ACQUIRE))return 0;
    layout_load(); pthread_mutex_lock(&s_lock);
    SDL_Window *w=SDL_GetWindowFromID(ev->tfinger.windowID);int ww=0,wh=0;if(w)SDL_GetWindowSize(w,&ww,&wh);if(ww>0&&wh>0)s_aspect=(float)ww/(float)wh;
    float x=ev->tfinger.x*s_aspect,y=ev->tfinger.y;
    if(ev->type==SDL_FINGERDOWN&&x>s_aspect-.045f&&y>.44f&&y<.56f){if(s_edit_mode)touch_pick_skin();else toggle_editor();pthread_mutex_unlock(&s_lock);return 0;}
    if(ev->type==SDL_FINGERDOWN&&s_edit_mode&&y>.115f&&y<.18f&&x>s_aspect*.40f&&x<s_aspect*.60f){layout_switch_preset();pthread_mutex_unlock(&s_lock);return 0;}
    if(ev->type==SDL_FINGERDOWN&&y<.105f){if(s_edit_mode)editor_toolbar_action(x);else if(x>s_aspect*.455f&&x<s_aspect*.545f)toggle_editor();pthread_mutex_unlock(&s_lock);return 0;}
    if(ev->type==SDL_FINGERDOWN&&hit(x,y,false)==SWAP){clear_touch_state();s_layout=s_layout!=1;s_layout_save=1;s_serial++;pthread_mutex_unlock(&s_lock);return 0;}
    int f;for(f=0;f<MAX_FINGERS&&!(s_finger[f].used&&s_finger[f].id==ev->tfinger.fingerId);f++);
    if(ev->type==SDL_FINGERDOWN&&f==MAX_FINGERS){
        int hit_id=hit(x,y,false);for(f=0;f<MAX_FINGERS&&s_finger[f].used;f++);
        if(f<MAX_FINGERS&&s_edit_mode){s_finger[f].used=true;s_finger[f].id=ev->tfinger.fingerId;s_finger[f].down_timestamp=ev->tfinger.timestamp;s_finger[f].ctrl=hit_id;s_finger[f].x0=x;s_finger[f].y0=y;s_finger[f].moved=false;s_finger[f].was_selected=hit_id==s_selected;if(hit_id>=0)s_selected=hit_id;s_serial++;}
        else if(hit_id>=0&&f<MAX_FINGERS){xbox_InputTouchUsed();s_finger[f].used=true;s_finger[f].id=ev->tfinger.fingerId;s_finger[f].ctrl=hit_id;finger_on(hit_id,x,y);s_serial++;}
        else if(hit_id<0&&f<MAX_FINGERS){s_finger[f].used=true;s_finger[f].id=ev->tfinger.fingerId;s_finger[f].ctrl=-2;s_finger[f].x0=x;s_finger[f].y0=y;}
    }else if(ev->type==SDL_FINGERMOTION&&f<MAX_FINGERS){
        int id=s_finger[f].ctrl;
        if(!s_edit_mode&&id==-2){float dx=x-s_finger[f].x0,dy=y-s_finger[f].y0;if(fabsf(dx)+fabsf(dy)>.002f){s_stick[1][0]=fminf(1.f,fmaxf(-1.f,dx*20.0f));s_stick[1][1]=fminf(1.f,fmaxf(-1.f,dy*20.0f));s_down[RSTICK]=true;xbox_InputTouchUsed();s_finger[f].x0=x;s_finger[f].y0=y;s_serial++;}}
        else if(s_edit_mode){if(id>=0){float dx=x-s_finger[f].x0,dy=y-s_finger[f].y0;if(fabsf(dx)+fabsf(dy)>.012f)s_finger[f].moved=true;if(s_finger[f].moved){s_layout_x[id]+=s_ctrl[id].edge==1?-dx:dx;s_layout_y[id]+=dy;s_layout_custom[id]=true;if(s_ctrl[id].edge==2)s_layout_x[id]=fminf(s_aspect/2-.04f,fmaxf(-s_aspect/2+.04f,s_layout_x[id]));else s_layout_x[id]=fminf(s_aspect-.04f,fmaxf(.04f,s_layout_x[id]));s_layout_y[id]=fminf(.97f,fmaxf(.03f,s_layout_y[id]));s_finger[f].x0=x;s_finger[f].y0=y;}}}
        else if(id>=0&&s_ctrl[id].kind==BUTTON&&id>=BA&&id<=BY){int to=hit(x,y,true);if(to>=BA&&to<=BY&&to!=id){s_finger[f].ctrl=to;finger_off(id);finger_on(to,x,y);s_serial++;}}
        else if(id==LSTICK||id==RSTICK||id==DPAD_ID){int si=id==RSTICK,wd=s_dpad;float wx=id==DPAD_ID?0:s_stick[si][0],wy=id==DPAD_ID?0:s_stick[si][1];finger_on(id,x,y);if(id==DPAD_ID?s_dpad!=wd:fabsf(s_stick[si][0]-wx)+fabsf(s_stick[si][1]-wy)>1.f/32)s_serial++;}
    }else if(ev->type==SDL_FINGERUP&&f<MAX_FINGERS){int id=s_finger[f].ctrl;bool moved=s_finger[f].moved,was=s_finger[f].was_selected;Uint32 held=ev->tfinger.timestamp-s_finger[f].down_timestamp;s_finger[f].used=false;if(s_edit_mode){if(!moved&&was&&id>=0)edit_selected(held);if(moved){s_layout_revision++;layout_save();s_serial++;}if(!moved&&id<0&&y>.86f){layout_defaults();layout_save();s_layout_revision++;s_selected=-1;}}else if(id==-2)finger_off(RSTICK);else if(id>=0)finger_off(id);s_serial++;}
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
    pthread_mutex_lock(&s_lock);
    int save = s_layout_save ? s_layout : -1;   /* (written here, on the game's side, not on Android's UI thread) */
    s_layout_save = 0;
    pthread_mutex_unlock(&s_lock);
    if (save >= 0 && ngb_pc_set_setting)
        ngb_pc_set_setting(48, save);
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
    static const int combo_buttons[6][2] = {
        {XBOX_BUTTON_A,XBOX_BUTTON_B},{XBOX_BUTTON_A,XBOX_BUTTON_X},{XBOX_BUTTON_A,XBOX_BUTTON_Y},
        {XBOX_BUTTON_B,XBOX_BUTTON_X},{XBOX_BUTTON_B,XBOX_BUTTON_Y},{XBOX_BUTTON_X,XBOX_BUTTON_Y}
    };
    if (s_down[LSTICK]) {
        st->Gamepad.sThumbLX = (SHORT)(s_stick[0][0] * 32767);
        st->Gamepad.sThumbLY = (SHORT)(-s_stick[0][1] * 32767);
    }
    if (s_down[RSTICK]) {
        st->Gamepad.sThumbRX = (SHORT)(s_stick[1][0] * 32767);
        st->Gamepad.sThumbRY = (SHORT)(-s_stick[1][1] * 32767);
    }
    Uint32 combo_now = SDL_GetTicks();
    for (int i=NINPO;i<NCTRL;i++) {
        if (s_layout_combo[i] < 6) {
            if (s_down[i]) { int p=s_layout_combo[i]; st->Gamepad.bAnalogButtons[combo_buttons[p][0]]=255; st->Gamepad.bAnalogButtons[combo_buttons[p][1]]=255; }
        } else if (s_combo_active[i]) {
            Uint32 elapsed = combo_now - s_combo_started[i];
            if (elapsed < 360) {
                float angle = (float)elapsed * (2.0f * (float)M_PI / 360.0f);
                st->Gamepad.sThumbLX = (SHORT)(cosf(angle) * 32767.0f);
                st->Gamepad.sThumbLY = (SHORT)(-sinf(angle) * 32767.0f);
            } else if (elapsed < 480) {
                st->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
            } else s_combo_active[i] = false;
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
static int d_dpad;

typedef struct { uint8_t *px; int w, h; } Sprite;   /* straight-alpha RGBA */
static const Sprite *user_skin(void) {
    static Sprite image;
    static char loaded[512];
    if (!d_skin_path[0]) return NULL;
    if (strcmp(loaded, d_skin_path)) {
        if (image.px) stbi_image_free(image.px);
        memset(&image, 0, sizeof image);
        snprintf(loaded, sizeof loaded, "%s", d_skin_path);
        int channels = 0;
        image.px = stbi_load(loaded, &image.w, &image.h, &channels, 4);
        if (image.px && (image.w > 2048 || image.h > 2048 || image.w < 8 || image.h < 8)) { stbi_image_free(image.px); memset(&image, 0, sizeof image); }
    }
    return image.px ? &image : NULL;
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

#define STANDIN_AFTER_MS 20000   /* frames without the game's art, the stand-ins then */

/* The presenter's, each frame it asks for the picture: the game's pieces once there (saved by an earlier start, or
 * made); the stand-ins after STANDIN_AFTER_MS of frames without them (each frame counted at most 0.1 s, so a long
 * load does not count) -- or at once in the arcade, which has no game art -- until the game's come */
static enum Art art_now(void)
{
    static int standin;
    static uint64_t last;
    static uint32_t waited;
    if (ngb_gp_touch && ngb_gp_touch("pad"))
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
typedef struct { const uint8_t *src; int sw, sh, w, h; float bw, bh, ox, oy; uint32_t *px; } Scaled;
#define SCALED_N 96
static Scaled s_scaled[SCALED_N];
static int s_nscaled;

static const Scaled *scaled(const Sprite *sp, int w, int h, float bw, float bh, float ox, float oy)
{
    for (int i = 0; i < s_nscaled; i++) {
        const Scaled *c = &s_scaled[i];
        if (c->src == sp->px && c->sw == sp->w && c->sh == sp->h && c->w == w && c->h == h &&
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
    *c = (Scaled){ sp->px, sp->w, sp->h, w, h, bw, bh, ox, oy, px };
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
    (void)c;
    if (d_layout_combo[id] == 6) { const Sprite *skin=d_layout_skin[id]?user_skin():NULL; if(skin) draw_sprite(skin,cx,cy,2.5f*r,2.5f*r,a); else if(s_art==ART_GAME) draw_piece("button_y",cx,cy,2.5f*r,2.5f*r,a); else draw_sprite(standin_button(id),cx,cy,2.5f*r,2.5f*r,a); draw_badge_text("360Y",(int)cx,(int)(cy-9),46); return; }
    if (s_art == ART_GAME && !d_layout_skin[id]) {
        draw_piece(combo_first(id), cx - 0.38f*r, cy, 1.65f*r, 1.65f*r, a);
        draw_piece(combo_second(id), cx + 0.38f*r, cy, 1.65f*r, 1.65f*r, a);
    } else if (d_layout_skin[id] && user_skin()) draw_sprite(user_skin(), cx, cy, 2.5f*r, 2.5f*r, a);
    else draw_sprite(standin_button(id), cx, cy, 2.5f*r, 2.5f*r, a);
}

static void draw_button(const Ctrl *c, bool lit, float cx, float cy, float r, float a)
{
    if (s_art == ART_GAME && !d_layout_skin[(int)(c - s_ctrl)]) {
        char n[40];
        snprintf(n, sizeof n, lit ? "%s_lit" : "%s", c->piece);
        draw_piece(n, cx, cy, 2.5f * r, 2.5f * r, a);
    } else {   /* (the disc about the button's size) */
        int id = (int)(c - s_ctrl);
        const Sprite *skin = d_layout_skin[id] ? user_skin() : NULL;
        if (skin) draw_sprite(skin, cx, cy, (lit ? 2.6f : 2.5f) * r, (lit ? 2.6f : 2.5f) * r, a);
        else draw_sprite(standin_button(id), cx, cy, (lit ? 2.6f : 2.5f) * r, (lit ? 2.6f : 2.5f) * r, a);
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

static void draw_idle(void)
{
    float u = (float)s_h;
    memset(s_px, 0, sizeof(uint32_t) * (size_t)s_w * s_h);
    for (int i = 0; i < NCTRL; i++) {
        if (!d_layout_enabled[i]) continue;
        const Ctrl *c = &s_ctrl[i];
        Place at = place_at(c, d_aspect, s_art == ART_STANDIN, d_layout);
        if (d_layout_custom[i]) { at.x=c->edge==2?d_aspect/2+d_layout_x[i]:c->edge?d_aspect-d_layout_x[i]:d_layout_x[i]; at.y=d_layout_y[i]; at.r=d_layout_r[i]; }
        float cx = at.x * u, cy = at.y * u, r = at.r * u;
        if (c->kind == STICK)
            draw_piece("pad", cx, cy, 2 * r, 2 * r, d_edit_mode && i == d_selected ? 0.98f : d_layout_alpha[i]);
        else if (c->kind == DPAD)
            draw_piece("dpad", cx, cy, 2.2f * r, 2.2f * r, d_edit_mode && i == d_selected ? 0.98f : d_layout_alpha[i]);
        else if (c->kind == COMBO)
            draw_combo(c, i, cx, cy, r, d_edit_mode && i == d_selected ? 0.98f : d_layout_alpha[i]);
        else
            draw_button(c, false, cx, cy, r, d_edit_mode && i == d_selected ? 0.98f : d_layout_alpha[i]);   /* (the face buttons
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


static void draw_style_outline(float cx, float cy, float r, int color, int shape, bool selected)
{
    if (!selected && color == 0 && shape == 0) return;
    static const uint8_t colors[6][3] = {
        {55, 166, 255}, {40, 112, 220}, {92, 132, 255},
        {24, 72, 160}, {130, 160, 255}, {18, 42, 92}
    };
    int ci = color < 0 || color > 5 ? 0 : color;
    uint8_t cr = selected ? 255 : colors[ci][0], cg = selected ? 210 : colors[ci][1], cb = selected ? 90 : colors[ci][2];
    float outer = r * 1.20f, inner = outer - fmaxf(1.5f, r * 0.055f);
    int x0 = (int)floorf(cx - outer - 1), x1 = (int)ceilf(cx + outer + 1);
    int y0 = (int)floorf(cy - outer - 1), y1 = (int)ceilf(cy + outer + 1);
    over_box(x0, y0, x1 + 1, y1 + 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = fabsf(x + 0.5f - cx), dy = fabsf(y + 0.5f - cy), d;
            if (shape == 1) d = fmaxf(dx, dy);
            else if (shape == 2) d = (dx + dy) * 0.70710678f;
            else d = hypotf(dx, dy);
            if (d >= inner && d <= outer)
                badge_pixel(x, y, cr, cg, cb, 230);
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
static void draw_editor_badge(void)
{
    draw_badge_text(d_edit_mode ? "DONE" : "EDIT", s_w / 2, 2, 58);
    static const char *presets[] = {"DEFAULT","CLASSIC","CUSTOM"};
    if (d_edit_mode) draw_badge_text(presets[d_preset >= 0 && d_preset < 3 ? d_preset : 0], s_w / 2, 24, 72);
    draw_badge_text(d_edit_mode ? "LOAD" : "EDIT", s_w - 30, s_h / 2 - 9, 54);
    if (d_edit_mode) {
        draw_badge_text("ADD", (int)(s_w * 0.07f), 2, 42);
        draw_badge_text(d_haptics ? "VIBE" : "MUTE", (int)(s_w * 0.17f), 2, 48);
        draw_badge_text("DEL", (int)(s_w * 0.93f), 2, 42);
        draw_badge_text("SIZE", (int)(s_w * 0.29f), 2, 48);
        draw_badge_text("ALPHA", (int)(s_w * 0.40f), 2, 52);
        draw_badge_text("COLOR", (int)(s_w * 0.60f), 2, 52);
        draw_badge_text("SHAPE", (int)(s_w * 0.71f), 2, 52);
        draw_badge_text(d_selected >= 0 && d_selected < NCTRL && s_ctrl[d_selected].kind == COMBO ? "MAP" : "SKIN", (int)(s_w * 0.82f), 2, 42);
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
        s_bg_layout != d_layout || s_bg_layout_revision != d_layout_revision) {
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
        if (d_layout_custom[i]) { at.x=c->edge==2?d_aspect/2+d_layout_x[i]:c->edge?d_aspect-d_layout_x[i]:d_layout_x[i]; at.y=d_layout_y[i]; at.r=d_layout_r[i]; }
        float cx = at.x * u, cy = at.y * u, r = at.r * u;
        bool on = d_down[i];
        if (c->kind == STICK) {
            int s = i == RSTICK;
            float nx = cx + d_stick[s][0] * r * 0.55f, ny = cy + d_stick[s][1] * r * 0.55f;
            nx = roundf(nx - r / 2) + r / 2;   /* (moved by whole pixels: its picture at this size made once) */
            ny = roundf(ny - r / 2) + r / 2;
            draw_piece("nub", nx, ny, r, r, on ? 0.95f : IDLE + 0.2f);
            if (on) {
                draw_piece("pad_lit", cx, cy, 2 * r, 2 * r, 0.9f);
                draw_piece("nub_lit", nx, ny, r, r, 0.9f);
            }
        } else if (c->kind == DPAD && on) {
            static const struct { int bit; const char *lit; float dx, dy; } arms[] = {
                { XBOX_GAMEPAD_DPAD_UP, "dpad_north_lit", 0, -1 },
                { XBOX_GAMEPAD_DPAD_DOWN, "dpad_south_lit", 0, 1 },
                { XBOX_GAMEPAD_DPAD_LEFT, "dpad_west_lit", -1, 0 },
                { XBOX_GAMEPAD_DPAD_RIGHT, "dpad_east_lit", 1, 0 },
            };
            float size = 2.2f * r;   /* (the picture's arms are a third of it each) */
            draw_piece("dpad", cx, cy, size, size, 0.45f);   /* (over the untouched one: less see-through) */
            for (int a = 0; a < 4; a++)
                if (d_dpad & arms[a].bit)
                    draw_piece(arms[a].lit, cx + arms[a].dx * size / 3, cy + arms[a].dy * size / 3, size / 2,
                               size / 2, 0.95f);
        } else if (c->kind == COMBO && on) {
            draw_combo(c, i, cx, cy, r, fmaxf(d_layout_alpha[i], 0.85f));
            draw("touch_button_lit", cx, cy, 2.1f*r, 2.1f*r, 0.9f);
        } else if (c->kind == BUTTON && on) {
            draw_button(c, true, cx, cy, r, 1.0f);   /* (the game's pieces: their own held picture) */
        }
    }
    for (int i=0;i<NCTRL;i++) { if(!d_layout_enabled[i]) continue; const Ctrl *c=&s_ctrl[i]; Place p=place_at(c,d_aspect,s_art==ART_STANDIN,d_layout); if(d_layout_custom[i]){p.x=c->edge==2?d_aspect/2+d_layout_x[i]:c->edge?d_aspect-d_layout_x[i]:d_layout_x[i];p.y=d_layout_y[i];p.r=d_layout_r[i];} float cx=p.x*u,cy=p.y*u,r=p.r*u; if(d_layout_skin[i]&&c->kind==BUTTON) draw("touch_button_lit",cx,cy,2.2f*r,2.2f*r,0.30f); draw_style_outline(cx,cy,r,d_layout_color[i],d_layout_shape[i],d_edit_mode&&i==d_selected); }
    draw_editor_badge();
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
        memcpy(d_layout_x,s_layout_x,sizeof d_layout_x); memcpy(d_layout_y,s_layout_y,sizeof d_layout_y); memcpy(d_layout_r,s_layout_r,sizeof d_layout_r); memcpy(d_layout_alpha,s_layout_alpha,sizeof d_layout_alpha); memcpy(d_layout_custom,s_layout_custom,sizeof d_layout_custom); memcpy(d_layout_color,s_layout_color,sizeof d_layout_color); memcpy(d_layout_shape,s_layout_shape,sizeof d_layout_shape); memcpy(d_layout_skin,s_layout_skin,sizeof d_layout_skin); memcpy(d_layout_combo,s_layout_combo,sizeof d_layout_combo); memcpy(d_layout_enabled,s_layout_enabled,sizeof d_layout_enabled);
        d_edit_mode=s_edit_mode; d_haptics=s_haptics; memcpy(d_skin_path,s_skin_path,sizeof d_skin_path); d_selected=s_selected; d_preset=s_preset; d_layout_revision=s_layout_revision;
        d_aspect = s_aspect;
        d_layout = s_layout == 1;   /* (known: layout_known above) */
    }
    pthread_mutex_unlock(&s_lock);
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
