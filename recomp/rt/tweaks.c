/* User settings (lcs.ini next to the project root).  Every setting here is
 * applied by writing a value into a variable the original engine already
 * owns and reads; no engine behaviour is replaced.  See recomp/SETTINGS.md
 * for how to add one. */
#include "tweaks.h"
#include "platform.h"
#include "rt.h"
#include "frontend.h"
#include <stdio.h>
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define G_BASE 0x200000000ull
/* GOT slot used by CHud::GetRadarPosX/Y (libGame 2.4.379): -> global -> HUD layout object */
#define GOT_HUD_LAYOUT (G_BASE + 0x7f9210)
#define LAYOUT_RADAR_WIDGET 0xe0  /* widget: +8 x, +0xc y, +0x10 w, +0x14 h (layout units) */
#define LAYOUT_SCALE_Y 0x4c0
#define LAYOUT_SCALE_SIZE 0x4c4

typedef struct Settings {
    int radar_bottom;
    float radar_margin; /* extra distance from the bottom edge, in HUD lines (448 = screen height) */
    float draw_distance; /* 1.0 = the game's own distances, up to 3.0 */
    float res_scale;     /* internal resolution scale; 0 = the engine's own per-device value */
    float radar_scale;   /* radar size factor (Rust CHud::GetRadarSize*) */
    float fov_scale;     /* field of view factor (Rust CDraw::SetFOV) */
    float camera_distance; /* on-foot camera distance factor (CCam::Process_FollowPed_SA parameter table) */
    float world_fade;    /* seconds of cross-fade between world sectors at a swap; 0 = off */
    float fx[6];         /* sharpen, fxaa, vignette, brightness, contrast, saturation */
    int look;            /* colour preset of the time cycle: 0 original, 1 natural, 2 clear, 3 moody */
    int hud_hidden;      /* bit 0 whole HUD, 1 clock, 2 money, 3 health, 4 armour */
    int hud_modern;      /* rectangular radar with bars under it */
    int touch_controls;  /* draw the Android on-screen controls */
} Settings;
static Settings cfg = {1, 28.0f, 1.0f, 0.0f, 1.0f, 1.0f, .fx = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f}};
static char ini_path[600];

static const char *DEFAULT_INI =
    "; GTA: Liberty City Stories - Windows port settings\n"
    "; Delete this file to restore the defaults.\n"
    "\n"
    "; display mode: windowed (resizable window), borderless (borderless fullscreen),\n"
    "; exclusive (exclusive fullscreen). Alt+Enter / F11 switch window <-> fullscreen.\n"
    "display_mode=borderless\n"
    "; window size in windowed mode\n"
    "width=1280\n"
    "height=720\n"
    "; 1 = open the pause menu and lower the FPS while the window is inactive (alt-tab, remote desktop)\n"
    "pause_on_focus_loss=0\n"
    "\n"
    "; radar position: bottom-left (like the PC games) or top-left (original mobile)\n"
    "radar=bottom-left\n"
    "; extra radar distance from the bottom edge, in HUD lines (screen height = 448)\n"
    "radar_margin=28\n"
    "; radar size: 0.75 .. 1.5\n"
    "radar_scale=1.0\n"
    "; field of view: 0.9 .. 1.4 (zoomed views keep theirs)\n"
    "fov_scale=1.0\n"
    "; on-foot camera distance: 0.8 .. 1.6\n"
    "camera_distance=1.0\n"
    "; Android on-screen controls: 0 = hidden (keyboard, mouse, gamepad), 1 = shown\n"
    "touch_controls=0\n"
    "; per-action keys, separate on foot (foot_) and in vehicles (car_); unset = the gamepad mapping.\n"
    "; actions: accelerate brake handbrake enter_exit sprint jump attack next_weapon prev_weapon pickup\n"
    ";          look_behind look_left look_right horn move_x move_y steer_x steer_y (axes: two keys, negative,positive)\n"
    ";          Keys by SDL name or Mouse1..Mouse3. Also set from SETTINGS > CONTROLS > ON FOOT / VEHICLE KEYS.\n"
    "; examples: car_accelerate=W  car_brake=S  car_handbrake=Space  foot_sprint=Left Shift  foot_jump=Space\n"
    "\n"
    "; seconds of cross-fade between world sectors when the streamed sector swaps in (hides the LOD pop-in); 0 = off\n"
    "world_fade=0\n"
    "\n"
    "\n"
    "; colour preset: original, natural (less yellow), clear (natural + less haze), moody (darker ambient light)\n"
    "look=original\n"
    "; screen effects: fx_sharpen 0..1, fx_fxaa 0/1, fx_vignette 0..1, fx_brightness / fx_contrast 0.7..1.4, fx_saturation 0..1.6\n"
    "fx_sharpen=0\n"
    "fx_fxaa=0\n"
    "fx_vignette=0\n"
    "fx_brightness=1.0\n"
    "fx_contrast=1.0\n"
    "fx_saturation=1.0\n"
    "; HUD style: legacy (the game's own) or modern (rectangular radar with health and armour bars under it)\n"
    "hud_style=legacy\n"
    "; HUD parts, 1 = shown: hud (everything, including messages), hud_clock, hud_money, hud_health, hud_armour\n"
    "hud=1\n"
    "hud_clock=1\n"
    "hud_money=1\n"
    "hud_health=1\n"
    "hud_armour=1\n"
    "\n"
    "; draw distance: 1.0 = original, up to 3.0 (vehicles, peds, small objects, far clip)\n"
    "draw_distance=2.0\n"
    "\n"
    "; frame cap: 30, 60 or 120 (max); vsync=1 also waits for the display\n"
    "fps_cap=60\n"
    "vsync=0\n"
    "\n"
    "; internal resolution (render scale, 0 = engine default, 0.67..1.0)\n"
    "res_scale=0\n"
    "\n"
    "; mouse moves the camera in game (right stick)\n"
    "mouse_look=1\n"
    "mouse_sensitivity=1.0\n"
    "\n"
    "; main menu: 1 = list on the left, 0 = original carousel\n"
    "definitive_menu=1\n";

/* screen effects, colour preset and HUD parts: lcs.ini names and allowed ranges */
static const struct { const char *key; float lo, hi; } fx_keys[6] = {
    {"fx_sharpen", 0, 1}, {"fx_fxaa", 0, 1}, {"fx_vignette", 0, 1}, {"fx_brightness", 0.7f, 1.4f},
    {"fx_contrast", 0.7f, 1.4f}, {"fx_saturation", 0, 1.6f}};
static const char *look_names[4] = {"original", "natural", "clear", "moody"};
static const char *hud_keys[5] = {"hud", "hud_clock", "hud_money", "hud_health", "hud_armour"};
static float fx_clamp(int i, float v) { return v < fx_keys[i].lo ? fx_keys[i].lo : v > fx_keys[i].hi ? fx_keys[i].hi : v; }
void tweaks_fx(float out[6]) { memcpy(out, cfg.fx, sizeof cfg.fx); }
float tweaks_fx_get(int i) { return i >= 0 && i < 6 ? cfg.fx[i] : 0; }
void tweaks_fx_set(int i, float v) {
    if (i < 0 || i > 5) return;
    cfg.fx[i] = fx_clamp(i, v);
    char t[32]; snprintf(t, sizeof t, "%.2f", cfg.fx[i]); tweaks_save(fx_keys[i].key, t);
}
int tweaks_look(void) { return cfg.look; }
void tweaks_set_look(int look) { cfg.look = (look + 4) % 4; tweaks_save("look", look_names[cfg.look]); }
int tweaks_hud_shown(int part) { return !(cfg.hud_hidden & (1 << part)); }
void tweaks_set_hud_shown(int part, int shown) {
    if (part < 0 || part > 4) return;
    cfg.hud_hidden = shown ? cfg.hud_hidden & ~(1 << part) : cfg.hud_hidden | (1 << part);
    tweaks_save(hud_keys[part], shown ? "1" : "0");
}
int tweaks_hud_modern(void) { return cfg.hud_modern; }
void tweaks_set_hud_modern(int modern) { cfg.hud_modern = modern != 0; tweaks_save("hud_style", modern ? "modern" : "legacy"); }
static int visual_ini(const char *key, const char *val) {
    if (!strcmp(key, "hud_style")) { cfg.hud_modern = !strcmp(val, "modern"); return 1; }
    for (int i = 0; i < 6; i++)
        if (!strcmp(key, fx_keys[i].key)) { cfg.fx[i] = fx_clamp(i, (float)atof(val)); return 1; }
    if (!strcmp(key, "look")) {
        for (int i = 0; i < 4; i++) if (!strcmp(val, look_names[i])) cfg.look = i;
        return 1;
    }
    for (int i = 0; i < 5; i++)
        if (!strcmp(key, hud_keys[i])) {
            cfg.hud_hidden = atoi(val) ? cfg.hud_hidden & ~(1 << i) : cfg.hud_hidden | (1 << i);
            return 1;
        }
    return 0;
}

void tweaks_set(const char *line, const char *val) {
    if (visual_ini(line, val)) return;
    if (!strcmp(line, "fullscreen")) plat.display_mode = atoi(val) ? 1 : 0; /* older lcs.ini */
    else if (!strcmp(line, "display_mode")) plat.display_mode = !strcmp(val, "windowed") ? 0 : !strcmp(val, "exclusive") ? 2 : 1;
    else if (!strcmp(line, "pause_on_focus_loss")) plat.pause_on_focus_loss = atoi(val);
    else if (!strcmp(line, "width")) plat.win_w = atoi(val);
    else if (!strcmp(line, "height")) plat.win_h = atoi(val);
    else if (!strcmp(line, "radar")) cfg.radar_bottom = !strcmp(val, "bottom-left");
    else if (!strcmp(line, "radar_margin")) cfg.radar_margin = (float)atof(val);
    else if (!strcmp(line, "radar_scale")) cfg.radar_scale = (float)atof(val);
    else if (!strcmp(line, "fov_scale")) cfg.fov_scale = (float)atof(val);
    else if (!strcmp(line, "camera_distance")) cfg.camera_distance = (float)atof(val);
    else if (!strcmp(line, "world_fade")) cfg.world_fade = (float)atof(val);
    else if (!strcmp(line, "touch_controls")) cfg.touch_controls = atoi(val);
    else if (!strcmp(line, "draw_distance")) cfg.draw_distance = (float)atof(val);
    else if (!strcmp(line, "res_scale")) cfg.res_scale = (float)atof(val);
    else if (!strcmp(line, "fps_cap")) plat.fps_cap = atoi(val);
    else if (!strcmp(line, "vsync")) plat.vsync = atoi(val);
    else if (!strncmp(line, "bind_", 5) || !strncmp(line, "mbind_", 6)) host_bind_ini(line, atoi(val));
    else if (!strncmp(line, "foot_", 5) || !strncmp(line, "car_", 4)) { int host_action_ini(const char *, const char *); host_action_ini(line, val); }
    else if (!strncmp(line, "padfoot_", 8) || !strncmp(line, "padcar_", 7)) { int host_pad_ini(const char *, const char *); host_pad_ini(line, val); }
    else if (!strcmp(line, "pad_preset")) { void host_pad_preset(int); host_pad_preset(!strcmp(val, "classic")); }
    else if (!strcmp(line, "mouse_look")) plat.mouse_look = atoi(val);
    else if (!strcmp(line, "mouse_sensitivity")) plat.mouse_sensitivity = (float)atof(val);
    else if (!strcmp(line, "definitive_menu")) frontend_set_enabled(atoi(val) != 0);
}

void tweaks_load(const char *root) {
    char path[600];
    snprintf(path, sizeof path, "%s/lcs.ini", root);
    snprintf(ini_path, sizeof ini_path, "%s", path);
    FILE *f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (f) { fputs(DEFAULT_INI, f); fclose(f); }
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (line[0] == ';' || line[0] == '#' || !eq) continue;
        *eq = 0;
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = 0;
        tweaks_set(line, val);
    }
    fclose(f);
}


/* Write one key back to lcs.ini (replacing its line, or appending it), keeping comments. */
static const char NLS[] = {10, 0};
void tweaks_save(const char *key, const char *value) {
    if (!ini_path[0]) return;
    static char text[16384];
    size_t n = 0;
    FILE *f = fopen(ini_path, "rb");
    if (f) { n = fread(text, 1, sizeof text - 256, f); fclose(f); }
    text[n] = 0;
    char out[16384 + 256]; size_t o = 0; int done = 0;
    size_t klen = strlen(key);
    for (char *line = text; *line; ) {
        char *end = strchr(line, 0x0a); size_t len = end ? (size_t)(end - line) + 1 : strlen(line);
        if (!done && !strncmp(line, key, klen) && line[klen] == '=') {
            o += (size_t)snprintf(out + o, sizeof out - o, "%s=%s%s", key, value, end ? NLS : "");
            done = 1;
        } else { memcpy(out + o, line, len); o += len; }
        line += len;
    }
    if (!done) o += (size_t)snprintf(out + o, sizeof out - o, "%s%s=%s%s", (o && out[o - 1] != 0x0a) ? NLS : "", key, value, NLS);
    out[o] = 0;
    f = fopen(ini_path, "wb");
    if (f) { fwrite(out, 1, o, f); fclose(f); }
}

float tweaks_world_fade(void) { return cfg.world_fade; }
float tweaks_draw_distance(void) { return cfg.draw_distance; }
void tweaks_set_draw_distance(float k) {
    cfg.draw_distance = k < 1.0f ? 1.0f : k > 3.0f ? 3.0f : k;
    char v[32]; snprintf(v, sizeof v, "%.2f", cfg.draw_distance); tweaks_save("draw_distance", v);
}
float tweaks_res_scale(void) { return cfg.res_scale; }
void tweaks_set_res_scale(float s) {
    cfg.res_scale = s;
    char v[32]; snprintf(v, sizeof v, "%.2f", s); tweaks_save("res_scale", v);
}
int tweaks_radar_bottom(void) { return cfg.radar_bottom; }
float tweaks_radar_scale(void) { return cfg.radar_scale; }
int tweaks_touch_controls(void) { return cfg.touch_controls; }
float tweaks_camera_distance(void) { return cfg.camera_distance > 0 ? cfg.camera_distance : 1.0f; }
void tweaks_set_camera_distance(float s) {
    cfg.camera_distance = s < 0.8f ? 0.8f : s > 1.6f ? 1.6f : s;
    char v[32]; snprintf(v, sizeof v, "%.2f", cfg.camera_distance); tweaks_save("camera_distance", v);
}
/* CCam::Process_FollowPed_SA reads its distances from a table of three 0x3c-byte entries (GOT +0x468). Fields 1, 3
 * and 4 are the follow distances: scale them from the values the game starts with. */
static void apply_camera_distance(void) {
    static float base[45]; static int have;
    uint64_t t = M64(G_BASE + 0x7f9468);
    if (t < G_BASE || t >= G_BASE + 0x100000000ull) return;
    if (!have) {
        for (int i = 0; i < 45; i++) base[i] = MF32(t + i * 4);
        if (!(base[1] > 0.5f && base[1] < 10.0f && base[4] > base[1])) return; /* not the table we know */
        have = 1;
    }
    float k = tweaks_camera_distance();
    static const int fields[] = {1, 3, 4};
    for (int e = 0; e < 3; e++) for (int f = 0; f < 3; f++) MF32(t + e * 60 + fields[f] * 4) = base[e * 15 + fields[f]] * k;
}
float tweaks_fov_scale(void) { return cfg.fov_scale > 0 ? cfg.fov_scale : 1.0f; }
void tweaks_set_fov_scale(float s) {
    cfg.fov_scale = s < 0.8f ? 0.8f : s > 1.5f ? 1.5f : s;
    char v[32]; snprintf(v, sizeof v, "%.2f", cfg.fov_scale); tweaks_save("fov_scale", v);
}
void tweaks_set_radar_scale(float s) {
    cfg.radar_scale = s < 0.5f ? 0.5f : s > 2.0f ? 2.0f : s;
    char v[32]; snprintf(v, sizeof v, "%.2f", cfg.radar_scale); tweaks_save("radar_scale", v);
}
void tweaks_set_radar_bottom(int bottom) {
    cfg.radar_bottom = bottom;
    tweaks_save("radar", bottom ? "bottom-left" : "top-left");
}
/* Fractions of the internal-resolution scale that the engine reports (first read of its own value). */
float tweaks_engine_res_scale(void);


/* Draw distance.  The engine keeps its source distances in exported DebugTweak floats (dv*,
 * value at +0x4c).  C_DeviceDetailSettings copies them into the working values it renders
 * with (dvLodDistanceScale, CVisibilityComponents::ms_*Dist as squares), so only the sources
 * are multiplied here; the engine derives everything else itself. */
#define DV_VALUE 0x4c
typedef struct Scaled { const char *sym; int dv; float limit; uint64_t addr; float base, written; int ready, logged; } Scaled;
static Scaled scaled[] = {
    /* Conservative: world LOD swaps (dvDeviceLodDistanceScale_*) and sector streaming stay native. Raising them
     * made the engine ask for detailed buildings beyond what streaming had loaded, so they popped in. */
    {"dvLODSettingsHigh_VehicleDist0", 1, 0}, {"dvLODSettingsHigh_VehicleDist1", 1, 0}, {"dvLODSettingsHigh_VehicleFadeDist", 1, 0},
    {"dvLODSettingsHigh_VehicleHiddenComponentDist", 1, 0}, {"dvLODSettingsHigh_BigVehicleDist0", 1, 0},
    {"dvLODSettingsHigh_BigVehicleDist1", 1, 0}, {"dvLODSettingsHigh_PedDist", 1, 0}, {"dvLODSettingsHigh_PedFadeDist", 1, 0},
    {"dvLODSettingsMedium_VehicleDist0", 1, 0}, {"dvLODSettingsMedium_VehicleDist1", 1, 0}, {"dvLODSettingsMedium_VehicleFadeDist", 1, 0},
    {"dvLODSettingsMedium_PedDist", 1, 0}, {"dvLODSettingsMedium_PedFadeDist", 1, 0},
    {"dvLODSettingsLow_VehicleDist0", 1, 0}, {"dvLODSettingsLow_VehicleDist1", 1, 0}, {"dvLODSettingsLow_VehicleFadeDist", 1, 0},
    {"dvLODSettingsLow_PedDist", 1, 0}, {"dvLODSettingsLow_PedFadeDist", 1, 0},
    /* world cull fade: distance where small things start being culled, and the size below which they are culled
     * (sizes shrink with the setting, so fewer small objects are dropped) */
    {"dvWorldCullFadeSettingsHigh_MinDist", 1, 0}, {"dvWorldCullFadeSettingsMedium_MinDist", 1, 0}, {"dvWorldCullFadeSettingsLow_MinDist", 1, 0},
    {"dvWorldCullFadeSettingsHigh_NearSize", 1, 0}, {"dvWorldCullFadeSettingsHigh_FarSize", 1, 0},
    {"dvWorldCullFadeSettingsMedium_NearSize", 1, 0}, {"dvWorldCullFadeSettingsMedium_FarSize", 1, 0},
    {"dvWorldCullFadeSettingsLow_NearSize", 1, 0}, {"dvWorldCullFadeSettingsLow_FarSize", 1, 0},
    {"gFarClip", 1, 6000.0f},
    /* Not CRenderer::ms_lodDistScale: CCamera::Process multiplies it with dvLodDistanceScale (the working copy of
     * dvDeviceLodDistanceScale_*), so scaling both squared the LOD distance (4x at 2.0, 9x at 3.0) and the engine
     * asked for detailed models far outside what sector streaming keeps loaded. It stays the native slider's. */
};

#define MENU_APPLY_FLAG (G_BASE + 0x9ec538 + 0x266) /* makes DeferredApplySettings re-read the dv values */

static void scale_world_cull(float k);
static void scale_draw_distance(void) {
    float k = cfg.draw_distance < 1.0f ? 1.0f : cfg.draw_distance > 3.0f ? 3.0f : cfg.draw_distance;
    static float last_k = 1.0f;
    int changed = 0;
    for (unsigned i = 0; i < sizeof scaled / sizeof scaled[0]; i++) {
        Scaled *v = &scaled[i];
        if (!v->addr) {
            uint64_t a = rt_sym(v->sym);
            if (!a) continue;
            v->addr = a + (v->dv ? DV_VALUE : 0);
        }
        float now = MF32(v->addr);
        if (!(now > 0)) continue; /* not initialised by the engine yet */
        if (v->ready && now == v->written && k == last_k) continue;
        if (!v->ready || now != v->written) v->base = now; /* engine (re)wrote its own value */
        if (strstr(v->sym, "Size")) v->written = v->base / (k * k); /* cull sizes fall faster than distances grow */
        else if (strstr(v->sym, "LodDistanceScale")) v->written = v->base * (1.0f + (k - 1.0f) * 0.5f); /* world LOD swaps: 2x at 3.0 */
        else v->written = v->base * k;
        if (v->limit > 0 && v->written > v->limit) v->written = v->limit;
        MF32(v->addr) = v->written;
        changed = 1;
        if (rt_verbose && v->logged++ < 3) rt_log("tweak: %s %.3f -> %.3f\n", v->sym, v->base, v->written);
        v->ready = 1;
    }
    if (changed && k != last_k) M8(MENU_APPLY_FLAG) = 1;
    last_k = k;
    scale_world_cull(k);
}

/* World and model cull fade. cWorldStreamEx::DrawPrimitiveRef drops (fading) every world mesh whose screen size is
 * below a threshold beyond MinDist (CalcScreenSize): that is the building detail popping in. The engine copies
 * the dvWorldCullFadeSettings* / dvModelCullFadeSettings* sources into gDeviceDetailSettings once at start-up
 * (three detail levels of {MinDist, Range, NearSize, FarSize} at +0x00 world and +0x34 model), so the dv sources
 * above have no effect on it; the working copies are scaled here instead: MinDist and Range by k^2, sizes by 1/k^2. */
static void scale_world_cull(float k) {
    static uint64_t dds; static float base[2][3][4], written[2][3][4]; static int ready; static float applied = -1;
    if (!dds) { dds = rt_sym("gDeviceDetailSettings"); if (!dds) return; }
    float k2 = k * k;
    for (int t = 0; t < 2; t++)
        for (int l = 0; l < 3; l++) {
            uint64_t e = dds + (t ? 0x34 : 0) + l * 16;
            for (int f = 0; f < 4; f++) {
                float now = MF32(e + f * 4);
                if (!ready || now != written[t][l][f]) { base[t][l][f] = now; applied = -1; }
            }
        }
    if (!(base[0][0][0] > 0)) return; /* not initialised by the engine yet */
    ready = 1;
    if (applied == k2) return;
    for (int t = 0; t < 2; t++)
        for (int l = 0; l < 3; l++) {
            uint64_t e = dds + (t ? 0x34 : 0) + l * 16;
            written[t][l][0] = base[t][l][0] * k2;
            written[t][l][1] = base[t][l][1] * k2;
            written[t][l][2] = base[t][l][2] / k2;
            written[t][l][3] = base[t][l][3] / k2;
            for (int f = 0; f < 4; f++) MF32(e + f * 4) = written[t][l][f];
        }
    applied = k2;
    if (rt_verbose) rt_log("tweak: world cull min dist %.0f -> %.0f, size %.3f -> %.4f\n", base[0][0][0], written[0][0][0], base[0][0][3], written[0][0][3]);
}

/* Internal resolution: the engine picks one of three dv_OffscreenResScale floats by graphics detail. */
static void apply_res_scale(void) {
    static const char *names[3] = {"dv_OffscreenResScaleLow", "dv_OffscreenResScaleMedium", "dv_OffscreenResScaleHigh"};
    static uint64_t addr[3]; static float base[3], written[3]; static int ready, applied = -1;
    float want = cfg.res_scale;
    for (int i = 0; i < 3; i++) {
        if (!addr[i]) { uint64_t a = rt_sym(names[i]); if (!a) return; addr[i] = a + DV_VALUE; }
        float now = MF32(addr[i]);
        if (!(now > 0)) return;
        if (!ready || now != written[i]) base[i] = now;
    }
    ready = 1;
    int key = (int)(want * 1000.0f + 0.5f);
    if (key == applied && MF32(addr[0]) == written[0]) return;
    for (int i = 0; i < 3; i++) {
        written[i] = want > 0 ? want : base[i];
        MF32(addr[i]) = written[i];
    }
    if (applied != -1) M8(MENU_APPLY_FLAG) = 1;
    applied = key;
    if (rt_verbose) rt_log("tweak: internal resolution %.2f (engine %.2f/%.2f/%.2f)\n", want, base[0], base[1], base[2]);
}

/* --verbose diagnostic: real time against the engine's own clocks, and the player's movement.
 * --autowalk holds the stick forward once the player has stood still in gameplay, so frame-rate
 * runs can be compared from the same starting situation. */
extern void host_script_axis(int axis, float value);
int tweaks_autowalk;
static void timing_diag(void) {
    static uint64_t t0; static uint32_t clock0; static float prev[3]; static int still, walking;
    uint64_t now = SDL_GetTicksNS();
    if (now - t0 < 1000000000ull) return;
    uint64_t base = rt_sym("_ZN6CWorld7PlayersE");
    uint64_t ped = base ? M64(base) : 0;
    uint32_t clk = M32(M64(G_BASE + 0x7f9208));
    if (t0 && ped) {
        double real = (now - t0) / 1e9, game = (clk - clock0) / 1000.0;
        float x = MF32(ped + 0x40), y = MF32(ped + 0x44), z = MF32(ped + 0x48);
        double d = sqrt((x - prev[0]) * (x - prev[0]) + (y - prev[1]) * (y - prev[1]));
        int cutscene = M8(G_BASE + 0x847d35);
        rt_log("diag: real=%.2f engine=%.2f step=%.3f pos=%.1f,%.1f,%.1f speed=%.2f u/s(real) cutscene=%d walking=%d cclock=%.3f\n",
               real, game, MF32(G_BASE + 0x8722dc), x, y, z, d / real, cutscene, walking, MF32(G_BASE + 0x848a54));
        {
            static const char *w[] = {"_ZN9CRenderer15ms_lodDistScaleE", "_ZN21CVisibilityComponents18ms_vehicleLod0DistE",
                                      "_ZN21CVisibilityComponents14ms_pedFadeDistE", "dvLodDistanceScale", "gFarClip"};
            char line[300]; int n = 0;
            for (unsigned i = 0; i < 5; i++) { uint64_t a2 = rt_sym(w[i]); if (a2) n += snprintf(line + n, sizeof line - n, " %s=%.2f", w[i] + (w[i][0]==95 ? 8 : 0), MF32(a2 + (i >= 3 ? DV_VALUE : 0))); }
            rt_log("diag: dist%s\n", line);
        }
        {   /* render list: visible entities, and how many of them are big-building LOD models (flag 0x10) */
            int n = MS32(G_BASE + 0x85ad6c), big = 0;
            uint64_t infos = M64(M64(G_BASE + 0x7f8fb0)); int ninfos = MS32(M64(G_BASE + 0x7f8fa0));
            for (int i = 0; i < n && i < 4000; i++) {
                uint64_t e = M64(G_BASE + 0x85ad80 + 8ull * i);
                int16_t mi = e ? MS16(e + 0x7c) : -1;
                uint64_t info = mi >= 0 && mi < ninfos ? M64(infos + 8ull * mi) : 0;
                if (info && (M16(info + 0x56) & 0x10)) big++;
            }
            rt_log("diag: render list %d entities, %d big-building LODs\n", n, big);
            { void engine_cull_stats(unsigned *, unsigned *); unsigned k, d; engine_cull_stats(&k, &d);
              rt_log("diag: world meshes kept %u, culled by size %u (last second)\n", k, d); }
        }
        {
            static int dumped;
            if (!dumped++) {
                static const char *sn[] = {"dvStreamerCameraPreStreamDist", "dvStreamerSpeedPreStreamTime", "dvStreamerCreateNumWorldsPerFrame",
                    "dvStreamerCreateNumModelsPerFrame", "dvStreamerCreateNumBuffersPerFrame", "dvStreamerCreateNumTexturesPerFrame",
                    "dvWorldCullFadeSettingsHigh_MinDist", "dvWorldCullFadeSettingsHigh_Range", "dvWorldCullFadeSettingsHigh_NearSize",
                    "dvWorldCullFadeSettingsHigh_FarSize", "dvWorldCullFadeSettingsMedium_MinDist", "dvWorldCullFadeSettingsMedium_Range",
                    "dvWorldCullFadeSettingsLow_MinDist", "dvWorldCullFadeSettingsLow_Range", "dvLodDistanceScale",
                    "dvLODSettingsHigh_PopulationAddMaxRadius", "dvLODSettingsHigh_PopulationCullRange"};
                for (unsigned i = 0; i < sizeof sn / sizeof sn[0]; i++) { uint64_t a2 = rt_sym(sn[i]); if (a2) rt_log("diag: dv %s=%.3f\n", sn[i], MF32(a2 + DV_VALUE)); }
            }
        }
        if (tweaks_autowalk && !walking && !cutscene && clk > 20000 && d < 0.05 && ++still >= 4) {
            walking = 1;
            rt_log("diag: autowalk started\n");
        }
        if (walking) { /* turn the stick every 2 s so the player does not stay pinned against a wall */
            static int ticks; int k = (ticks++ / 2) % 4;
            host_script_axis(0, k == 1 ? -1.0f : k == 3 ? 1.0f : 0.0f);
            host_script_axis(1, k == 0 ? -1.0f : k == 2 ? 1.0f : 0.0f);
        }
        prev[0] = x; prev[1] = y; prev[2] = z;
    }
    t0 = now; clock0 = clk;
}

/* --teleport X Y Z HEADING (tests): CPed::Teleport on the player once the game clock has run 1.5 s.
 * HEADING is accepted but not applied yet (calling CPlaceable::SetHeading from here crashed CWorld::Process). */
float tweaks_teleport[4];
int tweaks_teleport_pending;
static void teleport(CPU *c) {
    uint64_t base = rt_sym("_ZN6CWorld7PlayersE");
    uint64_t ped = base ? M64(base) : 0;
    if (!ped || M32(M64(G_BASE + 0x7f9208)) < 1500) return;
    CPU keep = *c;
    static float pos[3]; /* CPed::Teleport(CVector const &): guest pointers are host pointers */
    pos[0] = tweaks_teleport[0]; pos[1] = tweaks_teleport[1]; pos[2] = tweaks_teleport[2];
    c->x[0] = ped; c->x[1] = (uint64_t)pos;
    rt_call(c, G_BASE + 0x4324e8);
    *c = keep;
    tweaks_teleport_pending = 0;
    rt_log("diag: teleported to %.1f %.1f %.1f heading %.2f\n", tweaks_teleport[0], tweaks_teleport[1], tweaks_teleport[2], tweaks_teleport[3]);
}

/* --verbose: once in 120 frames, how far from the player the vehicles are (CPools vehicle pool: entries, flags with the
 * sign bit set for free slots, size; entry stride 0x7b0, position +0x40). */
static void vehicle_diag(void) {
    static unsigned n;
    if (++n % 120) return;
    uint64_t players = rt_sym("_ZN6CWorld7PlayersE"), ped = players ? M64(players) : 0;
    uint64_t pool = M64(M64(G_BASE + 0x7f9720));
    if (!ped || !pool) return;
    int bin[3] = {0, 0, 0}; /* traffic rings; scripted and parked cars far away are left out */
    double px = MF32(ped + 0x40), py = MF32(ped + 0x44);
    for (unsigned i = 0; i < M32(pool + 0x10) && i < 1000; i++) {
        if (MS8(M64(pool + 8) + i) < 0) continue;
        uint64_t v = M64(pool) + 0x7b0ull * i;
        double dx = MF32(v + 0x40) - px, dy = MF32(v + 0x44) - py, d = sqrt(dx * dx + dy * dy);
        if (d < 400) bin[d < 80 ? 0 : d < 160 ? 1 : 2]++;
    }
    rt_log("diag: vehicles within 80: %d, 80-160: %d, 160-400: %d\n", bin[0], bin[1], bin[2]);
}

void tweaks_apply(CPU *c) {
    { void port_test(CPU *c); port_test(c); }
    if (tweaks_teleport_pending) teleport(c);
    if (rt_verbose) timing_diag();
    {   /* radar position and size: Rust ports of the CHud radar getters (recomp/engine/src/hud.rs) */
        void lcs_set_radar(int bottom, float margin, float scale);
        lcs_set_radar(cfg.radar_bottom, cfg.radar_margin, cfg.radar_scale);
        void lcs_set_fov_scale(float scale);
        lcs_set_fov_scale(cfg.fov_scale > 0 ? cfg.fov_scale : 1.0f);
        {   /* colour preset: share of the weather colour filter tint kept, fog start factor, ambient light factor */
            static const float looks[4][3] = {{1.0f, 1.0f, 1.0f}, {0.1f, 1.0f, 1.0f}, {0.1f, 3.0f, 1.0f}, {1.0f, 1.0f, 0.75f}};
            void lcs_set_look(float tint, float fog, float ambient); void lcs_set_hud(int hidden);
            lcs_set_look(looks[cfg.look][0], looks[cfg.look][1], looks[cfg.look][2]);
            lcs_set_hud(cfg.hud_hidden);
            void lcs_set_hud_style(int modern); lcs_set_hud_style(cfg.hud_modern);
        }
        { void lcs_set_generation_scale(float s); const char *g = getenv("LCS_GEN"); /* tests: override */
          lcs_set_generation_scale(g ? (float)atof(g) : cfg.draw_distance); }
        if (rt_verbose) vehicle_diag();
        { void lcs_set_far_clip_scale(float s); const char *e = getenv("LCS_FAR");
          /* tests: LCS_FAR overrides the factor */
          lcs_set_far_clip_scale(e ? (float)atof(e) : cfg.draw_distance); }
        if (rt_verbose) { static unsigned n; if (++n % 120 == 0) { void lcs_far_clip_seen(float *o); float v[2]; lcs_far_clip_seen(v);
          rt_log("diag: time cycle far clip %.0f fog start %.0f (before scaling)", v[0], v[1]); rt_log("%c", 10); } }
        apply_camera_distance();
        void lcs_radar_layout(void); lcs_radar_layout();
    }
    scale_draw_distance();
    {   /* debugging: LCS_DV="dvName=value,dvName=value" forces DebugTweak floats every frame */
        static char spec[1024]; static int init;
        if (!init) { const char *e = getenv("LCS_DV"); if (e) snprintf(spec, sizeof spec, "%s", e); init = 1; }
        if (spec[0]) {
            char buf[1024]; snprintf(buf, sizeof buf, "%s", spec);
            for (char *tok = strtok(buf, ","); tok; tok = strtok(0, ",")) {
                char *eq = strchr(tok, '='); if (!eq) continue; *eq = 0;
                uint64_t a = rt_sym(tok);
                if (a && MF32(a + DV_VALUE) != (float)atof(eq + 1)) { MF32(a + DV_VALUE) = (float)atof(eq + 1); M8(MENU_APPLY_FLAG) = 1; }
            }
        }
    }
    apply_res_scale();
    {   /* big-building LOD models are drawn farther (Rust port of CSimpleModelInfo, recomp/engine/src/lod.rs) */
        void engine_set_far_lod_scale(float k); void engine_report(void);
        float k = cfg.draw_distance < 1.0f ? 1.0f : cfg.draw_distance > 3.0f ? 3.0f : cfg.draw_distance;
        engine_set_far_lod_scale(k);
        engine_report();
    }
}

/* Touchscreen::Render draws the Android on-screen controls (stick, fist, run, car, weapon buttons, steering
 * selector). The PC build plays with keyboard, mouse or a gamepad, so it is skipped unless lcs.ini sets
 * touch_controls=1. Touch input itself (Touchscreen::Update) still runs. */
extern void G_543b7c_orig(CPU *c);
void G_543b7c(CPU *c) {
    if (tweaks_touch_controls()) G_543b7c_orig(c);
}
