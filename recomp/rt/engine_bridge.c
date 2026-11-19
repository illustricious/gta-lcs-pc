/* Host side of the Rust engine crate (recomp/engine): layout check, settings, verification log. */
#include "rt.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>

/* recomp/engine/src/cpu.rs mirrors struct CPU */
_Static_assert(sizeof(CPU) == 1336, "struct CPU changed: update recomp/engine/src/cpu.rs");

void lcs_set_far_lod_scale(float scale);
void lcs_engine_verify(int on);
void lcs_engine_verify_counts(uint64_t *out);

__declspec(noreturn) void rt_engine_panic(void) { rt_fatal("Rust engine panic"); }

void rt_engine_mismatch(uintptr_t original, uint64_t ours, uint64_t theirs) {
    static int shown;
    /* the function as an address in lcs.map (image base 0x140000000) */
    extern __declspec(dllimport) void *__stdcall GetModuleHandleA(const char *);
    uintptr_t at = original - (uintptr_t)GetModuleHandleA(0) + 0x140000000u;
    if (shown++ < 20) rt_log("engine: MISMATCH in %llx (lcs.map): rust %llx, original %llx\n", (unsigned long long)at,
                             (unsigned long long)ours, (unsigned long long)theirs);
}

void engine_init(void) {
    const char *v = getenv("LCS_RS_VERIFY");
    if (v && *v == '1') { lcs_engine_verify(1); rt_log("engine: verifying Rust ports against the originals\n"); }
}

void engine_set_far_lod_scale(float k) { lcs_set_far_lod_scale(k); }

/* once a second while verifying */
void engine_report(void) {
    static unsigned frames;
    uint64_t n[2];
    if (++frames % 60) return;
    lcs_engine_verify_counts(n);
    if (n[0]) rt_log("engine: verified %llu calls, %llu mismatches\n", (unsigned long long)n[0], (unsigned long long)n[1]);
}

/* CalcScreenSize (world mesh cull by screen size, cWorldStreamEx::DrawPrimitiveRef): counts for the
 * verbose diagnostics, how many world meshes are kept and culled. */
extern void G_563238_orig(CPU *c);
static unsigned cull_kept, cull_dropped;
void G_563238(CPU *c) {
    G_563238_orig(c);
    if (c->x[0] & 1) cull_kept++; else cull_dropped++;
}
void engine_cull_stats(unsigned *kept, unsigned *dropped) {
    *kept = cull_kept; *dropped = cull_dropped;
    cull_kept = cull_dropped = 0;
}
