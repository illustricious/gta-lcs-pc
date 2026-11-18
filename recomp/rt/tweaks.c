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
} Settings;
static Settings cfg = {1, 28.0f, 1.0f, 0.0f};
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

void tweaks_set(const char *line, const char *val) {
    if (!strcmp(line, "fullscreen")) plat.display_mode = atoi(val) ? 1 : 0; /* older lcs.ini */
    else if (!strcmp(line, "display_mode")) plat.display_mode = !strcmp(val, "windowed") ? 0 : !strcmp(val, "exclusive") ? 2 : 1;
    else if (!strcmp(line, "pause_on_focus_loss")) plat.pause_on_focus_loss = atoi(val);
    else if (!strcmp(line, "width")) plat.win_w = atoi(val);
    else if (!strcmp(line, "height")) plat.win_h = atoi(val);
    else if (!strcmp(line, "radar")) cfg.radar_bottom = !strcmp(val, "bottom-left");
    else if (!strcmp(line, "radar_margin")) cfg.radar_margin = (float)atof(val);
    else if (!strcmp(line, "draw_distance")) cfg.draw_distance = (float)atof(val);
    else if (!strcmp(line, "res_scale")) cfg.res_scale = (float)atof(val);
    else if (!strcmp(line, "fps_cap")) plat.fps_cap = atoi(val);
    else if (!strcmp(line, "vsync")) plat.vsync = atoi(val);
    else if (!strncmp(line, "bind_", 5) || !strncmp(line, "mbind_", 6)) host_bind_ini(line, atoi(val));
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
void tweaks_set_radar_bottom(int bottom) {
    cfg.radar_bottom = bottom;
    tweaks_save("radar", bottom ? "bottom-left" : "top-left");
}
/* Fractions of the internal-resolution scale that the engine reports (first read of its own value). */
float tweaks_engine_res_scale(void);

static void radar_to_bottom(void) {
    static float written_y = -1e9f, original_y;
    uint64_t global = M64(GOT_HUD_LAYOUT);
    if (!global) return;
    uint64_t layout = M64(global);
    if (!layout) return;
    uint64_t widget = M64(layout + LAYOUT_RADAR_WIDGET);
    if (!widget) return;
    float y = MF32(widget + 0xc), h = MF32(widget + 0x14);
    float sy = MF32(layout + LAYOUT_SCALE_Y), ss = MF32(layout + LAYOUT_SCALE_SIZE);
    if (!(sy > 0) || !(ss > 0)) return;
    if (y == written_y) return;
    original_y = y; /* the engine (re)built its layout: mirror the top margin to the bottom edge */
    /* HUD space is 448 lines tall (PS2 heritage): position = y * sy, size = h * ss in that space */
    float top = original_y * sy, size = h * ss;
    written_y = (448.0f - top - size - cfg.radar_margin) / sy;
    MF32(widget + 0xc) = written_y;
    if (rt_verbose) rt_log("tweak: radar y %.1f -> %.1f layout units (x %.1f, size %.1f x %.1f, scale %.3f/%.3f)\n", original_y,
                           written_y, MF32(widget + 8), MF32(widget + 0x10), h, sy, ss);
}

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

void tweaks_apply(CPU *c) {
    if (tweaks_teleport_pending) teleport(c);
    if (rt_verbose) timing_diag();
    if (cfg.radar_bottom) radar_to_bottom();
    scale_draw_distance();
    apply_res_scale();
    {   /* big-building LOD models are drawn farther (Rust port of CSimpleModelInfo, recomp/engine/src/lod.rs) */
        void engine_set_far_lod_scale(float k); void engine_report(void);
        float k = cfg.draw_distance < 1.0f ? 1.0f : cfg.draw_distance > 3.0f ? 3.0f : cfg.draw_distance;
        engine_set_far_lod_scale(k);
        engine_report();
    }
}
