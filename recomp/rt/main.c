/* Windows entry point.  Plays the role of GameActivity/GameView/GameGlThread:
 * it calls the game's own exported JNI entry points in the order the Java
 * launcher does, then drives viewOnDrawFrame.  All game behaviour comes from
 * the statically recompiled libGame.so. */
#include "rt.h"
#include "platform.h"
#include "tweaks.h"
#include "frontend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <windows.h>
#include <shlobj.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

#define GTA "Java_com_rockstargames_gtalcs_GTAJNIlib_"
#define RS "Java_com_rockstargames_gtalcs_RockstarJNIlib_"

static CPU *cpu;
static uint64_t jcls;
static char root[MAX_PATH];
/* Installed layout (setup.exe): <install>/lcs.exe + <install>/data/{lib,assets}; saves, lcs.ini and the log
 * live in Documents/Rockstar Games/GTA Liberty City Stories. Otherwise the development tree is used. */
static int installed;
static char data_dir[MAX_PATH], user_dir[MAX_PATH];
static void slashes(char *s) { for (; *s; s++) if (*s == '\\') *s = '/'; }
static void find_layout(void) {
    char exe[MAX_PATH], probe[MAX_PATH];
    GetModuleFileNameA(0, exe, sizeof exe);
    char *p = strrchr(exe, '\\');
    if (p) *p = 0;
    snprintf(probe, sizeof probe, "%s\\data\\lib\\libGame.so", exe);
    DWORD a = GetFileAttributesA(probe);
    installed = a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    snprintf(root, sizeof root, "%s", exe);
    if (installed) {
        char docs[MAX_PATH] = "";
        if (FAILED(SHGetFolderPathA(0, CSIDL_PERSONAL | CSIDL_FLAG_CREATE, 0, 0, docs))) snprintf(docs, sizeof docs, "%s", exe);
        snprintf(data_dir, sizeof data_dir, "%s/data", exe);
        snprintf(user_dir, sizeof user_dir, "%s/Rockstar Games", docs);
        _mkdir(user_dir);
        snprintf(user_dir, sizeof user_dir, "%s/Rockstar Games/GTA Liberty City Stories", docs);
        _mkdir(user_dir);
    } else {
        char *q = strrchr(root, '\\'); /* build/lcs.exe -> project tree */
        if (q) *q = 0;
    }
    slashes(root); slashes(data_dir); slashes(user_dir);
}

static uint64_t gta(const char *name, int nargs, uint64_t a, uint64_t b, uint64_t c_, uint64_t d) {
    char sym[160];
    snprintf(sym, sizeof sym, GTA "%s", name);
    if (rt_verbose > 1) rt_log("host -> %s\n", name);
    return rt_invoke(cpu, rt_need(sym), 2 + nargs, plat.env, jcls, a, b, c_, d);
}

/* ------------------------------------------------------------ screenshots (diagnostics) */

static const char *shot_path;
static unsigned shot_every;

static void save_screenshot(unsigned frame) {
    typedef void (__stdcall *ReadPixels)(int, int, int, int, unsigned, unsigned, void *);
    ReadPixels rp = (ReadPixels)SDL_GL_GetProcAddress("glReadPixels");
    /* what the player sees: the window, after present_frame() scaled the scene into it */
    typedef void (__stdcall *BindFb)(unsigned, unsigned);
    BindFb bind = (BindFb)SDL_GL_GetProcAddress("glBindFramebuffer");
    extern unsigned rt_gl_default_fb;
    int w = plat.width, h = plat.height;
    if (plat.window) SDL_GetWindowSizeInPixels(plat.window, &w, &h);
    uint8_t *px = malloc((size_t)w * h * 4);
    if (bind) bind(0x8D40, 0);
    rp(0, 0, w, h, 0x1908, 0x1401, px);
    if (bind) bind(0x8D40, rt_gl_default_fb);
    char path[MAX_PATH];
    snprintf(path, sizeof path, shot_path, frame);
    FILE *f = fopen(path, "wb");
    if (f) {
        uint8_t hdr[54] = {'B', 'M'};
        uint32_t size = 54 + (uint32_t)w * h * 3;
        memcpy(hdr + 2, &size, 4);
        hdr[10] = 54; hdr[14] = 40;
        memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
        hdr[26] = 1; hdr[28] = 24;
        fwrite(hdr, 1, 54, f);
        size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3;
        uint8_t *row = calloc(1, stride);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                row[x * 3] = px[(y * w + x) * 4 + 2];
                row[x * 3 + 1] = px[(y * w + x) * 4 + 1];
                row[x * 3 + 2] = px[(y * w + x) * 4];
            }
            fwrite(row, 1, stride, f);
        }
        free(row);
        fclose(f);
        rt_log("host: wrote %s\n", path);
    }
    free(px);
}

void platform_present(void) {
    frontend_present();
    present_frame(); /* scene framebuffer -> window, letterboxed */
    if (shot_path && shot_every && plat.frames % shot_every == 0) save_screenshot(plat.frames);
    if (plat.window) SDL_GL_SwapWindow(plat.window);
}

/* ------------------------------------------------------------ input
 * Button ids are the ones GameView.toJoypadButton produces (OSX360_*):
 * A 0, B 1, X 2, Y 3, START 4, BACK 5, L1 6, R1 7, DPAD up 8 / down 9 / left 10 / right 11,
 * L3 12, R3 13.  Axes: 0/1 left stick, 2/3 right stick, 4 L2, 5 R2. */

enum { B_A, B_B, B_X, B_Y, B_START, B_BACK, B_L1, B_R1, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_L3, B_R3, B_COUNT };

/* Per-action bindings, separate on foot and in a vehicle. The Rust CPad ports (recomp/engine/src/pad.rs) ask
 * host_action() inside each action getter; an unbound action keeps the gamepad button the game expects.
 * lcs.ini: foot_<action>=<key> / car_<action>=<key>, keys by SDL name ("Left Shift", "W") or Mouse1..Mouse3;
 * move_x / move_y take two keys, "A,D". Must match `enum Act` in pad.rs. */
enum { ACT_ACCELERATE, ACT_BRAKE, ACT_HANDBRAKE, ACT_ENTER_EXIT, ACT_SPRINT, ACT_JUMP, ACT_ATTACK, ACT_NEXT_WEAPON,
       ACT_PREV_WEAPON, ACT_PICKUP, ACT_LOOK_BEHIND, ACT_MOVE_X, ACT_MOVE_Y, ACT_COUNT };
static const char *act_names[ACT_COUNT] = {"accelerate", "brake", "handbrake", "enter_exit", "sprint", "jump", "attack",
                                           "next_weapon", "prev_weapon", "pickup", "look_behind", "move_x", "move_y"};
static int act_key[2][ACT_COUNT][2];  /* [vehicle][action]: key (axis: negative key), axis positive key */
static unsigned char act_now[2][ACT_COUNT], act_prev[2][ACT_COUNT];
static signed char act_axis[2][ACT_COUNT];
static int act_any;
#define ACT_MOUSE 1000 /* + SDL mouse button */

static int act_parse_key(const char *s) {
    while (*s == ' ') s++;
    if (!SDL_strncasecmp(s, "mouse", 5)) return ACT_MOUSE + atoi(s + 5);
    return (int)SDL_GetScancodeFromName(s);
}
/* "foot_sprint" / "car_accelerate" = key; returns 1 when the key was an action binding */
int host_action_ini(const char *key, const char *val) {
    int veh = !strncmp(key, "car_", 4);
    if (!veh && strncmp(key, "foot_", 5)) return 0;
    const char *name = key + (veh ? 4 : 5);
    for (int a = 0; a < ACT_COUNT; a++) {
        if (strcmp(name, act_names[a])) continue;
        char buf[128]; snprintf(buf, sizeof buf, "%s", val);
        char *comma = strchr(buf, ',');
        if (comma) *comma = 0;
        act_key[veh][a][0] = act_parse_key(buf);
        act_key[veh][a][1] = comma ? act_parse_key(comma + 1) : 0;
        act_any |= act_key[veh][a][0] || act_key[veh][a][1];
        return 1;
    }
    return 1;
}
static int act_held(const bool *ks, int k) {
    if (k <= 0) return 0;
    if (k >= ACT_MOUSE) return (SDL_GetMouseState(0, 0) & SDL_BUTTON_MASK(k - ACT_MOUSE)) != 0;
    return k < SDL_SCANCODE_COUNT && ks[k];
}
static void actions_frame(const bool *ks) {
    if (!act_any) return;
    for (int v = 0; v < 2; v++)
        for (int a = 0; a < ACT_COUNT; a++) {
            act_prev[v][a] = act_now[v][a];
            int neg = act_held(ks, act_key[v][a][0]), pos = act_held(ks, act_key[v][a][1]);
            act_now[v][a] = (unsigned char)neg;
            act_axis[v][a] = (signed char)((pos - neg) * 127);
        }
}
int host_actions_bound(void) { return act_any; }

/* Menu access (rt/frontend.c, ON FOOT KEYS / VEHICLE KEYS): slot 1 is the positive key of move_x / move_y. */
int host_act_get(int veh, int a, int slot) { return a >= 0 && a < ACT_COUNT && slot >= 0 && slot < 2 ? act_key[veh != 0][a][slot] : 0; }
void host_act_key_name(int k, char *o, size_t n) {
    if (k >= ACT_MOUSE) snprintf(o, n, "Mouse%d", k - ACT_MOUSE);
    else snprintf(o, n, "%s", k > 0 ? SDL_GetScancodeName((SDL_Scancode)k) : "");
}
static void act_save(int veh, int a) {
    char key[48], v1[48], v2[48], val[100];
    snprintf(key, sizeof key, "%s_%s", veh ? "car" : "foot", act_names[a]);
    host_act_key_name(act_key[veh][a][0], v1, sizeof v1);
    host_act_key_name(act_key[veh][a][1], v2, sizeof v2);
    if (act_key[veh][a][1]) snprintf(val, sizeof val, "%s,%s", v1, v2); else snprintf(val, sizeof val, "%s", v1);
    tweaks_save(key, val);
}
static void act_recount(void) {
    act_any = 0;
    for (int v = 0; v < 2; v++) for (int a = 0; a < ACT_COUNT; a++) act_any |= act_key[v][a][0] || act_key[v][a][1];
}
/* k = 0 clears; the same key is removed from the other actions of this context */
void host_act_set(int veh, int a, int slot, int k) {
    veh = veh != 0;
    if (a < 0 || a >= ACT_COUNT || slot < 0 || slot > 1) return;
    if (k)
        for (int b = 0; b < ACT_COUNT; b++)
            for (int s = 0; s < 2; s++)
                if (act_key[veh][b][s] == k && !(b == a && s == slot)) { act_key[veh][b][s] = 0; act_save(veh, b); }
    act_key[veh][a][slot] = k;
    act_save(veh, a);
    act_recount();
}
void host_act_clear(int veh) {
    for (int a = 0; a < ACT_COUNT; a++) { act_key[veh != 0][a][0] = act_key[veh != 0][a][1] = 0; act_save(veh != 0, a); }
    act_recount();
}
int host_action(int a, int vehicle) {
    if (a < 0 || a >= ACT_COUNT || !act_key[vehicle != 0][a][0]) return -1;
    return act_now[vehicle != 0][a] | act_prev[vehicle != 0][a] << 1;
}
int host_action_axis(int a, int vehicle) {
    if (a < 0 || a >= ACT_COUNT || (!act_key[vehicle != 0][a][0] && !act_key[vehicle != 0][a][1])) return INT32_MIN;
    return act_axis[vehicle != 0][a] * 128 / 127;
}

static int held[B_COUNT];       /* number of host inputs currently holding each guest button */
static float axis_sent[6];
static int gamepads;
static float mouse_dx, mouse_dy;
static int touch_down;
static float script_axis[6];    /* --axis overrides, added to real input */

static void touch(const char *fn, float x, float y);
static int title_touch;
static void button(int id, int down) {
    /* PC: any button dismisses the title prompt (the engine only listens for a tap) */
    if (down && frontend_title()) { title_touch = 1; touch("onTouchStart", plat.width * 0.5f, plat.height * 0.75f); return; }
    if (!down && title_touch) { title_touch = 0; touch("onTouchEnd", plat.width * 0.5f, plat.height * 0.75f); return; }
    if (frontend_button(cpu, id, down)) return;
    int before = held[id];
    held[id] += down ? 1 : -1;
    if (held[id] < 0) held[id] = 0;
    if (!before && held[id]) gta("onJoyButtonDown", 2, 0, (uint64_t)id, 0, 0);
    if (before && !held[id]) gta("onJoyButtonUp", 2, 0, (uint64_t)id, 0, 0);
}

/* Press/release a pad button straight into the engine (used by the host menu to run native menu actions). */
void host_native_button(int id, int down) {
    int before = held[id];
    held[id] += down ? 1 : -1;
    if (held[id] < 0) held[id] = 0;
    if (!before && held[id]) { if (rt_verbose) rt_log("host: native pad %d down\n", id); gta("onJoyButtonDown", 2, 0, (uint64_t)id, 0, 0); }
    if (before && !held[id]) gta("onJoyButtonUp", 2, 0, (uint64_t)id, 0, 0);
}

static void set_axis(int axis, float v) {
    if (v > 1) v = 1;
    if (v < -1) v = -1;
    if (axis_sent[axis] == v) return;
    axis_sent[axis] = v;
    cpu->v[0].f[0] = v;
    rt_invoke(cpu, rt_need(GTA "setJoyAxis"), 4, plat.env, jcls, 0ull, (uint64_t)axis);
}

static void touch(const char *fn, float x, float y) {
    int phase = !strcmp(fn, "onTouchStart") ? 0 : !strcmp(fn, "onTouchMove") ? 1 : 2;
    if (frontend_touch(cpu, phase, x / (float)plat.width, y / (float)plat.height)) return;
    cpu->v[0].f[0] = x / (float)plat.width;   /* GameView passes view-normalised coordinates */
    cpu->v[1].f[0] = y / (float)plat.height;
    char sym[160];
    snprintf(sym, sizeof sym, GTA "%s", fn);
    rt_invoke(cpu, rt_need(sym), 3, plat.env, jcls, 0ull);
}

/* Keyboard and mouse bindings (rebindable from the host CONTROLS menu, stored in lcs.ini as bind_<button> and
 * mbind_<n>). Slot 1 only keeps the old secondary defaults (LShift = A, LCtrl = B). */
static int keybind[B_COUNT][2] = {
    [B_A] = {SDL_SCANCODE_RETURN, SDL_SCANCODE_LSHIFT}, [B_B] = {SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_LCTRL},
    [B_X] = {SDL_SCANCODE_SPACE, 0}, [B_Y] = {SDL_SCANCODE_F, 0}, [B_START] = {SDL_SCANCODE_P, 0},
    [B_BACK] = {SDL_SCANCODE_TAB, 0}, [B_L1] = {SDL_SCANCODE_Q, 0}, [B_R1] = {SDL_SCANCODE_E, 0},
    [B_UP] = {SDL_SCANCODE_UP, 0}, [B_DOWN] = {SDL_SCANCODE_DOWN, 0}, [B_LEFT] = {SDL_SCANCODE_LEFT, 0},
    [B_RIGHT] = {SDL_SCANCODE_RIGHT, 0}, [B_L3] = {SDL_SCANCODE_C, 0}, [B_R3] = {SDL_SCANCODE_V, 0},
};
static int mousebind[3] = {B_B, B_R1, B_R3}; /* left, right, middle -> pad button (-1 = none) */
int host_bind_get(int btn, int slot) { return btn >= 0 && btn < B_COUNT && slot >= 0 && slot < 2 ? keybind[btn][slot] : 0; }
void host_bind_set(int btn, int sc) {
    if (btn < 0 || btn >= B_COUNT) return;
    for (int i = 0; i < B_COUNT; i++) for (int s = 0; s < 2; s++) if (keybind[i][s] == sc) keybind[i][s] = 0; /* no duplicates */
    keybind[btn][0] = sc; keybind[btn][1] = 0;
}
void host_bind_reset(void) {
    static const int d[B_COUNT][2] = {
        [B_A] = {SDL_SCANCODE_RETURN, SDL_SCANCODE_LSHIFT}, [B_B] = {SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_LCTRL},
        [B_X] = {SDL_SCANCODE_SPACE, 0}, [B_Y] = {SDL_SCANCODE_F, 0}, [B_START] = {SDL_SCANCODE_P, 0},
        [B_BACK] = {SDL_SCANCODE_TAB, 0}, [B_L1] = {SDL_SCANCODE_Q, 0}, [B_R1] = {SDL_SCANCODE_E, 0},
        [B_UP] = {SDL_SCANCODE_UP, 0}, [B_DOWN] = {SDL_SCANCODE_DOWN, 0}, [B_LEFT] = {SDL_SCANCODE_LEFT, 0},
        [B_RIGHT] = {SDL_SCANCODE_RIGHT, 0}, [B_L3] = {SDL_SCANCODE_C, 0}, [B_R3] = {SDL_SCANCODE_V, 0},
    };
    memcpy(keybind, d, sizeof keybind);
    mousebind[0] = B_B; mousebind[1] = B_R1; mousebind[2] = B_R3;
}
int host_mouse_bind(int i) { return i >= 0 && i < 3 ? mousebind[i] : -1; }
void host_mouse_bind_set(int i, int b) { if (i >= 0 && i < 3) mousebind[i] = b; }
void host_bind_ini(const char *key, int value) { /* "bind_<n>" / "mbind_<n>" without the prefix handled by the caller */
    int n = atoi(key + (key[0] == 'm' ? 6 : 5));
    if (key[0] == 'm') host_mouse_bind_set(n, value);
    else if (n >= 0 && n < B_COUNT) { keybind[n][0] = value; keybind[n][1] = 0; }
}
static int key_button(SDL_Scancode sc) {
    if (!sc) return -1;
    for (int b = 0; b < B_COUNT; b++) if (keybind[b][0] == (int)sc || keybind[b][1] == (int)sc) return b;
    return -1;
}

static int pad_button(int b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return B_A;
    case SDL_GAMEPAD_BUTTON_EAST: return B_B;
    case SDL_GAMEPAD_BUTTON_WEST: return B_X;
    case SDL_GAMEPAD_BUTTON_NORTH: return B_Y;
    case SDL_GAMEPAD_BUTTON_START: return B_START;
    case SDL_GAMEPAD_BUTTON_BACK: return B_BACK;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return B_L1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return B_R1;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return B_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return B_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return B_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return B_RIGHT;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return B_L3;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return B_R3;
    default: return -1;
    }
}

/* Display modes: windowed (resizable), borderless fullscreen (desktop mode, best for capture/remote
 * desktop and alt-tab), exclusive fullscreen (the display switches to a fullscreen video mode). */
static const char *mode_names[3] = {"windowed", "borderless", "exclusive"};
void host_set_display_mode(int mode) {
    SDL_Window *w = plat.window;
    if (mode < 0 || mode > 2) mode = 1;
    plat.display_mode = mode; plat.fullscreen = mode != 0;
    tweaks_save("display_mode", mode_names[mode]);
    if (!w) return;
    if (mode == 0) {
        SDL_SetWindowFullscreen(w, false);
        SDL_SetWindowResizable(w, true);
        SDL_SetWindowSize(w, plat.win_w > 0 ? plat.win_w : 1280, plat.win_h > 0 ? plat.win_h : 720);
        SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    } else {
        static SDL_DisplayMode exclusive;
        const SDL_DisplayMode *use = NULL;
        if (mode == 2) { /* the desktop resolution and refresh rate, as a real fullscreen video mode */
            SDL_DisplayID d = SDL_GetDisplayForWindow(w);
            const SDL_DisplayMode *desk = SDL_GetDesktopDisplayMode(d);
            if (desk && SDL_GetClosestFullscreenDisplayMode(d, desk->w, desk->h, desk->refresh_rate, true, &exclusive)) use = &exclusive;
        }
        SDL_SetWindowFullscreenMode(w, use);
        SDL_SetWindowFullscreen(w, true);
    }
    SDL_SyncWindow(w);
    rt_log("host: display mode %s\n", mode_names[mode]);
}
static void toggle_fullscreen(void) {
    static int last_full = 1;
    if (plat.display_mode) { last_full = plat.display_mode; host_set_display_mode(0); }
    else host_set_display_mode(last_full);
}

/* Losing focus (alt-tab, a remote desktop client switching windows) can swallow key-up events:
 * release everything the game still sees as held, and optionally pause. */
static float pad_axis[6];
static void release_all_input(void) {
    for (int id = 0; id < B_COUNT; id++)
        if (held[id]) { held[id] = 0; gta("onJoyButtonUp", 2, 0, (uint64_t)id, 0, 0); }
    for (int i = 0; i < 6; i++) pad_axis[i] = 0;
    mouse_dx = mouse_dy = 0;
    if (touch_down) { touch("onTouchEnd", 0, 0); touch_down = 0; }
}

/* The cursor belongs to the menus (clicks are touches); during play the mouse is captured and
 * drives the right stick, i.e. the game's own camera control. */
static int mouse_look_active(void) { return plat.mouse_look && !plat.in_ui; }

static void pump_events(void) {
    SDL_Event e;
    static int captured = -1;
    static unsigned size_save_at;
    int want = mouse_look_active() && plat.focused;
    if (size_save_at && plat.frames >= size_save_at) { /* remember the window size once dragging stopped */
        char v[16]; size_save_at = 0;
        snprintf(v, sizeof v, "%d", plat.win_w); tweaks_save("width", v);
        snprintf(v, sizeof v, "%d", plat.win_h); tweaks_save("height", v);
    }
    if (want != captured && plat.window) {
        SDL_SetWindowRelativeMouseMode(plat.window, want);
        captured = want;
        if (want && touch_down) { touch("onTouchEnd", 0, 0); touch_down = 0; }
    }
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT: plat.quit = 1; break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
            int w = e.window.data1, h = e.window.data2;
            /* the engine keeps its own size (it cannot rebuild its targets); present.c scales into the window */
            if (w > 0 && h > 0 && plat.display_mode == 0) {
                int pw, ph; SDL_GetWindowSize(plat.window, &pw, &ph);
                if (pw != plat.win_w || ph != plat.win_h) { plat.win_w = pw; plat.win_h = ph; size_save_at = plat.frames + 30; }
            }
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            plat.focused = 0;
            release_all_input();
            if (plat.pause_on_focus_loss && !plat.in_ui) gta("onBackButtonPressed", 0, 0, 0, 0, 0); /* opens the pause menu */
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: plat.focused = 1; break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
            if (e.key.repeat) break;
            plat.pad_input = 0;
            int down = e.type == SDL_EVENT_KEY_DOWN;
            if (e.key.scancode == SDL_SCANCODE_ESCAPE) { if (!down && !frontend_back(cpu)) gta("onBackButtonPressed", 0, 0, 0, 0, 0); break; }
            if ((e.key.scancode == SDL_SCANCODE_F11 || ((e.key.scancode == SDL_SCANCODE_RETURN || e.key.scancode == SDL_SCANCODE_KP_ENTER) && (e.key.mod & SDL_KMOD_ALT)))) {
                if (down) toggle_fullscreen(); /* Alt+Enter / F11: window <-> the last fullscreen mode */
                break;
            }
            if (e.key.scancode == SDL_SCANCODE_F4 && down && (e.key.mod & SDL_KMOD_ALT)) { plat.quit = 1; break; }
            if (down && frontend_key((int)e.key.scancode)) break; /* host menu is capturing a key binding */
            int b = key_button(e.key.scancode);
            if (b >= 0) button(b, down);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
            int down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            plat.pad_input = 0;
            if (down && frontend_mouse(e.button.button)) break; /* host menu is capturing an action key */
            if (mouse_look_active()) {
                int mb = e.button.button == SDL_BUTTON_LEFT ? mousebind[0] : e.button.button == SDL_BUTTON_RIGHT ? mousebind[1]
                       : e.button.button == SDL_BUTTON_MIDDLE ? mousebind[2] : -1;
                if (mb >= 0) button(mb, down);
            } else if (e.button.button == SDL_BUTTON_LEFT) {
                touch_down = down;
                float mx = e.button.x, my = e.button.y; present_map_point(&mx, &my); /* window -> engine surface */
                touch(down ? "onTouchStart" : "onTouchEnd", mx, my);
            }
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            if (mouse_look_active()) { mouse_dx += e.motion.xrel; mouse_dy += e.motion.yrel; }
            else {
                float mx = e.motion.x, my = e.motion.y; present_map_point(&mx, &my);
                if (touch_down) touch("onTouchMove", mx, my);
                else frontend_hover(cpu, mx / (float)plat.width, my / (float)plat.height);
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (frontend_wheel(cpu, e.wheel.y)) break;
            if (mouse_look_active() && e.wheel.y != 0) { int b = e.wheel.y > 0 ? B_RIGHT : B_LEFT; button(b, 1); button(b, 0); }
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (SDL_OpenGamepad(e.gdevice.which)) { gamepads++; rt_log("input: gamepad connected\n"); }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED: {
            SDL_Gamepad *g = SDL_GetGamepadFromID(e.gdevice.which);
            if (g) { SDL_CloseGamepad(g); if (gamepads) gamepads--; }
            break;
        }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            int b = pad_button(e.gbutton.button);
            plat.pad_input = 1;
            if (b >= 0) button(b, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
            break;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            float v = e.gaxis.value / 32767.0f;
            if (v > 0.5f || v < -0.5f) plat.pad_input = 1;
            switch (e.gaxis.axis) {
            case SDL_GAMEPAD_AXIS_LEFTX: pad_axis[0] = v; break;
            case SDL_GAMEPAD_AXIS_LEFTY: pad_axis[1] = v; break;
            case SDL_GAMEPAD_AXIS_RIGHTX: pad_axis[2] = v; break;
            case SDL_GAMEPAD_AXIS_RIGHTY: pad_axis[3] = v; break;
            case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: pad_axis[4] = v; break;
            case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: pad_axis[5] = v; break;
            default: break;
            }
            break;
        }
        default: break;
        }
    }
    /* Combine devices into the single controller the game sees. */
    const bool *ks = SDL_GetKeyboardState(0);
    float lx = pad_axis[0] + (float)(ks[SDL_SCANCODE_D] - ks[SDL_SCANCODE_A]);
    float ly = pad_axis[1] + (float)(ks[SDL_SCANCODE_S] - ks[SDL_SCANCODE_W]);
    lx += script_axis[0]; ly += script_axis[1];
    float rx = pad_axis[2] + script_axis[2], ry = pad_axis[3] + script_axis[3];
    if (mouse_look_active()) {
        rx += mouse_dx * plat.mouse_sensitivity / 24.0f;
        ry += mouse_dy * plat.mouse_sensitivity / 24.0f;
    }
    mouse_dx = mouse_dy = 0;
    set_axis(0, lx);
    set_axis(1, ly);
    set_axis(2, rx);
    set_axis(3, ry);
    set_axis(4, pad_axis[4] + script_axis[4] + (float)ks[SDL_SCANCODE_Z]);
    set_axis(5, pad_axis[5] + script_axis[5] + (float)ks[SDL_SCANCODE_X]);
    actions_frame(ks);
}

void host_script_axis(int axis, float value) { script_axis[axis] = value; }
/* Scripted input for unattended checks: --tap FRAME X Y (normalised), --press FRAME BUTTON */
static struct { unsigned frame; float x, y; int button; int axis; int rel; } script[64];
static int nscript;
static void scripted_input(unsigned frame) {
    unsigned hold = plat.fps_cap > 0 ? (unsigned)plat.fps_cap / 10 : 6; /* ~100 ms */
    if (hold < 4) hold = 4;
    for (int i = 0; i < nscript; i++) {
        if (script[i].axis >= 0) {
            if (frame == script[i].frame) script_axis[script[i].axis] = script[i].x;
        } else if (script[i].button >= 0) {
            unsigned at = script[i].frame + (script[i].rel ? frontend_first_frame() : 0);
            if (script[i].rel && !frontend_first_frame()) continue; /* menu not drawn yet */
            if (frame == at) button(script[i].button, 1);
            if (frame == at + hold) button(script[i].button, 0);
        } else {
            if (frame == script[i].frame) touch("onTouchStart", script[i].x * plat.width, script[i].y * plat.height);
            if (frame == script[i].frame + hold) touch("onTouchEnd", script[i].x * plat.width, script[i].y * plat.height);
        }
    }
}

static void deliver_java_callbacks(void) {
    int n = plat.nevents;
    plat.nevents = 0;
    uint64_t thiz = jcls;
    for (int i = 0; i < n; i++) {
        switch (plat.events[i].kind) {
        case EV_GAME_LOADED:
            /* Rockstar.login("gameload") completes without a Social Club session (offline). */
            rt_log("platform: Social Club sign-in unavailable, continuing offline (GameLoaded)\n");
            rt_invoke(cpu, rt_need(RS "GameLoaded"), 2, plat.env, thiz);
            break;
        case EV_GATE:
            /* The purchase gate UI belongs to the Rockstar SDK and cannot be shown here.  The user
             * of this build has stated that they own the full game, so by default the gate
             * completes the way RockstarJNIlib.ShowGate does for an owner: FinishGate, then
             * StartGame.  --trial restores the closed gate. */
            rt_invoke(cpu, rt_need(RS "FinishGate"), 2, plat.env, thiz);
            if (plat.owner_declared) {
                rt_invoke(cpu, rt_need(plat.events[i].arg ? RS "StartGameBeforeLoad" : RS "StartGame"), 2, plat.env, thiz);
            } else {
                rt_log("platform: Rockstar purchase gate requested; closed (--trial)\n");
            }
            break;
        case EV_UPDATE_ID:
            gta("updateRockstarID", 1, jni_string(""), 0, 0, 0);
            break;
        }
    }
}

static void open_log(void) {
    /* Started from Explorer there is no console: keep the log next to the executable. */
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) return;
    char path[MAX_PATH];
    if (installed) snprintf(path, sizeof path, "%s/lcs.log", user_dir);
    else snprintf(path, sizeof path, "%s/build/lcs.log", root);
    if (!freopen(path, "w", stderr)) return;
    rt_gui = 1;
    freopen(path, "a", stdout);
    setvbuf(stderr, 0, _IONBF, 0);
}

/* Frame limiter.  The game itself never limits (Android's compositor did), so the host holds the
 * requested rate: sleep for the bulk of the wait, then spin for the last stretch so 120 Hz frames
 * are not stretched by the OS timer granularity. */
static void pace_frame(void) {
    static uint64_t deadline;
    uint64_t now = SDL_GetTicksNS();
    /* The engine's per-frame logic assumes steps >= 0.5 in places (see timing.c); 120 is the supported maximum. */
    int cap = plat.fps_cap;
    if (cap <= 0 || cap > 120) cap = 120;
    if (!plat.focused && plat.pause_on_focus_loss && cap > 30) cap = 30; /* in the background: leave the machine alone */
    uint64_t period = 1000000000ull / (unsigned)cap;
    if (!deadline) deadline = now;
    deadline += period;
    if (deadline + 4 * period < now) deadline = now; /* fell far behind (load, pause): do not burst */
    if (now < deadline) {
        if (deadline - now > 2000000ull) SDL_DelayNS(deadline - now - 2000000ull);
        while (SDL_GetTicksNS() < deadline) { }
    }
}

int main(int argc, char **argv) {
    int resize_frame = 0, resize_w = 0, resize_h = 0;
    int max_frames = 0, watchdog = 0, windowed = 0, have_size = 0, check_radio = 0;
    plat.owner_declared = 1;
    plat.display_mode = 1;
    plat.focused = 1;
    plat.pause_on_focus_loss = 0;
    plat.mouse_look = 1;
    plat.mouse_sensitivity = 1.0f;
    plat.in_ui = 1;
    plat.fps_cap = 60;
    find_layout();
    open_log();
    tweaks_load(installed ? user_dir : root);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--verbose")) rt_verbose++;
        else if (!strcmp(argv[i], "--gltrace")) { extern int rt_gl_trace; rt_gl_trace = 1; }
        else if (!strcmp(argv[i], "--hidden")) plat.hidden = 1;
        else if (!strcmp(argv[i], "--owned")) plat.owner_declared = 1;
        else if (!strcmp(argv[i], "--trial")) plat.owner_declared = 0;
        else if (!strcmp(argv[i], "--windowed")) windowed = 1;
        else if (!strcmp(argv[i], "--no-audio")) plat.no_audio = 1;
        else if (!strcmp(argv[i], "--radio-probe")) check_radio = 1;
        else if (!strcmp(argv[i], "--angle")) plat.use_angle = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--set") && i + 2 < argc) { tweaks_set(argv[i + 1], argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--autowalk")) { extern int tweaks_autowalk; tweaks_autowalk = 1; }
        else if (!strcmp(argv[i], "--teleport") && i + 4 < argc) { /* tests: X Y Z HEADING once in gameplay */
            extern float tweaks_teleport[4]; extern int tweaks_teleport_pending;
            for (int k = 0; k < 4; k++) tweaks_teleport[k] = (float)atof(argv[i + 1 + k]);
            tweaks_teleport_pending = 1; i += 4;
        }
        else if (!strcmp(argv[i], "--axis") && i + 3 < argc && nscript < 64) {
            script[nscript].frame = (unsigned)atoi(argv[i + 1]); script[nscript].axis = atoi(argv[i + 2]) % 6;
            script[nscript].x = (float)atof(argv[i + 3]); script[nscript++].button = 0;
            i += 3;
        }
        else if (!strcmp(argv[i], "--tap") && i + 3 < argc && nscript < 64) {
            script[nscript].frame = (unsigned)atoi(argv[i + 1]); script[nscript].x = (float)atof(argv[i + 2]);
            script[nscript].y = (float)atof(argv[i + 3]); script[nscript].axis = -1; script[nscript++].button = -1;
            i += 3;
        }
        else if (!strcmp(argv[i], "--mpress") && i + 2 < argc && nscript < 64) { /* frame relative to the first menu draw */
            script[nscript].frame = (unsigned)atoi(argv[i + 1]); script[nscript].axis = -1; script[nscript].rel = 1; script[nscript++].button = atoi(argv[i + 2]) % B_COUNT;
            i += 2;
        }
        else if (!strcmp(argv[i], "--press") && i + 2 < argc && nscript < 64) {
            script[nscript].frame = (unsigned)atoi(argv[i + 1]); script[nscript].axis = -1; script[nscript++].button = atoi(argv[i + 2]) % B_COUNT;
            i += 2;
        }
        else if (!strcmp(argv[i], "--shots") && i + 2 < argc) { shot_every = (unsigned)atoi(argv[i + 1]); shot_path = argv[i + 2]; i += 2; }
        else if (!strcmp(argv[i], "--resize") && i + 3 < argc) { resize_frame = atoi(argv[i + 1]); resize_w = atoi(argv[i + 2]); resize_h = atoi(argv[i + 3]); i += 3; }
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) watchdog = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 2 < argc) { plat.width = atoi(argv[i + 1]); plat.height = atoi(argv[i + 2]); i += 2; have_size = windowed = 1; }
        else {
            fprintf(stderr, "usage: lcs [--windowed] [--size W H] [--trial] [--no-audio] [--verbose] [--frames N] "
                            "[--shots N pattern] [--tap F X Y] [--press F BUTTON] [--watchdog S] [--gltrace] [--angle]\n");
            return 2;
        }
    }
    if (windowed) plat.display_mode = 0;
    if (have_size) { plat.win_w = plat.width; plat.win_h = plat.height; } /* tests: window = engine size */
    plat.fullscreen = plat.display_mode != 0;
    static char shot_abs[MAX_PATH];
    if (shot_path && _fullpath(shot_abs, shot_path, sizeof shot_abs)) shot_path = shot_abs;

    static char lib_dir[MAX_PATH], assets1[MAX_PATH], assets2[MAX_PATH], files_dir[MAX_PATH], pack_dir[MAX_PATH];
    static char music_dir[MAX_PATH], music_assets[MAX_PATH], music_wad[MAX_PATH];
    if (installed) { /* every APK's assets/ merged into data/assets; packs resolve <pack path>/assets/<name>.wad */
        snprintf(lib_dir, sizeof lib_dir, "%s/lib", data_dir);
        snprintf(assets1, sizeof assets1, "%s/assets", data_dir);
        assets2[0] = 0;
        snprintf(files_dir, sizeof files_dir, "%s/", user_dir);
        snprintf(pack_dir, sizeof pack_dir, "%s", data_dir);
        snprintf(music_dir, sizeof music_dir, "%s", data_dir);
    } else {
    snprintf(lib_dir, sizeof lib_dir, "%s/local/extracted/split_config.arm64_v8a/lib/arm64-v8a", root);
    snprintf(assets1, sizeof assets1, "%s/local/extracted/split_data_main/assets", root);
    snprintf(assets2, sizeof assets2, "%s/local/extracted/base/assets", root);
    snprintf(files_dir, sizeof files_dir, "%s/local/userdata/", root);
    snprintf(pack_dir, sizeof pack_dir, "%s/local/extracted/split_data_main", root);
    snprintf(music_dir, sizeof music_dir, "%s/local/extracted/split_data_music", root);
    }
    snprintf(music_assets, sizeof music_assets, "%s/assets", music_dir);
    snprintf(music_wad, sizeof music_wad, "%s/data_music.wad", music_assets);
    DWORD music_attributes = GetFileAttributesA(music_wad);
    int have_music = music_attributes != INVALID_FILE_ATTRIBUTES && !(music_attributes & FILE_ATTRIBUTE_DIRECTORY);
    rt_log("radio: %s (%s)\n", have_music ? "music pack found" : "music pack missing", music_wad);
    _mkdir(files_dir);
    _chdir(files_dir); /* relative files the game writes (DebugTweak.xml, logs) stay with the user data */
    plat.asset_dirs[plat.nasset_dirs++] = assets1;
    if (assets2[0]) plat.asset_dirs[plat.nasset_dirs++] = assets2;
    if (have_music) plat.asset_dirs[plat.nasset_dirs++] = music_assets;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) rt_fatal("SDL_Init: %s", SDL_GetError());
    if (!have_size) {
        const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        /* the engine always renders at the desktop resolution; the window may be any size (present.c) */
        if (dm) { plat.width = dm->w; plat.height = dm->h; }
        else { plat.width = 1920; plat.height = 1080; }
        if (plat.win_w <= 0 || plat.win_h <= 0) { plat.win_w = 1280; plat.win_h = 720; }
    }
    rt_init(lib_dir);
    { void engine_init(void); engine_init(); } /* Rust engine ports (recomp/engine) */
    cpu = rt_cpu();
    if (watchdog) { void rt_watchdog(unsigned); rt_watchdog((unsigned)watchdog); }
    rt_run_init_arrays();
    rt_log("host: modules loaded and initialised\n");
    if (check_radio) {
        extern int radio_probe(CPU *, const char *);
        return radio_probe(cpu, music_wad);
    }

    jcls = 0;
    rt_invoke(cpu, rt_need("JNI_OnLoad"), 2, plat.vm, 0ull);

    /* GameActivity.onCreate */
    gta("setAssetManager", 1, 0xA55E7, 0, 0, 0);
    gta("setPrivateFilesDir", 1, jni_string(files_dir), 0, 0, 0);
    if (!(gta("isInitialized", 0, 0, 0, 0, 0) & 0xff)) {
        const char *names[] = {"data_main", "data_music"};
        const char *paths[] = {pack_dir, music_dir};
        int packs = have_music ? 2 : 1;
        gta("setAssetPacksInfo", 2, jni_string_array(names, packs), jni_string_array(paths, packs), 0, 0);
        gta("initCloud", 2, jni_string(""), jni_string(""), 0, 0);
        gta("markInitialized", 0, 0, 0, 0, 0);
    }
    /* GameView surface callbacks, then setupDeviceInfo */
    gta("viewOnSurfaceCreated", 0, 0, 0, 0, 0);
    gta("viewOnSurfaceChanged", 3, 0x57F0, (uint64_t)plat.width, (uint64_t)plat.height, 0);
    gta("setIsTVDevice", 1, 0, 0, 0, 0);
    gta("setHasVibrator", 1, 0, 0, 0, 0);
    gta("setOSVersion", 1, 33, 0, 0, 0);
    gta("setDeviceInfo", 4, 2900000, jni_string("Windows PC"), jni_string("PC"), jni_string("x86_64"));
    gta("viewOnResume", 0, 0, 0, 0, 0);
    rt_log("host: entering frame loop (%dx%d%s)\n", plat.width, plat.height, plat.fullscreen ? ", fullscreen" : "");

    uint64_t last = SDL_GetTicksNS();
    int controllers = -1;
    for (unsigned frame = 0; !plat.quit; frame++) {
        pump_events();
        scripted_input(frame);
        if (resize_frame && (int)frame == resize_frame && plat.window) SDL_SetWindowSize(plat.window, resize_w, resize_h); /* test: window resize mid-game */
        deliver_java_callbacks();
        tweaks_apply(cpu);
        uint64_t now = SDL_GetTicksNS();
        float dt = (float)((now - last) / 1e9);
        last = now;
        cpu->v[0].f[0] = dt;
        rt_invoke(cpu, rt_need(GTA "viewOnDrawFrame"), 2, plat.env, jcls);
        /* keyboard and mouse act as controller 0, so the game always has one */
        int count = gamepads > 0 ? gamepads : 1;
        if (count != controllers) { controllers = count; gta("setNoJoysticks", 1, (uint64_t)count, 0, 0, 0); }
        if (max_frames && (int)frame + 1 >= max_frames) break;
        pace_frame();
    }
    rt_log("host: exit after %u presented frames\n", plat.frames);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
