/* High-refresh timing correction for libGame 2.4.379.
 * CTimer::Update at 0x370c64 floors the animation step to 0.5 (10 ms),
 * even when a real frame took 8/9 ms. CCutsceneMgr::Update_overlay uses
 * ms_fTimeStepNonClipped / 50, so 120 Hz otherwise advances its animation
 * clock at least 20% faster than the audio device's real-time clock.
 * Keep the original timer, pause handling, and millisecond clocks; only
 * remove that lower floor. The engine's OS clock is an integer millisecond
 * (8 or 9 ms at 120 fps), so the step is taken from the host's
 * nanosecond timer when it agrees with the engine's own measurement.
 * These addresses are protected by the module's startup SHA-256 check. */
#include "rt.h"
#include <SDL3/SDL.h>
#include <stdlib.h>

#define G 0x200000000ull
#define FRAME_CYCLES (G + 0x8722c0)
#define TIME_SCALE (G + 0x8722d8)
#define TIME_STEP (G + 0x8722dc)
#define TIME_STEP_NONCLIPPED (G + 0x8722e8)
#define USER_PAUSE (G + 0x8722f4)
#define CODE_PAUSE (G + 0x8722f5)
#define SNAPSHOT_ACTIVE (G + 0x9b60cc)
#define CHASE_RECORD_STATUS (G + 0x8990e0)

extern void G_370b30_orig(CPU *c);
extern void G_41e0cc_orig(CPU *c);
extern void G_48ca2c_orig(CPU *c);
extern void G_4106e8_orig(CPU *c);

void G_370b30(CPU *c) {
    static uint64_t last_ns;
    uint64_t now_ns = SDL_GetTicksNS();
    uint64_t delta_ns = last_ns ? now_ns - last_ns : 0;
    last_ns = now_ns;
    G_370b30_orig(c);
    /* CRecordDataForChase::Status == 1 deliberately forces a fixed step.
     * Pauses and the snapshot mode have separate original timer semantics. */
    if (M8(USER_PAUSE) || M8(CODE_PAUSE) || M8(SNAPSHOT_ACTIVE) || M8(CHASE_RECORD_STATUS) == 1) return;
    /* diagnostic: LCS_STEP=<n> forces a constant step (to measure how engine logic reacts to small steps) */
    static float forced = -1;
    if (forced < -0.5f) { const char *e = getenv("LCS_STEP"); forced = e ? (float)atof(e) : 0; }
    if (forced > 0) { MF32(TIME_STEP) = MF32(TIME_STEP_NONCLIPPED) = forced; return; }
    float raw = ((float)M32(FRAME_CYCLES) * MF32(TIME_SCALE)) / 2949120.0f;
    static unsigned corrected, samples;
    static double measured_seconds, animation_seconds;
    /* Comparing with exactly 0.5 confines this to the original lower-floor
     * case and leaves the original upper clamp and exceptional modes intact. */
    if (raw >= 0.0f && raw < 0.5f && MF32(TIME_STEP_NONCLIPPED) == 0.5f) {
        float step = raw;
        /* 50 steps per second, scaled like the engine does; accept the host timer within the +-1 ms
         * the integer clock can be off by (otherwise: first frame, long stall, odd time scale). */
        float hires = (float)((double)delta_ns * 50.0 / 1e9) * MF32(TIME_SCALE);
        if (delta_ns && hires > raw - 0.06f && hires < raw + 0.06f && hires > 0.0f) step = hires;
        MF32(TIME_STEP_NONCLIPPED) = step;
        if (MF32(TIME_STEP) == 0.5f) MF32(TIME_STEP) = step;
        corrected++;
        if (corrected == 1) rt_log("timing: high-refresh animation step corrected (%.3f ms, step %.6f)\n", step * 20.0f, step);
    }
    if (rt_verbose) {
        measured_seconds += raw / 50.0;
        animation_seconds += MF32(TIME_STEP_NONCLIPPED) / 50.0;
        if (++samples == 600) {
            rt_log("timing: 600 updates measured=%.6fs animation=%.6fs sub-10ms corrections=%u cutscene=%.3fs running=%u\n",
                   measured_seconds, animation_seconds, corrected,
                   MF32(G + 0x848a54), (unsigned)M8(G + 0x847d35));
            samples = 0;
            measured_seconds = animation_seconds = 0;
        }
    }
}

/* CPed::CalculateNewVelocity (0x41e0cc) turns the animation shift into a velocity: shift / step for
 * step >= 0.5, but shift * 0.5 for step < 0.5 (left over from the 0.5 step floor). That is a 4x drop at
 * the 0.5 boundary, so at 120 fps pedestrians crawl. Rescale to the intended shift / step. */
void G_41e0cc(CPU *c) {
    uint64_t ped = c->x[0];
    G_41e0cc_orig(c);
    float step = MF32(TIME_STEP);
    if (step > 0.0f && step < 0.5f) {
        float k = 2.0f / step; /* (1 / step) / 0.5 */
        MF32(ped + 0x5c0) *= k;
        MF32(ped + 0x5c4) *= k;
    }
}

/* Particle systems were tuned for 30-60 fps: parts of CParticle::Update (e.g. a fixed -0.15 on a velocity per call)
 * and the CParticleObject emitters run once per frame, not once per time step, so at 120 fps smoke and sparks
 * behave twice as fast. Run them at most ~60 times a second with the accumulated time step. */
static void gated(CPU *c, void (*orig)(CPU *), float *acc) {
    float step = MF32(TIME_STEP);
    if (step <= 0.0f || M8(USER_PAUSE) || M8(CODE_PAUSE)) { orig(c); return; }
    *acc += step;
    if (*acc < 0.7f) return; /* one 60 fps frame is 0.83; the margin keeps 60 fps jitter from skipping */
    float keep = MF32(TIME_STEP), keep_nc = MF32(TIME_STEP_NONCLIPPED);
    MF32(TIME_STEP) = *acc; MF32(TIME_STEP_NONCLIPPED) = *acc;
    *acc = 0.0f;
    orig(c);
    MF32(TIME_STEP) = keep; MF32(TIME_STEP_NONCLIPPED) = keep_nc;
}
void G_48ca2c(CPU *c) { static float acc; gated(c, G_48ca2c_orig, &acc); }
void G_4106e8(CPU *c) { static float acc; gated(c, G_4106e8_orig, &acc); }
