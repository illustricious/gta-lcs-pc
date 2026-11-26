/* LCS_PORT_TEST=1 together with LCS_RS_VERIFY=1: feed random records to Rust ports that a scripted session does not
 * reach (animation playback), once at start-up. Each call goes through the port, which runs the recompiled original on
 * the same memory and counts any difference ("engine: verified ..." in the log). Paths that call back into the game
 * (key frame lookup, delete, callbacks) are left out. */
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G_BASE 0x200000000ull

static uint8_t assoc[0x60], other[0x60], hier[0x30], hier2[0x30], node[0x30], seq[0x10], frames[16 * 16];
static uint8_t nodes[3 * 0x30];

static unsigned rnd(unsigned n) { return (unsigned)((((unsigned)rand() << 15) ^ (unsigned)rand()) % n); }
static float frnd(float lo, float hi) { return lo + (hi - lo) * (float)rnd(100000) / 100000.0f; }
static float pick(float lo, float hi) {
    unsigned k = rnd(12);
    return k == 0 ? 0.0f : k == 1 ? 1.0f : k == 2 ? hi : frnd(lo, hi);
}

static void fill_assoc(uint8_t *a, uint8_t *h) {
    memset(a, 0, 0x60);
    memset(h, 0, 0x30);
    float total = frnd(0.1f, 3.0f);
    MF32((uint64_t)h + 0x24) = total;
    M16((uint64_t)a + 0x10) = (uint16_t)(rnd(64) & ~4u);
    MF32((uint64_t)a + 0x14) = pick(0.0f, 0.2f);
    MF32((uint64_t)a + 0x18) = pick(-0.5f, 1.5f);
    M32((uint64_t)a + 0x20) = rnd(4);
    M64((uint64_t)a + 0x28) = (uint64_t)nodes; /* all without a sequence */
    M64((uint64_t)a + 0x30) = (uint64_t)h;
    MF32((uint64_t)a + 0x38) = pick(-8.0f, 8.0f);
    MF32((uint64_t)a + 0x3c) = rnd(8) == 0 ? total : pick(0.0f, total * 3.0f);
    MF32((uint64_t)a + 0x40) = pick(-1.0f, 3.0f) + 0.01f;
    M32((uint64_t)a + 0x48) = 0;
}

static void call(CPU *c, uint64_t entry, uint64_t x0, uint64_t x1, float s0, float s1) {
    CPU keep = *c;
    c->x[0] = x0;
    c->x[1] = x1;
    c->v[0].f[0] = s0;
    c->v[1].f[0] = s1;
    rt_call(c, G_BASE + entry);
    *c = keep;
}

void port_test(CPU *c) {
    static int done;
    if (done) return;
    done = 1;
    const char *e = getenv("LCS_PORT_TEST");
    if (!e || *e != '1') return;
    srand(1);
    for (int i = 0; i < 4000; i++) {
        memset(nodes, 0, sizeof nodes);
        fill_assoc(assoc, hier);
        fill_assoc(other, hier2);
        call(c, 0x2e91d4, (uint64_t)assoc, 0, pick(0.0f, 0.5f), 0);
        fill_assoc(assoc, hier);
        call(c, 0x2e973c, (uint64_t)assoc, 0, pick(0.0f, 1.0f), pick(0.0f, 2.0f));
        fill_assoc(assoc, hier);
        call(c, 0x2e6d64, (uint64_t)assoc, 0, pick(0.0f, 8.0f), 0);
        fill_assoc(assoc, hier);
        call(c, 0x2e6e0c, (uint64_t)assoc, (uint64_t)other, 0, 0);
        fill_assoc(assoc, hier);
        call(c, 0x2e6ed8, (uint64_t)assoc, 0, pick(0.0f, 8.0f), 0);
        call(c, 0x2e6f84, (uint64_t)assoc, 0, pick(-1.0f, 1.0f), pick(-8.0f, 8.0f));
        call(c, 0x2e6f98, (uint64_t)assoc, 0, pick(-1.0f, 1.0f), pick(-8.0f, 8.0f));
        /* key frame node: playing with time left, or stopped */
        fill_assoc(assoc, hier);
        memset(node, 0, sizeof node);
        M64((uint64_t)node + 0x28) = (uint64_t)assoc;
        MF32((uint64_t)assoc + 0x14) = frnd(0.0f, 0.2f); /* never past the frame while playing */
        MF32((uint64_t)node + 0x18) = rnd(2) ? frnd(0.5f, 2.0f) : 0.0f;
        if (MF32((uint64_t)node + 0x18) == 0.0f) M16((uint64_t)assoc + 0x10) &= ~1u;
        call(c, 0x2e762c, (uint64_t)node, 0, 0, 0);
        /* advance through packed key frames */
        fill_assoc(assoc, hier);
        int count = (int)rnd(8);
        uint16_t flags = rnd(2) ? 2 : 0;
        unsigned stride = flags ? 16 : 10;
        for (unsigned k = 0; k < 16; k++) M16((uint64_t)frames + k * stride % 240 + 8) = (uint16_t)(0x2c00 + rnd(0x1000));
        memset(seq, 0, sizeof seq);
        M16((uint64_t)seq) = flags;
        M16((uint64_t)seq + 2) = (uint16_t)count;
        M64((uint64_t)seq + 8) = (uint64_t)frames;
        memset(node, 0, sizeof node);
        M64((uint64_t)node + 8) = count > 0 ? rnd((unsigned)count) * stride : 0;
        MF32((uint64_t)node + 0x18) = pick(-3.0f, 0.5f);
        M64((uint64_t)node + 0x20) = (uint64_t)seq;
        M64((uint64_t)node + 0x28) = (uint64_t)assoc;
        call(c, 0x2e765c, (uint64_t)node, 0, 0, 0);
    }
    /* the per-frame skeleton update: a chain of associations on a clump without bones */
    {
        static uint8_t chain[12 * 0x60], hiers[12 * 0x30], group[0x40], frame[0xc0], sub[8], clump[0x30], bones[0x20];
        for (int i = 0; i < 4000; i++) {
            int n = (int)rnd(13);
            memset(group, 0, sizeof group); memset(frame, 0, sizeof frame); memset(sub, 0, sizeof sub);
            memset(clump, 0, sizeof clump); memset(bones, 0, sizeof bones);
            M64((uint64_t)group + 8) = (uint64_t)frame;
            M64((uint64_t)group + 0x30) = (uint64_t)clump;
            M64((uint64_t)frame + 0xb0) = (uint64_t)sub;
            M64((uint64_t)clump) = n ? (uint64_t)chain : 0;
            M64((uint64_t)clump + 0x20) = (uint64_t)bones;
            for (int k = 0; k < n; k++) {
                uint8_t *a = chain + k * 0x60, *h = hiers + k * 0x30;
                fill_assoc(a, h);
                M64((uint64_t)a) = k + 1 < n ? (uint64_t)(a + 0x60) : 0;
                M64((uint64_t)h) = rnd(5) ? 1 : 0; /* without it the association takes no part */
                if (!rnd(4)) M16((uint64_t)a + 0x10) |= 0x21;
            }
            call(c, 0x2e8e20, (uint64_t)group, 0, pick(-0.1f, 0.5f), 0);
        }
    }
    rt_log("porttest: 4000 rounds of the animation ports done\n");
}
