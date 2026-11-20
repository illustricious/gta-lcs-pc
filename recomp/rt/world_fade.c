/* Cross-fade between the old and the new world sector when cWorldStream::SwapToStreamedSector swaps them.
 *
 * The detail/LOD boundary of the building sectors is baked into the sector data, so the swap shows as a pop. Here the
 * swap keeps the old sector alive for a moment (RemoveOldSectorModels, which frees the models only the old sector
 * uses, is deferred to the end of the fade) and cWorldStream::Render is run a second time with the old sector
 * temporarily installed as the current one. Both passes are dithered with complementary screen-door patterns
 * (rt/dissolve.c): the new sector shows the pixels where noise < t, the old one the rest.
 *
 * cWorldStream layout used (libGame 2.4.379): vectors of sector pointers at +0x38 (current) and +0x80 (the other one,
 * the old sector after a swap), cell/flag fields +0x50..+0x60 and +0x98..+0xa8 swap together with them.
 * Enabled with world_fade=<seconds> in lcs.ini (or LCS_WORLD_FADE). */
#include "rt.h"
#include "tweaks.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void G_39d28c_orig(CPU *c);
extern void G_39bfd0_orig(CPU *c);
extern void G_39ef48_orig(CPU *c);
void rt_dissolve_set(float v);

static int in_swap;          /* inside the swap call: RemoveOldSectorModels is deferred */
static int deferred;         /* the swap skipped RemoveOldSectorModels */
static int fading;
static uint64_t cw;          /* the cWorldStream */
static uint64_t old_sector;  /* first sector pointer of the old vector */
static uint64_t t0_ns;
static float duration;
static unsigned swaps, aborted;

static float cfg_duration(void) {
    static float v = -1;
    if (v < 0) {
        const char *e = getenv("LCS_WORLD_FADE");
        v = e ? (float)atof(e) : tweaks_world_fade();
        if (v < 0) v = 0;
        if (v > 3) v = 3;
    }
    return v;
}

static void call_remove_old(CPU *c, uint64_t w) {
    CPU save = *c;
    c->x[0] = w;
    G_39bfd0_orig(c);
    *c = save;
}

static void end_fade(CPU *c, const char *why) {
    if (fading && rt_verbose) rt_log("swap: fade end (%s)\n", why);
    fading = 0;
    rt_dissolve_set(1.0f);
    if (deferred) { deferred = 0; call_remove_old(c, cw); }
}

static uint64_t vec_first(uint64_t w, int off) {
    uint64_t b = M64(w + off), e = M64(w + off + 8);
    return b && b != e ? M64(b) : 0;
}

static int dbg(void) { static int v = -1; if (v < 0) { const char *e = getenv("LCS_WORLD_FADE_DBG"); v = e ? atoi(e) : 0; } return v; }

void G_39bfd0(CPU *c) {
    if (in_swap && !(dbg() & 2)) { deferred = 1; return; }
    G_39bfd0_orig(c);
}

void G_39d28c(CPU *c) {
    uint64_t w = c->x[0];
    if (cfg_duration() <= 0) { G_39d28c_orig(c); return; }
    if (fading) end_fade(c, "next swap");
    uint64_t before = vec_first(w, 0x38);
    in_swap = 1; deferred = 0;
    G_39d28c_orig(c);
    in_swap = 0;
    uint64_t after = vec_first(w, 0x38), old = vec_first(w, 0x80);
    swaps++;
    if (rt_verbose) rt_log("swap: #%u before=%llx now=%llx old=%llx deferred=%d\n", swaps, (unsigned long long)before,
                           (unsigned long long)after, (unsigned long long)old, deferred);
    if (before && old == before && after != before) {
        cw = w; old_sector = before; fading = 1; duration = cfg_duration(); t0_ns = SDL_GetTicksNS();
    } else if (deferred) {
        deferred = 0; call_remove_old(c, w);
    }
}

/* swap the cWorldStream fields that name the current and the old sector */
static void swap_sectors(uint64_t w) {
    uint8_t tmp[0x30];
    memcpy(tmp, (void *)(w + 0x30), 0x30); memcpy((void *)(w + 0x30), (void *)(w + 0x78), 0x30); memcpy((void *)(w + 0x78), tmp, 0x30);
    /* +0x30..0x5f <-> +0x78..0xa7 covers the vectors, cells and flags that SwapToStreamedSector exchanges */
}

void G_39ef48(CPU *c) {
    if (!fading || (dbg() & 1) || ((dbg() & 4) && (uint32_t)c->x[1] != 0) || ((dbg() & 8) && (uint32_t)c->x[1] == 0)) { G_39ef48_orig(c); return; }
    CPU args = *c;
    float t = (float)((SDL_GetTicksNS() - t0_ns) / 1e9) / duration;
    if (vec_first(cw, 0x80) != old_sector) { aborted++; G_39ef48_orig(c); end_fade(c, "old sector gone"); return; }
    if (t >= 1.0f) { G_39ef48_orig(c); end_fade(c, "done"); return; }
    /* new sector: the pixels where noise < t */
    rt_dissolve_set(t);
    G_39ef48_orig(c);
    CPU after = *c;
    /* old sector: the other pixels */
    *c = args;
    swap_sectors(cw);
    rt_dissolve_set(-(t > 1e-4f ? t : 1e-4f));
    G_39ef48_orig(c);
    swap_sectors(cw);
    rt_dissolve_set(1.0f);
    *c = after;
}
