/* Definitive-inspired root menu for libGame 2.4.379. The original game
 * still owns save/load, settings, confirmation dialogs and pause/resume. */
#include "frontend.h"
#include "rt.h"
#include "platform.h"
#include "tweaks.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <SDL3/SDL.h>
#include <string.h>

#define G 0x200000000ull
#define MENU (G + 0x9ec538)
#define SCREEN 0x110
#define PANEL 300.0f /* left panel width in 640x448 menu space */
#define HOST_SETTINGS 1000
#define MAP_SHIFT 0.09f /* map centre shift, fraction of screen width: the map screen's own legend column (37% of its width) then ends exactly under the left panel */
static void strip_capture(void);
static int map_active;
static int draw_pause_map(CPU *c, int render);
static void end_pause_map(CPU *c);
static unsigned first_menu_frame;
unsigned frontend_first_frame(void) { return first_menu_frame; }
static int root_ctx = 51; /* last root page seen: 51 boot, 69 boot with save, 52 pause */
static int enabled = 1, selection, last_screen = -1, touch_row = -1;
static unsigned consumed_buttons;
static unsigned last_root_frame;
static int root_drawn;
extern void G_527bf4_orig(CPU *c);

void frontend_set_enabled(int value) { enabled = value != 0; }
static int menu_gate(void) {
    if (!enabled || !plat.in_ui || plat.loading_visible) return 0;
    /* The engine chooses page 51/69 before title/legal dismissal. The GOT
     * slot resolves to shownLegalScreen in this version. Until it is true,
     * let native rendering and input finish that startup flow. */
    uint64_t shown_legal = M64(G + 0x7fd478);
    if (!shown_legal || !M8(shown_legal)) return 0;
    return 1;
}
static int root_menu(void) {
    if (!menu_gate()) return 0;
    int s = MS32(MENU + SCREEN);
    if (s == 51 || s == 52 || s == 69) { root_ctx = s; return 1; }
    return 0;
}
static int engine_page(int s) { return s == 5 || s == 6 || s == 68 || s == 7; } /* Audio, Display, Game settings, Language */
static int game_page(void) { int s = MS32(MENU + SCREEN); return menu_gate() && (s == 2 || s == 41 || engine_page(s)); }
static int host_active(void) { return root_menu() || game_page(); }
/* The engine may select the root page before dismissing its title/legal
 * screens. Only take input after that page has actually been rendered. */
static int input_root(void) { return host_active() && root_drawn && plat.frames-last_root_frame <= 2; }
/* ---- host-drawn menu model -------------------------------------------------------------
 * Every list the player sees on the left is a Row array: root (engine pages 51/52/69), the host
 * SETTINGS list, the host GRAPHICS values, and the engine's GAME page (2), whose rows are read from
 * the engine's own aScreens table and driven through the native selection + A button, so each action
 * (save, load, new game, ...) runs the original code. Help/Brief/Stats and the leaf settings pages
 * stay native. */
enum { K_PAGE, K_SUB, K_RESUME, K_NATIVE, K_BACK, K_VALUE, K_BIND, K_RESET, K_ENGINE, K_ACT, K_ACT_CLEAR, K_MAP };
typedef struct Row { const char *label; int kind; int arg; } Row;
enum { S_FPS, S_VSYNC, S_DRAW, S_RES, S_RADAR, S_MODE, S_FOCUS, S_RADAR_SIZE, S_FOV, S_COUNT };
static const Row boot_rows[] = {{"GAME",K_PAGE,2},{"SETTINGS",K_PAGE,41},{"EXIT GAME",K_PAGE,42}};
static const Row continue_rows[] = {{"CONTINUE",K_PAGE,14},{"GAME",K_PAGE,2},{"SETTINGS",K_PAGE,41},{"EXIT GAME",K_PAGE,42}};
static const Row pause_rows[] = {{"RESUME",K_RESUME,0},{"MAP",K_MAP,0},{"GAME",K_PAGE,2},{"SETTINGS",K_PAGE,41},{"STATS",K_PAGE,1},{"BRIEF",K_PAGE,3},{"HELP",K_PAGE,62},{"EXIT GAME",K_PAGE,42}};
static const Row settings_rows[] = {{"GRAPHICS",K_SUB,2},{"CONTROLS",K_SUB,3},{"GAMEPLAY",K_PAGE,68},{"AUDIO",K_PAGE,5},{"DISPLAY",K_PAGE,6},{"LANGUAGE",K_PAGE,7},{"BACK",K_BACK,0}};
static const Row graphics_rows[] = {{"DISPLAY MODE",K_VALUE,S_MODE},{"FRAME RATE",K_VALUE,S_FPS},{"VSYNC",K_VALUE,S_VSYNC},{"DRAW DISTANCE",K_VALUE,S_DRAW},{"RESOLUTION",K_VALUE,S_RES},{"RADAR",K_VALUE,S_RADAR},{"RADAR SIZE",K_VALUE,S_RADAR_SIZE},{"FIELD OF VIEW",K_VALUE,S_FOV},{"BACKGROUND PAUSE",K_VALUE,S_FOCUS},{"BACK",K_BACK,0}};
enum { C_LOOK = 20, C_SENS, C_MOUSE0 /* 22..24 */ };
/* button ids match main.c: A B X Y START BACK L1 R1 UP DOWN LEFT RIGHT L3 R3 */
static const char *button_names[14] = {"BUTTON A","BUTTON B","BUTTON X","BUTTON Y","START","SELECT","L1","R1","D-PAD UP","D-PAD DOWN","D-PAD LEFT","D-PAD RIGHT","L3","R3"};
static const Row controls_rows[] = {
    {"ON FOOT KEYS",K_SUB,4},{"VEHICLE KEYS",K_SUB,5},
    {"MOUSE LOOK",K_VALUE,C_LOOK},{"SENSITIVITY",K_VALUE,C_SENS},
    {"LEFT CLICK",K_VALUE,C_MOUSE0},{"RIGHT CLICK",K_VALUE,C_MOUSE0+1},{"MIDDLE CLICK",K_VALUE,C_MOUSE0+2},
    {"BUTTON A",K_BIND,0},{"BUTTON B",K_BIND,1},{"BUTTON X",K_BIND,2},{"BUTTON Y",K_BIND,3},{"START",K_BIND,4},{"SELECT",K_BIND,5},
    {"L1",K_BIND,6},{"R1",K_BIND,7},{"D-PAD UP",K_BIND,8},{"D-PAD DOWN",K_BIND,9},{"D-PAD LEFT",K_BIND,10},{"D-PAD RIGHT",K_BIND,11},
    {"L3",K_BIND,12},{"R3",K_BIND,13},{"RESET DEFAULTS",K_RESET,0},{"BACK",K_BACK,0}};
#define CONTROLS_COUNT ((int)(sizeof controls_rows / sizeof controls_rows[0]))
/* Per-action keys (main.c host_act_*, used by the Rust CPad ports). arg = vehicle << 8 | action << 1 | slot;
 * action ids match main.c `ACT_*`, slot 1 = positive key of a move axis. Unbound = the gamepad mapping. */
#define ACT(veh, a, slot) ((veh) << 8 | (a) << 1 | (slot))
static const Row foot_rows[] = {
    {"SPRINT",K_ACT,ACT(0,4,0)},{"JUMP",K_ACT,ACT(0,5,0)},{"ENTER VEHICLE",K_ACT,ACT(0,3,0)},{"ATTACK",K_ACT,ACT(0,6,0)},
    {"NEXT WEAPON",K_ACT,ACT(0,7,0)},{"PREVIOUS WEAPON",K_ACT,ACT(0,8,0)},{"PICK UP",K_ACT,ACT(0,9,0)},
    {"LOOK BEHIND",K_ACT,ACT(0,10,0)},{"MOVE FORWARD",K_ACT,ACT(0,12,0)},{"MOVE BACK",K_ACT,ACT(0,12,1)},
    {"MOVE LEFT",K_ACT,ACT(0,11,0)},{"MOVE RIGHT",K_ACT,ACT(0,11,1)},{"CLEAR ALL",K_ACT_CLEAR,0},{"BACK",K_BACK,0}};
static const Row car_rows[] = {
    {"ACCELERATE",K_ACT,ACT(1,0,0)},{"BRAKE / REVERSE",K_ACT,ACT(1,1,0)},{"HANDBRAKE",K_ACT,ACT(1,2,0)},
    {"STEER LEFT",K_ACT,ACT(1,13,0)},{"STEER RIGHT",K_ACT,ACT(1,13,1)},{"LEAN FORWARD",K_ACT,ACT(1,14,0)},
    {"LEAN BACK",K_ACT,ACT(1,14,1)},{"HORN",K_ACT,ACT(1,15,0)},{"LOOK LEFT",K_ACT,ACT(1,16,0)},
    {"LOOK RIGHT",K_ACT,ACT(1,17,0)},{"LOOK BEHIND",K_ACT,ACT(1,10,0)},{"EXIT VEHICLE",K_ACT,ACT(1,3,0)},{"DRIVE-BY",K_ACT,ACT(1,6,0)},{"NEXT WEAPON",K_ACT,ACT(1,7,0)},
    {"PREVIOUS WEAPON",K_ACT,ACT(1,8,0)},{"CLEAR ALL",K_ACT_CLEAR,1},{"BACK",K_BACK,0}};
int host_act_get(int veh, int a, int slot);
void host_act_set(int veh, int a, int slot, int k);
void host_act_clear(int veh);
void host_act_key_name(int k, char *o, size_t n);
static int capture_act = -1;
static int capture_btn = -1, scroll_top;
#define VISIBLE_ROWS 8
#define VALUE_X 196.0f /* value column, 640x448 menu space */
static Row game_rows[12]; static char game_labels[12][40]; static int game_count, game_built;
static int sub, resume_sub, keep_key = -1;
static unsigned native_release;
extern void host_native_button(int id, int down);

/* Menu sounds: the engine's own front-end sounds, the same calls its menu code makes
 * (cDMAudio::PlayFrontEndSound: 0xc3 navigate/slider, 0xc4 select, 0xc5 back). Input arrives outside guest
 * code, so the sound is queued and played from the next menu draw. */
enum { SND_MOVE, SND_SELECT, SND_BACK, SND_ADJUST };
static int pending_sound = -1;
static void ui_sound(int kind) {
    static const int ids[4] = {0xc3, 0xc4, 0xc5, 0xc3};
    pending_sound = ids[kind];
}
static void play_pending_sound(CPU *c) {
    if (pending_sound < 0) return;
    static uint64_t dmaudio;
    if (!dmaudio) dmaudio = rt_sym("DMAudio");
    if (dmaudio && !plat.no_audio) rt_invoke(c, G + 0x30fae8, 3, dmaudio, (uint64_t)pending_sound, 0ull);
    pending_sound = -1;
}

static const int fps_opts[] = {30, 60, 120};
static const float dd_opts[] = {1.0f, 1.5f, 2.0f, 2.5f, 3.0f};
static const float res_opts[] = {0.0f, 0.67f, 0.8f, 0.9f, 1.0f};
static int nearest(const float *o, int n, float v) {
    int b = 0; for (int i = 1; i < n; i++) if (fabsf(o[i]-v) < fabsf(o[b]-v)) b = i; return b;
}
static int fps_index(void) {
    int f = plat.fps_cap; if (f <= 0 || f > 120) f = 120;
    int b = 0; for (int i = 1; i < 3; i++) if (abs(fps_opts[i]-f) < abs(fps_opts[b]-f)) b = i; return b;
}
static void adjust(int row, int dir) {
    char v[32], v2[32];
    if (row == S_FPS) {
        int i = fps_index() + dir; if (i < 0) i = 0; if (i > 2) i = 2;
        plat.fps_cap = fps_opts[i]; snprintf(v, sizeof v, "%d", plat.fps_cap); tweaks_save("fps_cap", v);
    } else if (row == S_VSYNC) {
        plat.vsync = !plat.vsync; snprintf(v, sizeof v, "%d", plat.vsync); tweaks_save("vsync", v);
        SDL_GL_SetSwapInterval(plat.vsync);
    } else if (row == S_DRAW) {
        int i = nearest(dd_opts, 5, tweaks_draw_distance()) + dir; if (i < 0) i = 0; if (i > 4) i = 4;
        tweaks_set_draw_distance(dd_opts[i]);
    } else if (row == S_RES) {
        int i = nearest(res_opts, 5, tweaks_res_scale()) + dir; if (i < 0) i = 0; if (i > 4) i = 4;
        tweaks_set_res_scale(res_opts[i]);
    } else if (row == S_RADAR) tweaks_set_radar_bottom(!tweaks_radar_bottom());
    else if (row == S_FOV) {
        static const float o[] = {0.9f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f};
        int i = nearest(o, 6, tweaks_fov_scale()) + dir; if (i < 0) i = 0; if (i > 5) i = 5;
        tweaks_set_fov_scale(o[i]);
    }
    else if (row == S_RADAR_SIZE) {
        static const float o[] = {0.75f, 0.85f, 1.0f, 1.15f, 1.3f, 1.5f};
        int i = nearest(o, 6, tweaks_radar_scale()) + dir; if (i < 0) i = 0; if (i > 5) i = 5;
        tweaks_set_radar_scale(o[i]);
    }
    else if (row == S_MODE) host_set_display_mode((plat.display_mode + (dir > 0 ? 1 : 2)) % 3);
    else if (row == S_FOCUS) { plat.pause_on_focus_loss = !plat.pause_on_focus_loss; snprintf(v, sizeof v, "%d", plat.pause_on_focus_loss); tweaks_save("pause_on_focus_loss", v); }
    else if (row == C_LOOK) { plat.mouse_look = !plat.mouse_look; tweaks_save("mouse_look", plat.mouse_look ? "1" : "0"); }
    else if (row == C_SENS) {
        float v = plat.mouse_sensitivity + 0.25f * dir; if (v < 0.25f) v = 0.25f; if (v > 3.0f) v = 3.0f;
        plat.mouse_sensitivity = v; snprintf(v2, sizeof v2, "%.2f", v); tweaks_save("mouse_sensitivity", v2);
    } else if (row >= C_MOUSE0 && row < C_MOUSE0 + 3) {
        int b = host_mouse_bind(row - C_MOUSE0) + dir; if (b < -1) b = 13; if (b > 13) b = -1;
        host_mouse_bind_set(row - C_MOUSE0, b); snprintf(v2, sizeof v2, "%d", b); snprintf(v, sizeof v, "mbind_%d", row - C_MOUSE0); tweaks_save(v, v2);
    }
}
static void value_text(int row, char *o, size_t n) {
    switch (row) {
    case S_FPS: snprintf(o, n, "%d", fps_opts[fps_index()]); break;
    case S_VSYNC: snprintf(o, n, "%s", plat.vsync ? "ON" : "OFF"); break;
    case S_DRAW: snprintf(o, n, "%.1fX", tweaks_draw_distance()); break;
    case S_RES: { float r = tweaks_res_scale(); if (r <= 0) snprintf(o, n, "AUTO"); else snprintf(o, n, "%dX%d", ((int)(plat.width*r+.5f))&~1, ((int)(plat.height*r+.5f))&~1); break; }
    case S_RADAR: snprintf(o, n, "%s", tweaks_radar_bottom() ? "BOTTOM" : "TOP"); break;
    case S_RADAR_SIZE: snprintf(o, n, "%d%%", (int)(tweaks_radar_scale() * 100.0f + 0.5f)); break;
    case S_FOV: snprintf(o, n, "%d%%", (int)(tweaks_fov_scale() * 100.0f + 0.5f)); break;
    case S_MODE: snprintf(o, n, "%s", plat.display_mode == 0 ? "WINDOWED" : plat.display_mode == 2 ? "EXCLUSIVE" : "BORDERLESS"); break;
    case S_FOCUS: snprintf(o, n, "%s", plat.pause_on_focus_loss ? "ON" : "OFF"); break;
    case C_LOOK: snprintf(o, n, "%s", plat.mouse_look ? "ON" : "OFF"); break;
    case C_SENS: snprintf(o, n, "%.2f", plat.mouse_sensitivity); break;
    case C_MOUSE0: case C_MOUSE0 + 1: case C_MOUSE0 + 2: { int b = host_mouse_bind(row - C_MOUSE0); snprintf(o, n, "%s", b < 0 ? "NONE" : button_names[b]); break; }
    default: o[0] = 0;
    }
}
static void bind_text(int btn, char *o, size_t n) {
    if (capture_btn == btn) { snprintf(o, n, "PRESS A KEY"); return; }
    int a = host_bind_get(btn, 0), b = host_bind_get(btn, 1);
    const char *na = a ? SDL_GetScancodeName((SDL_Scancode)a) : "-", *nb = b ? SDL_GetScancodeName((SDL_Scancode)b) : NULL;
    (void)nb; snprintf(o, n, "%s", (na && *na) ? na : "-"); /* the secondary default (LShift / LCtrl) is not listed */
    for (char *p = o; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
}

/* Menu text is kept as UTF-8 on the host side and converted back to the engine's UTF-16 when drawn, so
 * accented and Cyrillic labels survive. Upper case like the native menu (ASCII, Latin-1, Cyrillic). */
static uint16_t upper16(uint16_t ch) {
    if (ch >= 'a' && ch <= 'z') return (uint16_t)(ch - 32);
    if (ch >= 0xe0 && ch <= 0xfe && ch != 0xf7) return (uint16_t)(ch - 32);
    if (ch >= 0x430 && ch <= 0x44f) return (uint16_t)(ch - 32);
    if (ch >= 0x450 && ch <= 0x45f) return (uint16_t)(ch - 80);
    return ch;
}
static int put_utf8(char *o, int k, int n, uint16_t ch) {
    if (ch < 0x80) { if (k + 1 < n) o[k++] = (char)ch; }
    else if (ch < 0x800) { if (k + 2 < n) { o[k++] = (char)(0xc0 | (ch >> 6)); o[k++] = (char)(0x80 | (ch & 0x3f)); } }
    else if (k + 3 < n) { o[k++] = (char)(0xe0 | (ch >> 12)); o[k++] = (char)(0x80 | ((ch >> 6) & 0x3f)); o[k++] = (char)(0x80 | (ch & 0x3f)); }
    return k;
}
/* engine UTF-16 text (with ~x~ control codes) -> upper-case UTF-8 */
static void wide_to_menu(uint64_t str, char *o, int n) {
    int k = 0, skip = 0;
    for (int i = 0; str && i < 120; i++) {
        uint16_t ch = *(volatile uint16_t *)(str + 2 * i);
        if (!ch) break;
        if (ch == '~') { skip = !skip; continue; }
        if (!skip) k = put_utf8(o, k, n, upper16(ch));
    }
    o[k < n ? k : n - 1] = 0;
}
static void call(CPU *c, unsigned va) { rt_invoke(c, G + va, 0); }
/* Engine text (GXT) for a key stored in guest memory, flattened to upper-case ASCII. */
static void gxt_label(CPU *c, uint64_t key, char *o, size_t n) {
    o[0] = 0;
    uint64_t obj = rt_invoke(c, G + 0x340ba0, 0);
    uint64_t str = obj ? rt_invoke(c, G + 0x4c3728, 2, obj, key) : 0;
    if (!str || str < 0x100000) { snprintf(o, n, "%s", (const char *)key); return; }
    wide_to_menu(str, o, (int)n);
}
#define ASCREENS (G + 0x81c064)
#define SCREEN_STRIDE 2252
#define ENTRY_STRIDE 20
#define ENTRY_BASE 56
static void build_game_rows(CPU *c) {
    if (game_built) return;
    game_built = 1; game_count = 0;
    int has_back = 0;
    for (int i = 0; i < 10 && game_count < 11; i++) {
        uint64_t e = ASCREENS + 2 * SCREEN_STRIDE + ENTRY_BASE + i * ENTRY_STRIDE;
        if (!M8(e)) break;
        if (!strcmp((const char *)e, "FEM_MM")) { has_back = 1; continue; }
        Row *r = &game_rows[game_count];
        gxt_label(c, e, game_labels[game_count], sizeof game_labels[0]);
        /* entry = {int action; char name[8]; int ?; int target}: action 2 opens `target`, action 26 (load/delete)
         * refreshes the save slots first. Running it directly avoids faking an A press, which the engine resolved
         * against its own (reset) option and which then also confirmed the next page. */
        r->label = game_labels[game_count]; r->kind = K_NATIVE; r->arg = MS32(e + 12) | (MS32(e - 4) == 26 ? 0x100 : 0);
        game_count++;
    }
    (void)has_back;
    game_rows[game_count++] = (Row){"BACK", K_BACK, 0};
    if (rt_verbose) { rt_log("frontend: game page rows:"); for (int i = 0; i < game_count; i++) rt_log(" [%s]", game_rows[i].label); rt_log("\n"); }
}
/* ---- engine settings pages (Audio 5, Display 6, Game settings 68, Language 7) drawn by the host ----------------
 * Rows come from the engine's aScreens table. Values are the engine's own: sliders through
 * GetCurrentLabelValueSlider, every other value text is recorded from the native page draw
 * (CFont::PrintString is captured instead of drawn while it runs, see G_4772fc). Changes go through the
 * engine's ProcessOnOffMenuOptions / ProcessMultiMenuOptions / CheckSliderMovement with the row selected. */
static Row eng_rows[16]; static char eng_labels[16][96]; static int eng_count, eng_screen = -1, eng_compacted;
static int eng_logged = -1;
static char eng_value[16][96]; static float eng_slider[16]; static int eng_hidden[16];
/* slider ranges of the engine: brightness 0..512, radio/SFX volume 0..127 */
static float slider_max(int action) { return action == 11 ? 512.0f : 127.0f; }
static int eng_action(int i) { return MS32(ASCREENS + (uint64_t)eng_screen * SCREEN_STRIDE + ENTRY_BASE + i * ENTRY_STRIDE - 4); }
#define PREFS_LANGUAGE (G + 0x9ec910)
#define PREFS_LANGUAGE_BY_USER (G + 0x9ec988)
static void build_engine_rows(CPU *c) {
    int s = MS32(MENU + SCREEN);
    if (eng_screen == s) return;
    eng_screen = s; eng_count = 0; eng_compacted = 0;
    for (int i = 0; i < 15; i++) {
        uint64_t e = ASCREENS + (uint64_t)s * SCREEN_STRIDE + ENTRY_BASE + i * ENTRY_STRIDE;
        if (!M8(e)) break;
        if (MS32(e - 4) == 2) { /* BACK, in the game language */
            gxt_label(c, e, eng_labels[eng_count], sizeof eng_labels[0]);
            eng_rows[eng_count] = (Row){eng_labels[eng_count], K_BACK, 0}; eng_count++; continue;
        }
        if (MS32(e - 4) == 119) continue; /* SIGN OUT OF SOCIAL CLUB: the session is always offline */
        gxt_label(c, e, eng_labels[eng_count], sizeof eng_labels[0]);
        eng_rows[eng_count] = (Row){eng_labels[eng_count], K_ENGINE, i};
        eng_count++;
    }
}
/* CFont::PrintString capture */
static int font_capture, ncaps;
static struct { float x, y; char s[96]; } caps[96];
extern void G_4772fc_orig(CPU *c);
void G_4772fc(CPU *c) {
    if (!font_capture) { G_4772fc_orig(c); return; }
    if (ncaps < 96) {
        wide_to_menu(c->x[0], caps[ncaps].s, (int)sizeof caps[0].s);
        caps[ncaps].x = c->v[0].f[0]; caps[ncaps].y = c->v[1].f[0];
        ncaps++;
    }
}
static void engine_values(CPU *c) {
    CPU saved = *c;
    for (int r = 0; r < eng_count; r++) {
        eng_value[r][0] = 0; eng_slider[r] = -1; eng_hidden[r] = 0;
        if (eng_rows[r].kind != K_ENGINE) continue;
        int i = eng_rows[r].arg;
        int li = -1; /* the value is printed on the label's line, to the right of it */
        for (int k = 0; k < ncaps; k++) if (!strcmp(caps[k].s, eng_rows[r].label)) { li = k; break; }
        if (li < 0 && ncaps) { eng_hidden[r] = 1; continue; } /* not shown by the game on this device */
        if (eng_screen == 7) { if (MS32(PREFS_LANGUAGE) == eng_action(i) - 18) snprintf(eng_value[r], sizeof eng_value[0], "SELECTED"); continue; }
        rt_invoke(c, G + 0x534560, 2, MENU, (uint64_t)i);
        if (c->v[0].f[0] >= 0) { eng_slider[r] = c->v[0].f[0] / slider_max(MS32(ASCREENS + (uint64_t)eng_screen * SCREEN_STRIDE + ENTRY_BASE + i * ENTRY_STRIDE - 4)); continue; }
        if (li < 0) continue;
        for (int k = 0; k < ncaps; k++)
            if (k != li && fabsf(caps[k].y - caps[li].y) < 6.0f && caps[k].x > caps[li].x && caps[k].s[0]) {
                size_t n = strlen(eng_value[r]);
                snprintf(eng_value[r] + n, sizeof eng_value[0] - n, "%s%s", n ? " " : "", caps[k].s);
            }
    }
    if (!eng_compacted && ncaps) { /* drop rows the game hides on this device (e.g. REFLECTIONS) */
        int w = 0;
        for (int r = 0; r < eng_count; r++) {
            if (eng_hidden[r]) continue;
            if (w != r) {
                memcpy(eng_labels[w], eng_labels[r], sizeof eng_labels[0]); eng_rows[w] = eng_rows[r];
                eng_rows[w].label = eng_labels[w];
                memcpy(eng_value[w], eng_value[r], sizeof eng_value[0]); eng_slider[w] = eng_slider[r];
            }
            w++;
        }
        eng_count = w; eng_compacted = 1;
    }
    if (rt_verbose && eng_logged != eng_screen) {
        eng_logged = eng_screen;
        for (int k = 0; k < ncaps; k++) rt_log("frontend: text %.1f,%.1f [%s]\n", caps[k].x, caps[k].y, caps[k].s);
        for (int r = 0; r < eng_count; r++) rt_log("frontend: row %d [%s] value [%s] slider %.2f\n", r, eng_rows[r].label, eng_value[r], eng_slider[r]);
        rt_log("frontend: language %d\n", MS32(PREFS_LANGUAGE));
    }
    *c = saved;
}
static void engine_adjust(CPU *c, int i, int dir) {
    CPU saved = *c;
    M32(MENU + 0xc) = i;
    rt_invoke(c, G + 0x534560, 2, MENU, (uint64_t)i);
    float slider = c->v[0].f[0];
    int onoff = (int)rt_invoke(c, G + 0x53438c, 2, MENU, (uint64_t)i);
    int action = eng_action(i);
    if (action >= 18 && action <= 24) { /* language rows: CMenuManager::ChangeLanguage(action - 18): 0 English, 1 French, 2 German ... */
        if (dir > 0) { M8(PREFS_LANGUAGE_BY_USER) = 1; rt_invoke(c, G + 0x526fdc, 2, MENU, (uint64_t)(action - 18)); eng_screen = -1; }
    } else if (action == 102) { /* RESTORE DEFAULTS: the engine handles it on its own A press for this row */
        if (dir > 0) { host_native_button(0, 1); native_release = plat.frames + 2; }
    } else if (slider >= 0) { c->v[0].f[0] = (float)dir; rt_invoke(c, G + 0x532654, 1, MENU); }
    else {
        if (onoff >= 0) rt_invoke(c, G + 0x53204c, 1, MENU);
        /* some on/off-looking options (targeting mode) only react to the multi handler */
        if (onoff < 0 || (int)rt_invoke(c, G + 0x53438c, 2, MENU, (uint64_t)i) == onoff) rt_invoke(c, G + 0x5324fc, 2, MENU, (uint64_t)(int64_t)dir);
    }
    eng_logged = -1; /* log the values again after the change */
    if (rt_verbose) { rt_invoke(c, G + 0x534560, 2, MENU, (uint64_t)i); rt_log("frontend: engine row %d dir %d: slider %.2f -> %.2f, onoff %d\n", i, dir, slider, c->v[0].f[0], onoff); }
    *c = saved;
}
/* LCS_DUMP_MENU=1: log every row of the engine's menu table (action, GXT key and text, extra field, target). */
static void dump_screens(CPU *c) {
    static int done;
    if (done || !getenv("LCS_DUMP_MENU")) return;
    done = 1;
    for (int s = 0; s < 72; s++) {
        uint64_t base = ASCREENS + (uint64_t)s * SCREEN_STRIDE;
        char title[64]; gxt_label(c, base, title, sizeof title);
        rt_log("menu screen %d [%.8s] %s\n", s, (const char *)base, title);
        for (int i = 0; i < 30; i++) {
            uint64_t e = base + ENTRY_BASE + i * ENTRY_STRIDE;
            if (!M8(e)) break;
            char t[64]; gxt_label(c, e, t, sizeof t);
            int keep = MS32(MENU + SCREEN); M32(MENU + SCREEN) = s;
            int onoff = (int)rt_invoke(c, G + 0x53438c, 2, MENU, (uint64_t)i);
            rt_invoke(c, G + 0x534560, 2, MENU, (uint64_t)i); float slider = c->v[0].f[0];
            M32(MENU + SCREEN) = keep;
            rt_log("   row %2d action %3d key %-8.8s extra %3d target %3d onoff %d slider %.2f  %s\n", i, MS32(e - 4), (const char *)e, MS32(e + 8), MS32(e + 12), onoff, slider, t);
        }
    }
}
static int root_rows(int ctx, const Row **out) {
    *out = ctx == 52 ? pause_rows : ctx == 69 ? continue_rows : boot_rows;
    return ctx == 52 ? (int)(sizeof pause_rows / sizeof pause_rows[0]) : ctx == 69 ? 4 : 3;
}
/* Row of the root list that opens `kind`/`arg` (SETTINGS, GAME), so Back lands on it. */
static int root_index(int kind, int arg) {
    const Row *r; int n = root_rows(root_ctx, &r);
    for (int i = 0; i < n; i++) if (r[i].kind == kind && r[i].arg == arg) return i;
    return 0;
}
static int cur_rows(const Row **out);
/* Two-pane layout: a view below the root shows its parent list on the left (the row that opened it marked) and its
 * own rows on the right. Returns 0 at the root, where the left list is the current one. */
static int parent_rows(const Row **out, int *sel) {
    int s = MS32(MENU + SCREEN);
    if (s == 41 && (sub == 2 || sub == 3)) { *out = settings_rows; *sel = sub - 2; return 7; }
    if (s == 41 && (sub == 4 || sub == 5)) { *out = controls_rows; *sel = sub - 4; return CONTROLS_COUNT; }
    if (engine_page(s)) { *out = settings_rows; *sel = s == 68 ? 2 : s == 5 ? 3 : s == 6 ? 4 : 5; return 7; }
    if (s == 41) { *sel = root_index(K_PAGE, 41); return root_rows(root_ctx, out); }
    if (s == 2) { *sel = root_index(K_PAGE, 2); return root_rows(root_ctx, out); }
    return 0;
}
static int child_view(void) { const Row *r; int sel; return parent_rows(&r, &sel) > 0; }
/* Rows a highlighted root/parent row would open, for the right pane preview (host lists only). */
static int preview_rows(const Row *x, const Row **out) {
    if (x->kind == K_PAGE && x->arg == 41) { *out = settings_rows; return 7; }
    if (x->kind == K_SUB && x->arg == 2) { *out = graphics_rows; return (int)(sizeof graphics_rows / sizeof graphics_rows[0]); }
    if (x->kind == K_SUB && x->arg == 3) { *out = controls_rows; return CONTROLS_COUNT; }
    if (x->kind == K_SUB && x->arg == 4) { *out = foot_rows; return (int)(sizeof foot_rows / sizeof foot_rows[0]); }
    if (x->kind == K_SUB && x->arg == 5) { *out = car_rows; return (int)(sizeof car_rows / sizeof car_rows[0]); }
    return 0;
}
/* x range of the interactive list in 640x448 menu space */
static float list_x0(void) { return child_view() ? PANEL + 21.0f : 21.0f; }
static float list_x1(void) { return child_view() ? 640.0f - 18.0f : PANEL - 18.0f; }
static int map_row_selected(void) {
    const Row *r; int n = cur_rows(&r);
    return root_ctx == 52 && root_menu() && selection < n && r[selection].kind == K_MAP;
}
static int cur_rows(const Row **out) {
    int s = MS32(MENU + SCREEN);
    if (s == 2) { *out = game_rows; return game_count; }
    if (engine_page(s)) { *out = eng_rows; return eng_count; }
    if (s == 41 && sub == 3) { *out = controls_rows; return CONTROLS_COUNT; }
    if (s == 41 && sub == 4) { *out = foot_rows; return (int)(sizeof foot_rows / sizeof foot_rows[0]); }
    if (s == 41 && sub == 5) { *out = car_rows; return (int)(sizeof car_rows / sizeof car_rows[0]); }
    if (s == 41 && sub == 2) { *out = graphics_rows; return (int)(sizeof graphics_rows / sizeof graphics_rows[0]); }
    if (s == 41) { *out = settings_rows; return 7; }
    return root_rows(s, out);
}
static int rows(void) { const Row *r; return cur_rows(&r); }
static float row_top(void) { return rows() >= 5 ? 168.0f : 190.0f; }
static float row_step(void) { return rows() >= 5 ? 26.0f : 40.0f; }
static int view_key(void) { return MS32(MENU + SCREEN) * 8 + sub; }
static void sync_selection(void) {
    int s = MS32(MENU + SCREEN);
    if (s != 2) game_built = 0;
    if (view_key() != last_screen) { last_screen = view_key(); if (keep_key == last_screen) keep_key = -1; else selection = 0; }
    if (selection >= rows()) selection = 0;
    if (selection < scroll_top) scroll_top = selection;
    if (selection >= scroll_top + VISIBLE_ROWS) scroll_top = selection - VISIBLE_ROWS + 1;
    if (rows() <= VISIBLE_ROWS) scroll_top = 0;
}
static void set_sub(int v, int sel) { sub = v; selection = sel; last_screen = view_key(); }
static void navigate(CPU *c, int target) {
    int old = MS32(MENU + SCREEN);
    if (rt_verbose) rt_log("frontend: navigate %d -> %d (row=%d, native option=%d)\n", old, target, selection, MS32(MENU+0xc));
    M32(MENU + 0xc) = 0; /* native selected menu option, reset on every page change */
    M32(MENU + 0x114) = old;
    M32(MENU + SCREEN) = target;
    M32(MENU + 0x118) = 0;
    M64(MENU + 0x120) = 0;
    uint64_t timer = M64(G + 0x7fa1d0);
    M32(MENU + 0x11c) = M32(timer);
    rt_invoke(c, G + 0x534350, 1, MENU); /* SetMenuScrollPos */
}
extern void host_native_button(int id, int down);
static void native_back(CPU *c) {
    CPU saved = *c;
    end_pause_map(c); /* an active map would swallow the Back action as "close map" */
    /* Queue the original Android Back action: SwitchMenuOnAndOff is a per-frame edge detector. */
    rt_invoke(c, G + 0x56c090, 2, plat.env, 0ull);
    *c = saved;
}
static void go_back(CPU *c) {
    if (capture_btn >= 0) capture_btn = -1;
    else if (capture_act >= 0) capture_act = -1;
    else if (sub >= 4) set_sub(3, sub - 4); /* back to CONTROLS, on the row that opened the page */
    else if (sub) set_sub(0, sub == 2 ? 0 : 1);
    else if (MS32(MENU + SCREEN) == 41) { navigate(c, root_ctx); selection = root_index(K_PAGE, 41); last_screen = -1; keep_key = view_key(); }
    else if (engine_page(MS32(MENU + SCREEN))) { /* back to SETTINGS, on the row that opened this page */
        int from = MS32(MENU + SCREEN);
        { CPU keep = *c; rt_invoke(c, G + 0x527304, 1, MENU); *c = keep; } /* CMenuManager::SaveSettings, as the native Back does */
        navigate(c, 41);
        selection = from == 68 ? 2 : from == 5 ? 3 : from == 6 ? 4 : 5; last_screen = view_key(); keep_key = view_key();
    }
    else if (MS32(MENU + SCREEN) == 2) { /* GAME: straight back to the root list (the native Back was ignored after
                                          * visiting Load/Delete, which left the player stuck on this page) */
        CPU keep = *c; end_pause_map(c); *c = keep;
        navigate(c, root_ctx); selection = root_index(K_PAGE, 2); last_screen = -1; keep_key = view_key();
    }
    ui_sound(SND_BACK);
}
static void activate(CPU *c) {
    CPU saved = *c;
    const Row *r; int n = cur_rows(&r);
    if (selection < 0 || selection >= n) { *c = saved; return; }
    const Row *x = &r[selection];
    if (rt_verbose) rt_log("frontend: activate %s kind=%d arg=%d\n", x->label, x->kind, x->arg);
    switch (x->kind) {
    case K_VALUE: adjust(x->arg, +1); break;
    case K_SUB: set_sub(x->arg, 0); break;
    case K_PAGE: sub = 0; navigate(c, x->arg); break;
    case K_RESUME: native_back(c); break;
    case K_NATIVE:
        if (x->arg & 0x100) rt_invoke(c, G + 0x53e158, 1, G + 0x8443b0); /* C_PcSave::PopulateSlotInfo(&PcSaveHelper) */
        navigate(c, x->arg & 0xff);
        break;
    case K_BACK: go_back(c); break;
    case K_ENGINE: engine_adjust(c, x->arg, 1); break;
    case K_BIND: capture_btn = x->arg; break;
    case K_ACT: capture_act = x->arg; break;
    case K_ACT_CLEAR: host_act_clear(x->arg); break;
    case K_MAP: break;
    case K_RESET: {
        host_bind_reset(); char v[32], k[32];
        for (int i = 0; i < 14; i++) { snprintf(k, sizeof k, "bind_%d", i); snprintf(v, sizeof v, "%d", host_bind_get(i, 0)); tweaks_save(k, v); }
        for (int i = 0; i < 3; i++) { snprintf(k, sizeof k, "mbind_%d", i); snprintf(v, sizeof v, "%d", host_mouse_bind(i)); tweaks_save(k, v); }
        break;
    }
    }
    *c = saved;
}
static void release_native(void) {
    if (native_release && plat.frames >= native_release) { host_native_button(0, 0); native_release = 0; }
}
int frontend_button(CPU *c, int b, int down) {
    if (rt_verbose) rt_log("frontend: button %d %s page=%d sub=%d drawn=%d row=%d\n", b, down?"down":"up", MS32(MENU+SCREEN), sub, root_drawn, selection);
    if (b < 0 || b > 13) return 0;
    unsigned mask = 1u << b;
    if (!down && (consumed_buttons & mask)) { consumed_buttons &= ~mask; return 1; }
    if (!down) return 0; /* release any press that originally reached the game */
    if (!input_root()) return 0;
    if (b != 0 && b != 1 && b != 8 && b != 9 && b != 10 && b != 11) return 0;
    int s = MS32(MENU + SCREEN);
    if (b == 1 && !sub && s != 2 && s != 41 && !engine_page(s)) { end_pause_map(c); return 0; } /* native Back still closes the pause menu */
    consumed_buttons |= mask;
    sync_selection();
    if (s == 2) build_game_rows(c);
    const Row *r; int n = cur_rows(&r);
    if (engine_page(s)) build_engine_rows(c);
    int is_value = selection < n && (r[selection].kind == K_VALUE || r[selection].kind == K_ENGINE);
    if (b == 1) go_back(c);
    else if (is_value && (b == 10 || b == 11)) {
        if (r[selection].kind == K_ENGINE) engine_adjust(c, r[selection].arg, b == 11 ? 1 : -1); else adjust(r[selection].arg, b == 11 ? 1 : -1);
        ui_sound(SND_ADJUST);
    }
    else if (b == 10 && child_view()) go_back(c); /* left: back to the parent list */
    else if (b == 11 && selection < n && (r[selection].kind == K_SUB || (r[selection].kind == K_PAGE && r[selection].arg != 42))) {
        ui_sound(SND_SELECT); activate(c); /* right: open the submenu */
    }
    else if (b == 8 || b == 10) { selection = (selection + n - 1) % n; ui_sound(SND_MOVE); }
    else if (b == 9 || b == 11) { selection = (selection + 1) % n; ui_sound(SND_MOVE); }
    else { ui_sound(SND_SELECT); activate(c); }
    return 1;
}
static void act_text(int arg, char *o, size_t n) {
    if (capture_act == arg) { snprintf(o, n, "PRESS A KEY"); return; }
    int k = host_act_get(arg >> 8, (arg >> 1) & 0x7f, arg & 1);
    host_act_key_name(k, o, n);
    if (!o[0]) snprintf(o, n, "GAMEPAD");
    for (char *p = o; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
}
int frontend_mouse(int button) {
    if (capture_act < 0) return 0;
    host_act_set(capture_act >> 8, (capture_act >> 1) & 0x7f, capture_act & 1, 1000 + button);
    ui_sound(SND_SELECT);
    capture_act = -1;
    return 1;
}
int frontend_key(int scancode) {
    if (capture_act >= 0) {
        if (scancode == SDL_SCANCODE_F11 || scancode == SDL_SCANCODE_F4) return 0;
        if (scancode != SDL_SCANCODE_ESCAPE) { /* Delete clears the binding (back to the gamepad mapping) */
            host_act_set(capture_act >> 8, (capture_act >> 1) & 0x7f, capture_act & 1, scancode == SDL_SCANCODE_DELETE ? 0 : scancode);
            ui_sound(SND_SELECT);
        }
        capture_act = -1;
        return 1;
    }
    if (capture_btn < 0) return 0;
    if (scancode != SDL_SCANCODE_ESCAPE && scancode != SDL_SCANCODE_F11 && scancode != SDL_SCANCODE_F4) {
        char k[32], v[32];
        host_bind_set(capture_btn, scancode);
        for (int i = 0; i < 14; i++) { snprintf(k, sizeof k, "bind_%d", i); snprintf(v, sizeof v, "%d", host_bind_get(i, 0)); tweaks_save(k, v); }
        ui_sound(SND_SELECT);
    } else if (scancode != SDL_SCANCODE_ESCAPE) return 0;
    capture_btn = -1;
    return 1;
}
int frontend_title(void) {
    if (!enabled || !plat.in_ui || plat.loading_visible) return 0;
    uint64_t sl = M64(G + 0x7fd478);
    if (sl && M8(sl)) return 0;
    int s = MS32(MENU + SCREEN);
    return (s == 51 || s == 52 || s == 69) && M8(G + 0x9ec7aa); /* renderedTapToContinue */
}
int frontend_back(CPU *c) {
    if (input_root() && (sub || MS32(MENU + SCREEN) == 2 || MS32(MENU + SCREEN) == 41 || engine_page(MS32(MENU + SCREEN)))) { sync_selection(); go_back(c); return 1; }
    if (map_active && !input_root()) end_pause_map(c);
    else if (map_active && root_ctx == 52) end_pause_map(c);
    return 0;
}
void frontend_hover(CPU *c, float x, float y) {
    if (!input_root() || touch_row != -1) return;
    sync_selection();
    if (MS32(MENU + SCREEN) == 2) build_game_rows(c);
    float dy = y * 448.0f - row_top();
    int row = (int)(dy / row_step()) + scroll_top;
    if (x * 640.0f >= list_x0() && x * 640.0f <= list_x1() && dy >= 0 && row >= scroll_top && row < scroll_top + VISIBLE_ROWS && row < rows() &&
        dy - (row - scroll_top) * row_step() <= row_step() - 4 && row != selection) {
        selection = row; ui_sound(SND_MOVE);
    }
}
int frontend_wheel(CPU *c, float dy) {
    (void)c;
    if (!input_root() || dy == 0) return 0;
    if (map_active && map_row_selected()) {
        uint64_t rm = M64(G + 0xa44270);
        if (rm) { /* zoom is 1..3 (GetZoomPercentage = (zoom - 1) / 2); +0x88 marks the map dirty */
            float z = MF32(rm + 0x74) + (dy > 0 ? 0.25f : -0.25f);
            if (z < 1.0f) z = 1.0f;
            if (z > 3.0f) z = 3.0f;
            MF32(rm + 0x74) = z; M8(rm + 0x88) = 1;
            if (rt_verbose) rt_log("frontend: map zoom %.2f\n", z);
            return 1;
        }
    }
    int n = rows();
    selection = (selection + (dy > 0 ? n - 1 : 1)) % n; ui_sound(SND_MOVE);
    return 1;
}
int frontend_touch(CPU *c, int phase, float x, float y) {
    static int map_touch;
    if (phase == 0 && input_root() && map_active && map_row_selected() && x * 640.0f > PANEL) map_touch = 1;
    if (map_touch) { if (phase == 2) map_touch = 0; return 0; } /* drag on the map pans it (engine touch input) */
    if (!input_root() && touch_row == -1) return 0;
    sync_selection();
    if (MS32(MENU + SCREEN) == 2) build_game_rows(c);
    float dy = y * 448.0f - row_top();
    int row = (int)(dy / row_step()) + scroll_top;
    int hit = input_root() && x * 640.0f >= list_x0() && x * 640.0f <= list_x1() && dy >= 0 && row >= scroll_top && row < scroll_top + VISIBLE_ROWS &&
              row < rows() && dy - (row - scroll_top) * row_step() <= row_step()-4;
    if (phase == 0) { touch_row = hit ? row : -2; if (hit) { if (selection != row) ui_sound(SND_MOVE); selection = row; } }
    else if (phase == 1 && hit && touch_row >= 0) selection = row;
    else if (phase == 2) {
        if (touch_row >= 0 && hit && row == touch_row) { ui_sound(SND_SELECT); activate(c); }
        touch_row = -1;
    }
    return 1;
}
/* CRect layout is {left,bottom,right,top}. Native drawing APIs use 640x448
 * menu space, including the engine's existing aspect-ratio correction. */
static void rect(CPU *c, float x, float y, float w, float h, unsigned rgba) {
    float r[4] = {x, y+h, x+w, y};
    rt_invoke(c, G + 0x4a50a0, 3, (uint64_t)r, (uint64_t)&rgba, 1ull);
}
static void sprite(CPU *c, uint64_t p, float x, float y, float w, float h, unsigned rgba) {
    if (!p) return;
    float r[4] = {x, y+h, x+w, y};
    rt_invoke(c, G + 0x4a3c40, 3, p, (uint64_t)r, (uint64_t)&rgba);
}
/* CSprite2d::DrawRect(rect, c0..c3): c0 bottom-left, c1 bottom-right, c2 top-left, c3 top-right */
static void hgradient(CPU *c, float x, float y, float w, float h, unsigned left, unsigned right) {
    float r[4] = {x, y+h, x+w, y};
    rt_invoke(c, G + 0x4a52f4, 5, (uint64_t)r, (uint64_t)&left, (uint64_t)&right, (uint64_t)&left, (uint64_t)&right);
}
static void drop_color(CPU *c, unsigned rgba) { rt_invoke(c, G + 0x4786cc, 1, (uint64_t)&rgba); }
static void text(CPU *c, const char *s, float x, float y, float scale, unsigned color) {
    uint16_t wide[160]; unsigned n = 0; /* UTF-8 -> UTF-16 */
    const unsigned char *u = (const unsigned char *)s;
    while (*u && n < 159) {
        if (*u < 0x80) wide[n++] = *u++;
        else if ((*u & 0xe0) == 0xc0 && u[1]) { wide[n++] = (uint16_t)(((u[0] & 0x1f) << 6) | (u[1] & 0x3f)); u += 2; }
        else if ((*u & 0xf0) == 0xe0 && u[1] && u[2]) { wide[n++] = (uint16_t)(((u[0] & 0x0f) << 12) | ((u[1] & 0x3f) << 6) | (u[2] & 0x3f)); u += 3; }
        else u++;
    }
    wide[n] = 0;
    rt_invoke(c, G + 0x47551c, 1, (uint64_t)&color);
    c->v[0].f[0] = scale; c->v[1].f[0] = scale * 1.48f;
    call(c, 0x4754e4);
    c->v[0].f[0] = x; c->v[1].f[0] = y;
    rt_invoke(c, G + 0x4772fc, 2, (uint64_t)wide, 0ull);
}
/* The title prompt is a GXT string ("Tap To Continue"). Find its key by asking the engine's own
 * CText::Get for every key-shaped string in the module's rodata, then rewrite the text in place
 * with PC wording (it must not be longer than the original: strings are packed back to back). */
static void patch_title_text(CPU *c) {
    static int done;
    if (done) return;
    done = 1;
    CPU keep = *c;
    uint64_t text_obj = rt_invoke(c, G + 0x340ba0, 0);
    static const char want[] = "Tap To Continue";
    unsigned tried = 0;
    for (uint64_t p = G + 0x1c0000; p < G + 0x260000 && text_obj; p++) {
        if (M8(p - 1) != 0) continue;
        unsigned n = 0;
        while (n < 12 && ((M8(p + n) >= 'A' && M8(p + n) <= 'Z') || (M8(p + n) >= '0' && M8(p + n) <= '9') || M8(p + n) == '_')) n++;
        if (n < 3 || M8(p + n) != 0) continue;
        tried++;
        uint64_t str = rt_invoke(c, G + 0x4c3728, 2, text_obj, p);
        if (!str || str < 0x100000) continue;
        int match = 1;
        for (unsigned i = 0; i < sizeof want; i++) {
            uint16_t ch = *(volatile uint16_t *)(str + 2 * i);
            if (ch != (unsigned char)want[i]) { match = 0; break; }
        }
        if (!match) continue;
        static const char pc[] = "Press Any Key";
        for (unsigned i = 0; i < sizeof pc; i++) *(volatile uint16_t *)(str + 2 * i) = (unsigned char)pc[i];
        if (rt_verbose) rt_log("frontend: title prompt rewritten (key %s, %u keys tried)\n", (const char *)p, tried);
        break;
    }
    if (rt_verbose && tried) rt_log("frontend: title prompt scan done, %u keys\n", tried);
    *c = keep;
}
/* Public guest entry is supplied by this host wrapper; the lifter preserves
 * the original implementation for every screen outside the redesigned root. */
/* One menu list. x0 = 0 for the left pane, PANEL for the right pane; `live` lists take input (values, captures,
 * scroll bar), the other pane's list is shown dimmed. sel = -1 marks no row. */
static void draw_list(CPU *c, const Row *items, int count, int sel, int top, float x0, int live) {
    float lx = x0 + 21.0f, width = (x0 > 0 ? 640.0f - PANEL : PANEL) - 39.0f, vx = x0 + VALUE_X;
    if (x0 > 0) rect(c, PANEL + 2, 0, 640 - PANEL - 2, 448, 0xff161616); /* the right pane: a plain panel under the list */
    int last = top + VISIBLE_ROWS < count ? top + VISIBLE_ROWS : count;
    for (int i = top; i < last; i++) {
        float y = row_top() + (i - top) * row_step(), h = row_step() - 5;
        if (i == sel) {
            /* native-style highlight: a white band that fades out to the right, with dark red text */
            float solid = width * 0.55f;
            unsigned band = live ? 0xffffffff : 0xff7a7a7a;
            rect(c, lx, y, solid, h, band);
            hgradient(c, lx + solid, y, width - solid, h, band, band & 0x00ffffff);
            rect(c, lx, y, 3, h, 0xff000099);
        }
        unsigned col = i == sel ? 0xff2424a7 : live ? 0xffffffff : 0xff9a9a9a;
        float ts = count > 4 ? .60f : .77f, ty = y + (count > 4 ? 4 : 8);
        if (i == sel) drop_color(c, 0x00000000); /* native highlight text has no shadow */
        text(c, items[i].label, lx + 12, ty, ts, col);
        if (i == sel) drop_color(c, 0xff000000);
        if (!live && x0 == 0) continue; /* parent list on the left: labels only */
        char val[48] = "", shown[64];
        int is_sel = live && i == sel;
        if (items[i].kind == K_VALUE) { value_text(items[i].arg, val, sizeof val); snprintf(shown, sizeof shown, is_sel ? "< %s >" : "%s", val); }
        else if (live && items[i].kind == K_ENGINE && eng_slider[i] >= 0) { /* engine slider: a bar in the value column */
            float f = eng_slider[i] > 1 ? 1 : eng_slider[i];
            rect(c, vx, y + 7, 80, 7, 0xff3a3a3a); rect(c, vx, y + 7, 80 * f, 7, is_sel ? 0xff000099 : 0xffd2d2d2);
            shown[0] = 0;
        }
        else if (live && items[i].kind == K_ENGINE && eng_value[i][0]) snprintf(shown, sizeof shown, is_sel ? "< %s >" : "%s", eng_value[i]);
        else if (items[i].kind == K_BIND) { bind_text(items[i].arg, val, sizeof val); snprintf(shown, sizeof shown, "%s", val); }
        else if (items[i].kind == K_ACT) { act_text(items[i].arg, val, sizeof val); snprintf(shown, sizeof shown, "%s", val); }
        else shown[0] = 0;
        int capturing = live && ((items[i].kind == K_BIND && capture_btn == items[i].arg) || (items[i].kind == K_ACT && capture_act == items[i].arg));
        if (shown[0]) text(c, shown, vx, ty, .46f, (is_sel || capturing) ? 0xff2424a7 : live ? 0xffd2d2d2 : 0xff8a8a8a);
    }
    if (live && count > VISIBLE_ROWS) { /* scroll bar */
        float st = row_top(), span = VISIBLE_ROWS * row_step() - 5, bx = x0 + width + 25.0f;
        rect(c, bx, st, 3, span, 0xff2e2e2e);
        rect(c, bx, st + span * top / count, 3, span * VISIBLE_ROWS / count, 0xff000099);
    }
}
void G_527bf4(CPU *c) {
    /* host_native_button runs guest code on this CPU: keep x0 (the menu) and the rest for the original draw below */
    CPU entry = *c;
    release_native(); /* must also run on native leaf pages, or A stays held and the page ignores input */
    play_pending_sound(c);
    *c = entry;
    if (!host_active()) {
        game_built = 0;
        end_pause_map(c);
        G_527bf4_orig(c);
        if (frontend_title()) patch_title_text(c);
        return;
    }
    CPU saved = *c;
    /* The native draw also advances frontend fades, retires the splash and
     * initialises text/UI state. Keep those side effects before repainting
     * the layout; native submenus depend on them. */
    font_capture = engine_page(MS32(MENU + SCREEN)); ncaps = 0; /* engine pages: record their text, draw it ourselves */
    G_527bf4_orig(c);
    font_capture = 0;
    *c = saved;
    root_drawn = 1;
    if (!first_menu_frame) first_menu_frame = plat.frames;
    last_root_frame = plat.frames;
    int screen = MS32(MENU + SCREEN);
    if (resume_sub && root_menu()) { set_sub(resume_sub, 0); resume_sub = 0; }
    sync_selection();
    if (screen == 2) build_game_rows(c);
    if (engine_page(screen)) { build_engine_rows(c); engine_values(c); }
    else eng_screen = -1;
    dump_screens(c);
    static int logged_page = -1;
    if (rt_verbose && logged_page != screen) { logged_page = screen; rt_log("frontend: draw page=%d frame=%u native option=%d\n", logged_page, plat.frames, MS32(MENU+0xc)); }
    call(c, 0x5339ec); /* original frontend render states */
    call(c, 0x4a2ea0); /* initialise sprite buffer */
    rect(c, 0, 0, 640, 448, 0xff1e1e1e); /* LCS menu grey */
    /* Original LCS artwork fills the right side. */
    uint64_t m = M64(G + 0x7f92c0);
    sprite(c, m ? M64(m + 0x178) : 0, PANEL - 17, 0, 640 - PANEL + 17, 448, 0xffffffff);
    float artwork_h = (406.0f/640.0f) * plat.width / plat.height * 448.0f / 2.14f;
    if (artwork_h > 448) artwork_h = 448;
    int show_map = map_row_selected();
    if (!show_map) sprite(c, M64(MENU + 0x140), PANEL + 10, (448-artwork_h)*.5f, 350, artwork_h * 350.0f / 406.0f, 0xffffffff);
    /* In the pause menu the map mode stays on (it also replaces the native page's gamepad icons with the map's,
     * which strip_capture hides); the map itself is drawn only while the MAP row is highlighted. */
    if (root_ctx == 52 && draw_pause_map(c, show_map)) {
        strip_capture(); /* the map screen's gamepad hints (queued text and icons) land on the bottom edge: keep what is under them */
        call(c, 0x476f1c); /* the map's legend text is queued: flush it now so the panel can cover it */
    }
    rect(c, 0, 0, PANEL, 448, 0xff171717);
    rect(c, PANEL, 0, 2, 448, 0xff000099); /* LCS accent red */
    rt_invoke(c, G + 0x47563c, 1, 3ull);
    call(c, 0x4755dc); call(c, 0x475628); call(c, 0x475598);
    call(c, 0x475584); call(c, 0x4786a8); call(c, 0x47867c);
    c->v[0].f[0] = 620; call(c, 0x4755a8);
    {   /* menu space is 640x448; the logo was drawn for a 4:3 screen, so undo this window's extra stretch */
        float k = ((float)plat.width / plat.height) / (640.0f / 448.0f), k0 = (4.0f / 3.0f) / (640.0f / 448.0f);
        float lh = 112.0f, lw = lh * (184.0f / 133.0f) * k0 / k;
        sprite(c, M64(MENU + 0x180), (21.0f + (PANEL - 18.0f)) * 0.5f - lw * 0.5f, 22, lw, lh, 0xffffffff); /* centred over the rows */
    }
    const Row *items; int count = cur_rows(&items);
    int has_value = selection < count && (items[selection].kind == K_VALUE || items[selection].kind == K_ENGINE);
    int is_bind = selection < count && (items[selection].kind == K_BIND || items[selection].kind == K_ACT);
    const Row *parent; int parent_sel = 0, parent_count = parent_rows(&parent, &parent_sel);
    if (parent_count) {
        /* left: the parent list with the row that opened this view; right: this view's rows and values */
        draw_list(c, parent, parent_count, parent_sel, 0, 0.0f, 0);
        draw_list(c, items, count, selection, scroll_top, PANEL, 1);
    } else {
        draw_list(c, items, count, selection, scroll_top, 0.0f, 1);
        const Row *pre; int pn = selection < count ? preview_rows(&items[selection], &pre) : 0;
        if (pn) draw_list(c, pre, pn, -1, 0, PANEL, 0); /* what the highlighted row opens */
    }
    int can_back = sub || screen == 2 || screen == 41 || engine_page(screen);
    float fy = 426.0f - 15.0f * (can_back ? 2 : 1);
    int pad = plat.pad_input; /* hints follow the device that was used last */
    if (capture_btn >= 0) { text(c, "PRESS A KEY   ESC CANCEL", 26, fy, .40f, 0xffbfbfbf); fy += 15; }
    else if (capture_act >= 0) { text(c, "PRESS A KEY OR MOUSE BUTTON   DEL CLEAR   ESC CANCEL", 26, fy, .40f, 0xffbfbfbf); fy += 15; }
    else if (has_value) { text(c, pad ? "D-PAD LEFT / RIGHT   CHANGE" : "LEFT / RIGHT   CHANGE", 26, fy, .40f, 0xffbfbfbf); fy += 15; }
    else if (is_bind) { text(c, pad ? "A   REBIND KEY" : "ENTER   REBIND KEY", 26, fy, .40f, 0xffbfbfbf); fy += 15; }
    else if (show_map) { text(c, pad ? "RIGHT STICK   MOVE MAP" : "WHEEL ZOOM   DRAG MOVE", 26, fy, .40f, 0xffbfbfbf); fy += 15; }
    else { text(c, pad ? "D-PAD   SELECT" : "UP / DOWN   SELECT", 26, fy, .40f, 0xffbfbfbf); fy += 15; }
    text(c, pad ? "A   CONFIRM" : "ENTER   CONFIRM", 26, fy, .40f, 0xffbfbfbf); fy += 15;
    if (can_back) text(c, pad ? "B   BACK" : "ESC   BACK", 26, fy, .40f, 0xffbfbfbf);
    call(c, 0x476f1c); /* draw/flush text before stack-backed strings go away */
    *c = saved;
}


/* Keep the actual last game camera frame on the pause menu's right side.
 * This runs at the presentation boundary, after the game's final compositing.
 * The GPU copy avoids readback and remains frozen while the engine is paused. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengles2.h>
#define GLFN(ret, name, args) static ret (GL_APIENTRY *q##name) args
GLFN(void, GetIntegerv, (GLenum, GLint*));
GLFN(void, GetBooleanv, (GLenum, GLboolean*));
GLFN(GLboolean, IsEnabled, (GLenum));
GLFN(void, Enable, (GLenum)); GLFN(void, Disable, (GLenum));
GLFN(void, ActiveTexture, (GLenum)); GLFN(void, BindTexture, (GLenum, GLuint));
GLFN(void, GenTextures, (GLsizei, GLuint*));
GLFN(void, TexParameteri, (GLenum, GLenum, GLint));
GLFN(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*));
GLFN(void, CopyTexSubImage2D, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei));
GLFN(GLuint, CreateShader, (GLenum));
GLFN(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*));
GLFN(void, CompileShader, (GLuint)); GLFN(void, GetShaderiv, (GLuint, GLenum, GLint*));
GLFN(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*));
GLFN(void, DeleteShader, (GLuint)); GLFN(GLuint, CreateProgram, (void));
GLFN(void, AttachShader, (GLuint, GLuint)); GLFN(void, BindAttribLocation, (GLuint, GLuint, const GLchar*));
GLFN(void, LinkProgram, (GLuint)); GLFN(void, GetProgramiv, (GLuint, GLenum, GLint*));
GLFN(void, DeleteProgram, (GLuint)); GLFN(void, UseProgram, (GLuint));
GLFN(GLint, GetUniformLocation, (GLuint, const GLchar*)); GLFN(void, Uniform1i, (GLint, GLint));
GLFN(void, GenBuffers, (GLsizei, GLuint*)); GLFN(void, BindBuffer, (GLenum, GLuint));
GLFN(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum));
GLFN(void, GetVertexAttribiv, (GLuint, GLenum, GLint*));
GLFN(void, GetVertexAttribPointerv, (GLuint, GLenum, void**));
GLFN(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*));
GLFN(void, EnableVertexAttribArray, (GLuint)); GLFN(void, DisableVertexAttribArray, (GLuint));
GLFN(void, DrawArrays, (GLenum, GLint, GLsizei));
GLFN(void, Viewport, (GLint, GLint, GLsizei, GLsizei));
GLFN(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean));
GLFN(void, Scissor, (GLint, GLint, GLsizei, GLsizei)); GLFN(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)); GLFN(void, Clear, (GLbitfield));
static GLuint camera_texture, camera_program, camera_buffer;
static int camera_width, camera_height, camera_ready, gl_available = -1;
static GLuint camera_shader(GLenum type, const char *source) {
    GLuint shader = qCreateShader(type); GLint ok;
    qShaderSource(shader, 1, &source, 0); qCompileShader(shader);
    qGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; qGetShaderInfoLog(shader, sizeof log, 0, log);
        rt_log("frontend: camera shader failed: %s\n", log);
        qDeleteShader(shader); return 0;
    }
    return shader;
}
static int camera_init(void) {
    if (gl_available >= 0) return gl_available;
    gl_available = 0;
#define LOAD(name) if (!(q##name = (void*)SDL_GL_GetProcAddress("gl" #name))) return 0
    LOAD(GetIntegerv); LOAD(GetBooleanv); LOAD(IsEnabled); LOAD(Enable); LOAD(Disable);
    LOAD(ActiveTexture); LOAD(BindTexture); LOAD(GenTextures); LOAD(TexParameteri);
    LOAD(TexImage2D); LOAD(CopyTexSubImage2D); LOAD(CreateShader); LOAD(ShaderSource);
    LOAD(CompileShader); LOAD(GetShaderiv); LOAD(GetShaderInfoLog); LOAD(DeleteShader);
    LOAD(CreateProgram); LOAD(AttachShader); LOAD(BindAttribLocation); LOAD(LinkProgram);
    LOAD(GetProgramiv); LOAD(DeleteProgram); LOAD(UseProgram); LOAD(GetUniformLocation);
    LOAD(Uniform1i); LOAD(GenBuffers); LOAD(BindBuffer); LOAD(BufferData);
    LOAD(GetVertexAttribiv); LOAD(GetVertexAttribPointerv); LOAD(VertexAttribPointer);
    LOAD(EnableVertexAttribArray); LOAD(DisableVertexAttribArray); LOAD(DrawArrays);
    LOAD(Viewport); LOAD(ColorMask); LOAD(Scissor); LOAD(ClearColor); LOAD(Clear);
#undef LOAD
    GLuint vs = camera_shader(GL_VERTEX_SHADER,
        "attribute vec4 a; varying vec2 uv; void main(){gl_Position=vec4(a.xy,0.0,1.0);uv=a.zw;}");
    GLuint fs = camera_shader(GL_FRAGMENT_SHADER,
        "precision mediump float; varying vec2 uv; uniform sampler2D camera; void main(){gl_FragColor=texture2D(camera,uv);}");
    if (!vs || !fs) { if (vs) qDeleteShader(vs); if (fs) qDeleteShader(fs); return 0; }
    camera_program = qCreateProgram(); qAttachShader(camera_program, vs); qAttachShader(camera_program, fs);
    qBindAttribLocation(camera_program, 0, "a"); qLinkProgram(camera_program);
    qDeleteShader(vs); qDeleteShader(fs);
    GLint ok; qGetProgramiv(camera_program, GL_LINK_STATUS, &ok);
    if (!ok) { qDeleteProgram(camera_program); camera_program = 0; return 0; }
    qGenTextures(1, &camera_texture); qGenBuffers(1, &camera_buffer);
    gl_available = 1;
    return 1;
}
/* Bottom edge of the right pane: the engine draws its gamepad hint icons there, unshifted, after the menu.
 * The pixels under them are copied to a texture when the map is drawn and drawn back before presenting. */
#define STRIP_X 0.47f
#define STRIP_H 0.07f
static GLuint strip_texture;
static int strip_w, strip_h, strip_ready;
static void strip_capture(void) {
    if (!camera_init()) return;
    int x0 = (int)(plat.width * STRIP_X), w = plat.width - x0, h = (int)(plat.height * STRIP_H);
    if (w < 1 || h < 1) return;
    GLint active, texture;
    qGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    qActiveTexture(GL_TEXTURE0); qGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    if (!strip_texture) qGenTextures(1, &strip_texture);
    qBindTexture(GL_TEXTURE_2D, strip_texture);
    if (w != strip_w || h != strip_h) {
        strip_w = w; strip_h = h;
        qTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        qTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        qTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        qTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        qTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    qCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, x0, 0, w, h);
    strip_ready = 1;
    qBindTexture(GL_TEXTURE_2D, texture); qActiveTexture(active);
}
static void strip_restore(void) {
    if (!strip_ready || !camera_init()) return;
    GLint active, texture, program, array_buffer, viewport[4]; GLboolean mask[4];
    GLint attr_enabled, attr_size, attr_type, attr_normalized, attr_stride, attr_buffer;
    void *attr_pointer;
    qGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    qActiveTexture(GL_TEXTURE0); qGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    qGetIntegerv(GL_CURRENT_PROGRAM, &program); qGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
    qGetIntegerv(GL_VIEWPORT, viewport); qGetBooleanv(GL_COLOR_WRITEMASK, mask);
    qGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attr_enabled);
    qGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attr_size);
    qGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attr_type);
    qGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &attr_normalized);
    qGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attr_stride);
    qGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attr_buffer);
    qGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &attr_pointer);
    GLenum caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST, GL_CULL_FACE};
    GLboolean states[5];
    for (int i = 0; i < 5; i++) { states[i] = qIsEnabled(caps[i]); qDisable(caps[i]); }
    qColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    qViewport(0, 0, plat.width, plat.height);
    float left = STRIP_X * 2.0f - 1.0f, top = -1.0f + 2.0f * strip_h / plat.height;
    float vertices[] = {left,-1,0,0, 1,-1,1,0, left,top,0,1, 1,top,1,1};
    qBindTexture(GL_TEXTURE_2D, strip_texture);
    qUseProgram(camera_program); qUniform1i(qGetUniformLocation(camera_program, "camera"), 0);
    qBindBuffer(GL_ARRAY_BUFFER, camera_buffer); qBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STREAM_DRAW);
    qVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4*sizeof(float), 0); qEnableVertexAttribArray(0);
    qDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    qBindBuffer(GL_ARRAY_BUFFER, attr_buffer);
    qVertexAttribPointer(0, attr_size, attr_type, (GLboolean)attr_normalized, attr_stride, attr_pointer);
    if (!attr_enabled) qDisableVertexAttribArray(0);
    qBindBuffer(GL_ARRAY_BUFFER, array_buffer); qUseProgram(program);
    qViewport(viewport[0], viewport[1], viewport[2], viewport[3]); qColorMask(mask[0], mask[1], mask[2], mask[3]);
    for (int i = 0; i < 5; i++) if (states[i]) qEnable(caps[i]);
    qBindTexture(GL_TEXTURE_2D, texture); qActiveTexture(active);
}
void frontend_present(void) {
    /* The pause pane shows only the live map: no copy of the last game frame (it flashed as a frozen
    * snapshot before the map appeared and cost a full-screen copy every gameplay frame). */
    if (host_active() && root_ctx == 52 && map_active) strip_restore();
}
/* The pause menu's right pane shows the engine's own map (CRadar::DrawRadarMap + RadarMap::Render).
 * They draw for the full screen, so the GL viewport and every scissor the engine sets are shifted
 * right by rt_gl_dx; the opaque left panel is drawn afterwards and hides the part that spills over. */
extern int rt_gl_dx;
static int map_active;
static int draw_pause_map(CPU *c, int render) {
    uint64_t rm = M64(G + 0xa44270);
    if (!rm || !camera_init()) return 0;
    CPU keep = *c;
    if (!map_active) { rt_invoke(c, G + 0x54e59c, 1, rm); map_active = 1; if (rt_verbose) rt_log("frontend: pause map activated\n"); }
    if (!render) { *c = keep; return 1; }
    int dx = (int)(plat.width * MAP_SHIFT);
    rt_gl_dx = dx;
    qViewport(dx, 0, plat.width, plat.height);
    call(c, 0x35810c);
    int mode = getenv("LCS_MAPMODE") ? atoi(getenv("LCS_MAPMODE")) : 1;
    if (mode == 1) rt_invoke(c, G + 0x54f3ec, 1, rm);
    rt_gl_dx = 0;
    qViewport(0, 0, plat.width, plat.height);
    *c = keep;
    return 1;
}
static void end_pause_map(CPU *c) {
    if (!map_active) return;
    uint64_t rm = M64(G + 0xa44270);
    CPU keep = *c;
    if (rm) rt_invoke(c, G + 0x54e934, 1, rm);
    map_active = 0;
    *c = keep;
}
