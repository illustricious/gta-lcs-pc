/* Guest CPU state and instruction helpers shared by all statically
 * recompiled ARM64 code.  Guest pointers are host pointers: each module is
 * mapped at its fixed link base, so no address translation takes place. */
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef union V128 {
    uint8_t b[16];
    int8_t sb[16];
    uint16_t h[8];
    int16_t sh[8];
    uint32_t s[4];
    int32_t ss[4];
    uint64_t d[2];
    int64_t sd[2];
    float f[4];
    double df[2];
} V128;

#define RT_TRACE_DEPTH 64

typedef struct CPU {
    uint64_t x[32]; /* x[31] is SP */
    V128 v[32];
    uint32_t nf, zf, cf, vf;
    uint64_t tpidr;
    uint64_t excl_addr, excl_val;
    uint64_t trace[RT_TRACE_DEPTH]; /* ring of recently entered guest functions */
    uint32_t trace_pos;
    uint32_t depth;
    void *host; /* per-thread runtime data */
} CPU;

typedef void (*RtFunc)(CPU *);
typedef struct RtEntry {
    uint64_t addr;
    RtFunc fn;
} RtEntry;
typedef struct RtImport {
    const char *name;
    RtFunc fn;
} RtImport;

void rt_call(CPU *c, uint64_t target);
__declspec(noreturn) void rt_trap(CPU *c, uint64_t pc, const char *why);

#ifdef RT_NO_TRACE
#define RT_ENTER(a) ((void)0)
#else
#define RT_ENTER(a) (c->trace[c->trace_pos++ & (RT_TRACE_DEPTH - 1)] = (a))
#endif

typedef uint8_t __attribute__((may_alias)) u8u;
typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef int8_t __attribute__((may_alias)) s8u;
typedef int16_t __attribute__((aligned(1), may_alias)) s16u;
typedef int32_t __attribute__((aligned(1), may_alias)) s32u;
typedef int64_t __attribute__((aligned(1), may_alias)) s64u;
typedef float __attribute__((aligned(1), may_alias)) f32u;
typedef double __attribute__((aligned(1), may_alias)) f64u;
typedef V128 __attribute__((aligned(1), may_alias)) v128u;

#define M8(a) (*(u8u *)(a))
#define M16(a) (*(u16u *)(a))
#define M32(a) (*(u32u *)(a))
#define M64(a) (*(u64u *)(a))
#define MS8(a) (*(s8u *)(a))
#define MS16(a) (*(s16u *)(a))
#define MS32(a) (*(s32u *)(a))
#define MS64(a) (*(s64u *)(a))
#define MF32(a) (*(f32u *)(a))
#define MF64(a) (*(f64u *)(a))
#define M128(a) (*(v128u *)(a))

#define X(n) (c->x[n])
#define SP (c->x[31])
#define V(n) (c->v[n])

#define C_EQ (c->zf)
#define C_NE (!c->zf)
#define C_HS (c->cf)
#define C_LO (!c->cf)
#define C_MI (c->nf)
#define C_PL (!c->nf)
#define C_VS (c->vf)
#define C_VC (!c->vf)
#define C_HI (c->cf && !c->zf)
#define C_LS (!c->cf || c->zf)
#define C_GE (c->nf == c->vf)
#define C_LT (c->nf != c->vf)
#define C_GT (!c->zf && c->nf == c->vf)
#define C_LE (c->zf || c->nf != c->vf)
#define C_AL 1

#define SET_Q(n, val) (c->v[n] = (val))
#define SET_S(n, val) do { float m_ = (val); c->v[n].d[0] = 0; c->v[n].d[1] = 0; c->v[n].f[0] = m_; } while (0)
#define SET_D(n, val) do { double m_ = (val); c->v[n].d[1] = 0; c->v[n].df[0] = m_; } while (0)
#define SET_SU(n, val) do { uint32_t m_ = (uint32_t)(val); c->v[n].d[1] = 0; c->v[n].d[0] = m_; } while (0)
#define SET_DU(n, val) do { uint64_t m_ = (uint64_t)(val); c->v[n].d[1] = 0; c->v[n].d[0] = m_; } while (0)
#define SET_HU(n, val) do { uint16_t m_ = (uint16_t)(val); c->v[n].d[1] = 0; c->v[n].d[0] = m_; } while (0)
#define SET_BU(n, val) do { uint8_t m_ = (uint8_t)(val); c->v[n].d[1] = 0; c->v[n].d[0] = m_; } while (0)

static inline void set_nzcv(CPU *c, unsigned f) {
    c->nf = (f >> 3) & 1; c->zf = (f >> 2) & 1; c->cf = (f >> 1) & 1; c->vf = f & 1;
}
static inline uint64_t adds64(CPU *c, uint64_t a, uint64_t b) {
    uint64_t r = a + b;
    c->nf = (uint32_t)(r >> 63); c->zf = r == 0; c->cf = r < a;
    c->vf = (uint32_t)(((~(a ^ b)) & (a ^ r)) >> 63);
    return r;
}
static inline uint32_t adds32(CPU *c, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    c->nf = r >> 31; c->zf = r == 0; c->cf = r < a;
    c->vf = ((~(a ^ b)) & (a ^ r)) >> 31;
    return r;
}
static inline uint64_t subs64(CPU *c, uint64_t a, uint64_t b) {
    uint64_t r = a - b;
    c->nf = (uint32_t)(r >> 63); c->zf = r == 0; c->cf = a >= b;
    c->vf = (uint32_t)(((a ^ b) & (a ^ r)) >> 63);
    return r;
}
static inline uint32_t subs32(CPU *c, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    c->nf = r >> 31; c->zf = r == 0; c->cf = a >= b;
    c->vf = ((a ^ b) & (a ^ r)) >> 31;
    return r;
}
static inline uint64_t adcs64(CPU *c, uint64_t a, uint64_t b, int set) {
    uint64_t t = a + b, r = t + c->cf;
    if (set) {
        uint32_t carry = (t < a) | (r < t);
        c->vf = (uint32_t)(((~(a ^ b)) & (a ^ r)) >> 63);
        c->nf = (uint32_t)(r >> 63); c->zf = r == 0; c->cf = carry;
    }
    return r;
}
static inline uint32_t adcs32(CPU *c, uint32_t a, uint32_t b, int set) {
    uint64_t w = (uint64_t)a + b + c->cf;
    uint32_t r = (uint32_t)w;
    if (set) {
        c->vf = ((~(a ^ b)) & (a ^ r)) >> 31;
        c->nf = r >> 31; c->zf = r == 0; c->cf = (uint32_t)(w >> 32);
    }
    return r;
}
static inline uint64_t logs64(CPU *c, uint64_t r) {
    c->nf = (uint32_t)(r >> 63); c->zf = r == 0; c->cf = 0; c->vf = 0;
    return r;
}
static inline uint32_t logs32(CPU *c, uint32_t r) {
    c->nf = r >> 31; c->zf = r == 0; c->cf = 0; c->vf = 0;
    return r;
}
static inline void fcmp64(CPU *c, double a, double b) {
    if (a != a || b != b) set_nzcv(c, 3);
    else if (a == b) set_nzcv(c, 6);
    else if (a < b) set_nzcv(c, 8);
    else set_nzcv(c, 2);
}
#define fcmp32(c, a, b) fcmp64(c, (double)(a), (double)(b))

static inline uint32_t ror32(uint32_t v, unsigned n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
static inline uint64_t ror64(uint64_t v, unsigned n) { n &= 63; return n ? (v >> n) | (v << (64 - n)) : v; }
static inline uint32_t rbit32(uint32_t v) { return __builtin_bitreverse32(v); }
static inline uint64_t rbit64(uint64_t v) { return __builtin_bitreverse64(v); }
static inline uint32_t clz32(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }
static inline uint64_t clz64(uint64_t v) { return v ? (uint64_t)__builtin_clzll(v) : 64; }
static inline uint32_t rev16_32(uint32_t v) { return ((v & 0xff00ff00u) >> 8) | ((v & 0x00ff00ffu) << 8); }
static inline uint64_t rev16_64(uint64_t v) { return ((v & 0xff00ff00ff00ff00ull) >> 8) | ((v & 0x00ff00ff00ff00ffull) << 8); }
static inline uint64_t rev32_64(uint64_t v) { return ((uint64_t)__builtin_bswap32((uint32_t)(v >> 32)) << 32) | __builtin_bswap32((uint32_t)v); }
static inline uint64_t umulh64(uint64_t a, uint64_t b) { return (uint64_t)(((unsigned __int128)a * b) >> 64); }
static inline uint64_t smulh64(uint64_t a, uint64_t b) { return (uint64_t)(((__int128)(int64_t)a * (int64_t)b) >> 64); }
static inline uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
static inline uint32_t sdiv32(uint32_t a, uint32_t b) {
    if (!b) return 0;
    if (a == 0x80000000u && b == 0xffffffffu) return a;
    return (uint32_t)((int32_t)a / (int32_t)b);
}
static inline uint64_t sdiv64(uint64_t a, uint64_t b) {
    if (!b) return 0;
    if (a == 0x8000000000000000ull && b == ~0ull) return a;
    return (uint64_t)((int64_t)a / (int64_t)b);
}

/* Float to integer with ARM saturation; the caller applies the rounding. */
static inline uint32_t cvt_s32(double v) {
    if (v != v) return 0;
    if (v >= 2147483648.0) return 0x7fffffffu;
    if (v <= -2147483649.0) return 0x80000000u;
    return (uint32_t)(int32_t)v;
}
static inline uint32_t cvt_u32(double v) {
    if (!(v > -1.0)) return 0;
    if (v >= 4294967296.0) return 0xffffffffu;
    return (uint32_t)v;
}
static inline uint64_t cvt_s64(double v) {
    if (v != v) return 0;
    if (v >= 9223372036854775808.0) return 0x7fffffffffffffffull;
    if (v < -9223372036854775808.0) return 0x8000000000000000ull;
    return (uint64_t)(int64_t)v;
}
static inline uint64_t cvt_u64(double v) {
    if (!(v > -1.0)) return 0;
    if (v >= 18446744073709551616.0) return ~0ull;
    return (uint64_t)v;
}
static inline double rint_even(double v) { return nearbyint(v); }

static inline float fmax32(float a, float b) {
    if (a != a || b != b) return a + b;
    if (a == 0 && b == 0) return signbit(a) ? b : a;
    return a > b ? a : b;
}
static inline float fmin32(float a, float b) {
    if (a != a || b != b) return a + b;
    if (a == 0 && b == 0) return signbit(a) ? a : b;
    return a < b ? a : b;
}
static inline double fmax64(double a, double b) {
    if (a != a || b != b) return a + b;
    if (a == 0 && b == 0) return signbit(a) ? b : a;
    return a > b ? a : b;
}
static inline double fmin64(double a, double b) {
    if (a != a || b != b) return a + b;
    if (a == 0 && b == 0) return signbit(a) ? a : b;
    return a < b ? a : b;
}
static inline float fmaxnm32(float a, float b) { return a != a ? b : b != b ? a : fmax32(a, b); }
static inline float fminnm32(float a, float b) { return a != a ? b : b != b ? a : fmin32(a, b); }
static inline double fmaxnm64(double a, double b) { return a != a ? b : b != b ? a : fmax64(a, b); }
static inline double fminnm64(double a, double b) { return a != a ? b : b != b ? a : fmin64(a, b); }

/* Element shifts by a signed per-element count (SSHL/USHL). */
static inline int64_t sshl_e(int64_t v, int8_t sh, int bits) {
    if (sh >= bits) return 0;
    if (sh >= 0) return (int64_t)((uint64_t)v << sh);
    if (-sh >= bits) return v < 0 ? -1 : 0;
    return v >> -sh;
}
static inline uint64_t ushl_e(uint64_t v, int8_t sh, int bits) {
    if (sh >= bits || -sh >= bits) return 0;
    return sh >= 0 ? v << sh : v >> -sh;
}
