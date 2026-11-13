/* Bionic libc / libm / pthread imports implemented on Windows.  These are
 * operating-system services only: memory, files, time, threads, formatting. */
#include "rt.h"
#include "platform.h"
#include <ctype.h>
#include <direct.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <wctype.h>
#include <windows.h>

/* ------------------------------------------------------------ variadics */

ArgSrc rt_args_regs(CPU *c, int first_gr, int first_vr) {
    ArgSrc a = {&c->x[first_gr], 8 - first_gr, &c->v[first_vr], 8 - first_vr, (uint64_t *)c->x[31]};
    return a;
}

ArgSrc rt_args_valist(uint64_t va) {
    /* AAPCS64 va_list: { void *stack; void *gr_top; void *vr_top; int gr_offs; int vr_offs; } */
    int32_t gr_offs = MS32(va + 24), vr_offs = MS32(va + 28);
    ArgSrc a;
    a.stack = (uint64_t *)M64(va);
    a.gr = (uint64_t *)(M64(va + 8) + gr_offs);
    a.gr_left = gr_offs < 0 ? -gr_offs / 8 : 0;
    a.vr = (V128 *)(M64(va + 16) + vr_offs);
    a.vr_left = vr_offs < 0 ? -vr_offs / 16 : 0;
    return a;
}

uint64_t rt_arg_int(ArgSrc *a) {
    if (a->gr_left > 0) { a->gr_left--; return *a->gr++; }
    return *a->stack++;
}

double rt_arg_double(ArgSrc *a) {
    if (a->vr_left > 0) { a->vr_left--; return (a->vr++)->df[0]; }
    double d;
    memcpy(&d, a->stack++, 8);
    return d;
}

int rt_vformat(char *out, size_t cap, const char *fmt, ArgSrc *a) {
    size_t n = 0;
#define PUT(ch) do { char ch_ = (ch); if (n + 1 < cap) out[n] = ch_; n++; } while (0)
    while (*fmt) {
        if (*fmt != '%') { PUT(*fmt++); continue; }
        fmt++;
        if (*fmt == '%') { PUT('%'); fmt++; continue; }
        char spec[80];
        int si = 0;
        spec[si++] = '%';
        while (*fmt && strchr("-+ #0'", *fmt) && si < 20) spec[si++] = *fmt++;
        if (*fmt == '*') { si += sprintf(spec + si, "%d", (int)rt_arg_int(a)); fmt++; }
        else while (isdigit((unsigned char)*fmt) && si < 40) spec[si++] = *fmt++;
        if (*fmt == '.') {
            spec[si++] = *fmt++;
            if (*fmt == '*') { si += sprintf(spec + si, "%d", (int)rt_arg_int(a)); fmt++; }
            else while (isdigit((unsigned char)*fmt) && si < 60) spec[si++] = *fmt++;
        }
        int lng = 0;
        for (;; fmt++) {
            if (*fmt == 'l') lng++;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't' || *fmt == 'q') lng = 2;
            else if (*fmt == 'h') lng--;
            else if (*fmt != 'L') break;
        }
        char conv = *fmt ? *fmt++ : 0;
        char tmp[512];
        char *text = tmp;
        char *heap = 0;
        tmp[0] = 0;
        switch (conv) {
        case 'd': case 'i': {
            int64_t v = (int64_t)rt_arg_int(a);
            if (lng <= 0) v = (int32_t)v;
            if (lng == -1) v = (int16_t)v;
            if (lng <= -2) v = (int8_t)v;
            strcpy(spec + si, "lld");
            snprintf(tmp, sizeof tmp, spec, (long long)v);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = rt_arg_int(a);
            if (lng <= 0) v = (uint32_t)v;
            if (lng == -1) v = (uint16_t)v;
            if (lng <= -2) v = (uint8_t)v;
            spec[si++] = 'l'; spec[si++] = 'l'; spec[si++] = conv; spec[si] = 0;
            snprintf(tmp, sizeof tmp, spec, (unsigned long long)v);
            break;
        }
        case 'c':
            spec[si++] = 'c'; spec[si] = 0;
            snprintf(tmp, sizeof tmp, spec, (int)rt_arg_int(a));
            break;
        case 'p':
            snprintf(tmp, sizeof tmp, "0x%llx", (unsigned long long)rt_arg_int(a));
            break;
        case 's': {
            const char *s = (const char *)rt_arg_int(a);
            if (!s) s = "(null)";
            if (lng > 0) s = "(wide)";
            spec[si++] = 's'; spec[si] = 0;
            int need = snprintf(0, 0, spec, s);
            if (need >= (int)sizeof tmp) text = heap = malloc(need + 1);
            snprintf(text, need + 1, spec, s);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            spec[si++] = conv; spec[si] = 0;
            double v = rt_arg_double(a);
            int need = snprintf(0, 0, spec, v);
            if (need >= (int)sizeof tmp) text = heap = malloc(need + 1);
            snprintf(text, need + 1, spec, v);
            break;
        }
        case 'n':
            rt_arg_int(a);
            break;
        default:
            PUT('%');
            if (conv) PUT(conv);
            break;
        }
        for (char *p = text; *p; p++) PUT(*p);
        free(heap);
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
#undef PUT
    return (int)n;
}

static char *format_alloc(const char *fmt, ArgSrc *a, int *len) {
    ArgSrc copy = *a;
    int n = rt_vformat(0, 0, fmt, &copy);
    char *buf = malloc(n + 1);
    rt_vformat(buf, n + 1, fmt, a);
    if (len) *len = n;
    return buf;
}

/* Translate a guest scanf format (LP64: long is 64-bit) to the host's and count its outputs. */
static int scan_format(const char *fmt, char *out, size_t cap) {
    size_t n = 0;
    int count = 0;
    while (*fmt && n + 8 < cap) {
        if (*fmt != '%') { out[n++] = *fmt++; continue; }
        out[n++] = *fmt++;
        if (*fmt == '%') { out[n++] = *fmt++; continue; }
        int suppress = 0;
        if (*fmt == '*') { suppress = 1; out[n++] = *fmt++; }
        while (isdigit((unsigned char)*fmt)) out[n++] = *fmt++;
        int lng = 0, half = 0;
        for (;; fmt++) {
            if (*fmt == 'l') lng++;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') lng = 2;
            else if (*fmt == 'h') half++;
            else if (*fmt != 'L') break;
        }
        char conv = *fmt++;
        if (strchr("diuxXon", conv)) {
            for (int i = 0; i < half; i++) out[n++] = 'h';
            if (lng) { out[n++] = 'l'; out[n++] = 'l'; }
        } else if (lng && strchr("feEgGaA", conv)) {
            out[n++] = 'l';
        }
        out[n++] = conv;
        if (conv == '[') {
            if (*fmt == '^') out[n++] = *fmt++;
            if (*fmt == ']') out[n++] = *fmt++;
            while (*fmt && *fmt != ']' && n + 8 < cap) out[n++] = *fmt++;
            if (*fmt) out[n++] = *fmt++;
        }
        if (!suppress) count++;
    }
    out[n] = 0;
    return count;
}

static int do_sscanf(const char *str, const char *fmt, ArgSrc *a) {
    char hostfmt[2048];
    void *p[64] = {0};
    int count = scan_format(fmt, hostfmt, sizeof hostfmt);
    if (count > 64) rt_fatal("sscanf with %d outputs", count);
    for (int i = 0; i < count; i++) p[i] = (void *)rt_arg_int(a);
    return sscanf(str, hostfmt, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15], p[16], p[17], p[18], p[19], p[20], p[21], p[22], p[23], p[24], p[25], p[26], p[27], p[28], p[29], p[30], p[31], p[32], p[33], p[34], p[35], p[36], p[37], p[38], p[39], p[40], p[41], p[42], p[43], p[44], p[45], p[46], p[47], p[48], p[49], p[50], p[51], p[52], p[53], p[54], p[55], p[56], p[57], p[58], p[59], p[60], p[61], p[62], p[63]);
}

/* ------------------------------------------------------------ memory */

static void *g_malloc(size_t n) { return _aligned_malloc(n ? n : 1, 16); }

IMP(malloc) { RET(g_malloc(A(0))); }
IMP(calloc) {
    size_t n = A(0) * A(1);
    void *p = g_malloc(n);
    if (p) memset(p, 0, n);
    RET(p);
}
IMP(realloc) { RET(_aligned_realloc(P(0), A(1) ? A(1) : 1, 16)); }
IMP(free) { _aligned_free(P(0)); }
IMP(posix_memalign) {
    void *p = _aligned_malloc(A(2) ? A(2) : 1, A(1) < 16 ? 16 : A(1));
    M64(A(0)) = (uint64_t)p;
    RET(p ? 0 : 12);
}

IMP(memcpy) { RET(memcpy(P(0), P(1), A(2))); }
IMP(__memcpy_chk) { RET(memcpy(P(0), P(1), A(2))); }
IMP(memmove) { RET(memmove(P(0), P(1), A(2))); }
IMP(__memmove_chk) { RET(memmove(P(0), P(1), A(2))); }
IMP(memset) { RET(memset(P(0), I(1), A(2))); }
IMP(__memset_chk) { RET(memset(P(0), I(1), A(2))); }
IMP(memcmp) { RETI(memcmp(P(0), P(1), A(2))); }
IMP(memchr) { RET(memchr(P(0), I(1), A(2))); }
IMP(strlen) { RET(strlen(S(0))); }
IMP(__strlen_chk) { RET(strlen(S(0))); }
IMP(strcmp) { RETI(strcmp(S(0), S(1))); }
IMP(strncmp) { RETI(strncmp(S(0), S(1), A(2))); }
IMP(strcasecmp) { RETI(_stricmp(S(0), S(1))); }
IMP(strncasecmp) { RETI(_strnicmp(S(0), S(1), A(2))); }
IMP(strcpy) { RET(strcpy(P(0), S(1))); }
IMP(__strcpy_chk) { RET(strcpy(P(0), S(1))); }
IMP(strncpy) { RET(strncpy(P(0), S(1), A(2))); }
IMP(__strncpy_chk) { RET(strncpy(P(0), S(1), A(2))); }
IMP(__strncpy_chk2) { RET(strncpy(P(0), S(1), A(2))); }
IMP(strcat) { RET(strcat(P(0), S(1))); }
IMP(__strcat_chk) { RET(strcat(P(0), S(1))); }
IMP(strchr) { RET(strchr(S(0), I(1))); }
IMP(__strchr_chk) { RET(strchr(S(0), I(1))); }
IMP(strrchr) { RET(strrchr(S(0), I(1))); }
IMP(__strrchr_chk) { RET(strrchr(S(0), I(1))); }
IMP(strstr) { RET(strstr(S(0), S(1))); }
IMP(strtok) { RET(strtok(P(0), S(1))); }
IMP(strerror) { RET(strerror(I(0))); }
IMP(strerror_r) { strncpy(P(1), strerror(I(0)), A(2)); RET(0); }
IMP(strdup) {
    size_t n = strlen(S(0)) + 1;
    RET(memcpy(g_malloc(n), S(0), n));
}
IMP(atoi) { RETI(atoi(S(0))); }
IMP(atof) { RETD(atof(S(0))); }
IMP(strtod) { RETD(strtod(S(0), (char **)P(1))); }
IMP(strtof) { RETF(strtof(S(0), (char **)P(1))); }
IMP(strtol) { RET(strtoll(S(0), (char **)P(1), I(2))); }
IMP(strtoll) { RET(strtoll(S(0), (char **)P(1), I(2))); }
IMP(strtoll_l) { RET(strtoll(S(0), (char **)P(1), I(2))); }
IMP(strtoul) { RET(strtoull(S(0), (char **)P(1), I(2))); }
IMP(strtoull) { RET(strtoull(S(0), (char **)P(1), I(2))); }
IMP(strtoull_l) { RET(strtoull(S(0), (char **)P(1), I(2))); }

typedef struct { CPU *c; uint64_t cmp; } SortCtx;
static int guest_compare(void *ctx, const void *a, const void *b) {
    SortCtx *s = ctx;
    return (int32_t)rt_invoke(s->c, s->cmp, 2, (uint64_t)a, (uint64_t)b);
}
IMP(qsort) {
    SortCtx s = {c, A(3)};
    qsort_s(P(0), A(1), A(2), guest_compare, &s);
}
IMP(bsearch) {
    SortCtx s = {c, A(4)};
    void *r = bsearch_s(P(0), P(1), A(2), A(3), guest_compare, &s);
    RET(r);
}
IMP(rand) { RETI(rand()); }
IMP(srand) { srand((unsigned)A(0)); }
IMP(abort) { rt_fatal("guest called abort()"); }
IMP(raise) { rt_fatal("guest called raise(%d)", I(0)); }
IMP(__stack_chk_fail) { rt_fatal("guest stack protector failure"); }
IMP(__assert2) { rt_fatal("guest assertion failed: %s:%d %s: %s", S(0), I(1), S(2), S(3)); }
IMP(android_set_abort_message) { rt_log("abort message: %s\n", S(0)); }
IMP(getenv) {
    /* --no-audio selects OpenAL Soft's own silent backend. */
    if (plat.no_audio && !strcmp(S(0), "ALSOFT_DRIVERS")) { RET("null"); return; }
    RET(0);
}
IMP(getauxval) { RET(0); }
IMP(__system_property_get) { M8(A(1)) = 0; RET(0); }
IMP(dl_iterate_phdr) { RET(0); }
IMP(__register_atfork) { RET(0); }
IMP(__cxa_atexit) { RET(0); }
IMP(__cxa_finalize) { }
IMP(__cxa_thread_atexit_impl) { RET(0); }
IMP(sysconf) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    switch (I(0)) {
    case 39: case 40: RET(4096); break;                   /* _SC_PAGESIZE / _SC_PAGE_SIZE */
    case 96: case 97: RET(si.dwNumberOfProcessors); break; /* _SC_NPROCESSORS_CONF / _ONLN */
    case 98: RET(1u << 20); break;                        /* _SC_PHYS_PAGES */
    default: RET(-1); break;
    }
}
IMP(syscall) {
    if (A(0) == 178) RET(GetCurrentThreadId());
    else RET(-1);
}
IMP(getcwd) { RET(_getcwd(P(0), (int)A(1))); }

/* ------------------------------------------------------------ ctype / locale */

static char ctype_table[257];
static const char *ctype_ptr = ctype_table;
static void *lconv_block[24];

static void init_ctype(void) {
    for (int ch = 0; ch < 128; ch++) {
        char f = 0;
        if (isupper(ch)) f |= 0x01;
        if (islower(ch)) f |= 0x02;
        if (isdigit(ch)) f |= 0x04;
        if (isspace(ch)) f |= 0x08;
        if (ispunct(ch)) f |= 0x10;
        if (iscntrl(ch)) f |= 0x20;
        if (isxdigit(ch) && !isdigit(ch)) f |= 0x40;
        if (ch == ' ') f |= 0x80;
        ctype_table[ch + 1] = f;
    }
    for (int i = 0; i < 10; i++) lconv_block[i] = "";
    lconv_block[0] = ".";
}

IMP(__ctype_get_mb_cur_max) { RET(1); }
IMP(newlocale) { RET(lconv_block); }
IMP(uselocale) { RET(lconv_block); }
IMP(freelocale) { }
IMP(setlocale) { RET("C"); }
IMP(localeconv) { RET(lconv_block); }
IMP(isdigit_l) { RETI(isdigit(I(0) & 255) != 0); }
IMP(isxdigit_l) { RETI(isxdigit(I(0) & 255) != 0); }
IMP(islower_l) { RETI(islower(I(0) & 255) != 0); }
IMP(isupper_l) { RETI(isupper(I(0) & 255) != 0); }
IMP(tolower_l) { RETI(tolower(I(0))); }
IMP(toupper_l) { RETI(toupper(I(0))); }
IMP(iswalpha_l) { RETI(iswalpha((wint_t)A(0)) != 0); }
IMP(iswblank_l) { RETI(A(0) == ' ' || A(0) == '\t'); }
IMP(iswcntrl_l) { RETI(iswcntrl((wint_t)A(0)) != 0); }
IMP(iswdigit_l) { RETI(iswdigit((wint_t)A(0)) != 0); }
IMP(iswlower_l) { RETI(iswlower((wint_t)A(0)) != 0); }
IMP(iswprint_l) { RETI(iswprint((wint_t)A(0)) != 0); }
IMP(iswpunct_l) { RETI(iswpunct((wint_t)A(0)) != 0); }
IMP(iswspace_l) { RETI(iswspace((wint_t)A(0)) != 0); }
IMP(iswupper_l) { RETI(iswupper((wint_t)A(0)) != 0); }
IMP(iswxdigit_l) { RETI(iswxdigit((wint_t)A(0)) != 0); }
IMP(towlower_l) { RETI(towlower((wint_t)A(0))); }
IMP(towupper_l) { RETI(towupper((wint_t)A(0))); }
IMP(strcoll_l) { RETI(strcmp(S(0), S(1))); }
IMP(strxfrm_l) {
    size_t n = strlen(S(1));
    if (A(2)) strncpy(P(0), S(1), A(2));
    RET(n);
}
/* wchar_t is 32-bit in the guest */
IMP(wcslen) { uint32_t *p = P(0); size_t n = 0; while (p[n]) n++; RET(n); }
IMP(wmemcpy) { RET(memcpy(P(0), P(1), A(2) * 4)); }
IMP(wmemmove) { RET(memmove(P(0), P(1), A(2) * 4)); }
IMP(wmemset) { uint32_t *p = P(0); for (size_t i = 0; i < A(2); i++) p[i] = (uint32_t)A(1); RET(p); }
IMP(wmemcmp) {
    uint32_t *a = P(0), *b = P(1);
    for (size_t i = 0; i < A(2); i++)
        if (a[i] != b[i]) { RETI(a[i] < b[i] ? -1 : 1); return; }
    RETI(0);
}
IMP(wmemchr) {
    uint32_t *a = P(0);
    for (size_t i = 0; i < A(2); i++)
        if (a[i] == (uint32_t)A(1)) { RET(a + i); return; }
    RET(0);
}

/* ------------------------------------------------------------ math */

IMP(sinf) { RETF(sinf(FA(0))); }
IMP(cosf) { RETF(cosf(FA(0))); }
IMP(tanf) { RETF(tanf(FA(0))); }
IMP(asinf) { RETF(asinf(FA(0))); }
IMP(acosf) { RETF(acosf(FA(0))); }
IMP(atanf) { RETF(atanf(FA(0))); }
IMP(atan2f) { RETF(atan2f(FA(0), FA(1))); }
IMP(sinhf) { RETF(sinhf(FA(0))); }
IMP(expf) { RETF(expf(FA(0))); }
IMP(exp2f) { RETF(exp2f(FA(0))); }
IMP(logf) { RETF(logf(FA(0))); }
IMP(log2f) { RETF(log2f(FA(0))); }
IMP(log10f) { RETF(log10f(FA(0))); }
IMP(powf) { RETF(powf(FA(0), FA(1))); }
IMP(fmodf) { RETF(fmodf(FA(0), FA(1))); }
IMP(cbrtf) { RETF(cbrtf(FA(0))); }
IMP(ldexpf) { RETF(ldexpf(FA(0), I(0))); }
IMP(sin) { RETD(sin(DA(0))); }
IMP(cos) { RETD(cos(DA(0))); }
IMP(tan) { RETD(tan(DA(0))); }
IMP(atan2) { RETD(atan2(DA(0), DA(1))); }
IMP(hypot) { RETD(hypot(DA(0), DA(1))); }
IMP(pow) { RETD(pow(DA(0), DA(1))); }
IMP(log) { RETD(log(DA(0))); }
IMP(exp2) { RETD(exp2(DA(0))); }
IMP(modf) { double ip; double r = modf(DA(0), &ip); MF64(A(0)) = ip; RETD(r); }
IMP(frexp) { int e; double r = frexp(DA(0), &e); MS32(A(0)) = e; RETD(r); }
IMP(sincosf) { float v = FA(0); MF32(A(0)) = sinf(v); MF32(A(1)) = cosf(v); }
IMP(sincos) { double v = DA(0); MF64(A(0)) = sin(v); MF64(A(1)) = cos(v); }

/* ------------------------------------------------------------ time */

static double qpc_scale;
static int64_t unix_100ns(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ll;
}
static int64_t mono_ns(void) {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (t.QuadPart / f.QuadPart) * 1000000000 + (t.QuadPart % f.QuadPart) * 1000000000 / f.QuadPart;
}
IMP(clock_gettime) {
    int64_t ns = I(0) == 0 ? unix_100ns() * 100 : mono_ns();
    MS64(A(1)) = ns / 1000000000;
    MS64(A(1) + 8) = ns % 1000000000;
    RET(0);
}
IMP(gettimeofday) {
    int64_t t = unix_100ns() / 10;
    if (A(0)) { MS64(A(0)) = t / 1000000; MS64(A(0) + 8) = t % 1000000; }
    RET(0);
}
IMP(time) {
    int64_t t = unix_100ns() / 10000000;
    if (A(0)) MS64(A(0)) = t;
    RET(t);
}
static void sleep_ns(int64_t ns) {
    if (ns <= 0) { SwitchToThread(); return; }
    static __thread HANDLE timer;
    if (!timer) timer = CreateWaitableTimerExW(0, 0, 2 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -(ns / 100 ? ns / 100 : 1);
    if (timer && SetWaitableTimer(timer, &due, 0, 0, 0, 0)) WaitForSingleObject(timer, INFINITE);
    else Sleep((DWORD)(ns / 1000000));
}
IMP(nanosleep) { sleep_ns(MS64(A(0)) * 1000000000 + MS64(A(0) + 8)); RET(0); }
IMP(usleep) { sleep_ns((int64_t)A(0) * 1000); RET(0); }
IMP(sched_yield) { SwitchToThread(); RET(0); }
static int32_t guest_tm[14];
IMP(gmtime) {
    time_t t = (time_t)MS64(A(0));
    struct tm *tm = gmtime(&t);
    memset(guest_tm, 0, sizeof guest_tm);
    if (tm) memcpy(guest_tm, tm, 9 * sizeof(int));
    RET(guest_tm);
}
IMP(mktime) {
    struct tm tm;
    memcpy(&tm, P(0), 9 * sizeof(int));
    RET((int64_t)mktime(&tm));
}

/* ------------------------------------------------------------ stdio */

static uint8_t guest_sF[3 * 152];
static uint64_t guest_stdout = (uint64_t)(guest_sF + 152), guest_stderr = (uint64_t)(guest_sF + 304);
static __thread int guest_errno;

void *rt_host_file(uint64_t g) {
    uint64_t off = g - (uint64_t)guest_sF;
    if (off < sizeof guest_sF) return off < 152 ? stdin : off < 304 ? stdout : stderr;
    return (void *)g;
}
#define HF(n) ((FILE *)rt_host_file(A(n)))

IMP(__errno) { RET(&guest_errno); }
IMP(fopen) {
    char mode[8];
    int k = 0;
    for (const char *m = S(1); *m && k < 4; m++)
        if (strchr("rwa+", *m)) mode[k++] = *m; /* drop Linux-only flags such as 'e' */
    mode[k++] = 'b';
    mode[k] = 0;
    FILE *f = fopen(S(0), mode);
    if (rt_verbose) rt_log("fopen(%s, %s) -> %s\n", S(0), mode, f ? "ok" : "failed");
    if (!f) guest_errno = 2;
    RET(f);
}
IMP(fclose) { RETI(fclose(HF(0))); }
IMP(fread) { RET(fread(P(0), A(1), A(2), HF(3))); }
IMP(__fread_chk) { RET(fread(P(0), A(1), A(2), HF(3))); }
IMP(fwrite) { RET(fwrite(P(0), A(1), A(2), HF(3))); }
IMP(fseek) { RETI(_fseeki64(HF(0), (int64_t)A(1), I(2))); }
IMP(fseeko) { RETI(_fseeki64(HF(0), (int64_t)A(1), I(2))); }
IMP(ftell) { RET(_ftelli64(HF(0))); }
IMP(ftello) { RET(_ftelli64(HF(0))); }
IMP(rewind) { rewind(HF(0)); }
IMP(fflush) { RETI(A(0) ? fflush(HF(0)) : fflush(0)); }
IMP(ferror) { RETI(ferror(HF(0))); }
IMP(clearerr) { clearerr(HF(0)); }
IMP(fputc) { RETI(fputc(I(0), HF(1))); }
IMP(fputs) { RETI(fputs(S(0), HF(1))); }
IMP(puts) { RETI(puts(S(0))); }
IMP(setvbuf) { RET(0); }
IMP(remove) { RETI(remove(S(0))); }
IMP(rename) {
    remove(S(1));
    RETI(rename(S(0), S(1)));
}
IMP(fprintf) {
    ArgSrc a = rt_args_regs(c, 2, 0);
    int n;
    char *s = format_alloc(S(1), &a, &n);
    fwrite(s, 1, n, HF(0));
    free(s);
    RETI(n);
}
IMP(vfprintf) {
    ArgSrc a = rt_args_valist(A(2));
    int n;
    char *s = format_alloc(S(1), &a, &n);
    fwrite(s, 1, n, HF(0));
    free(s);
    RETI(n);
}
IMP(printf) {
    ArgSrc a = rt_args_regs(c, 1, 0);
    int n;
    char *s = format_alloc(S(0), &a, &n);
    fwrite(s, 1, n, stdout);
    free(s);
    RETI(n);
}
IMP(snprintf) {
    ArgSrc a = rt_args_regs(c, 3, 0);
    RETI(rt_vformat(P(0), A(1), S(2), &a));
}
IMP(vsnprintf) {
    ArgSrc a = rt_args_valist(A(3));
    RETI(rt_vformat(P(0), A(1), S(2), &a));
}
IMP(__vsnprintf_chk) {
    ArgSrc a = rt_args_valist(A(5));
    RETI(rt_vformat(P(0), A(1), S(4), &a));
}
IMP(__vsprintf_chk) {
    ArgSrc a = rt_args_valist(A(4));
    RETI(rt_vformat(P(0), A(2) ? A(2) : (size_t)1 << 30, S(3), &a));
}
IMP(vasprintf) {
    ArgSrc a = rt_args_valist(A(2));
    int n;
    char *s = format_alloc(S(1), &a, &n);
    char *g = g_malloc(n + 1);
    memcpy(g, s, n + 1);
    free(s);
    M64(A(0)) = (uint64_t)g;
    RETI(n);
}
IMP(sscanf) {
    ArgSrc a = rt_args_regs(c, 2, 0);
    RETI(do_sscanf(S(0), S(1), &a));
}
IMP(vsscanf) {
    ArgSrc a = rt_args_valist(A(2));
    RETI(do_sscanf(S(0), S(1), &a));
}

static void android_log(int prio, const char *tag, const char *msg) {
    static const char level[] = "??VDIWEF";
    fprintf(stderr, "[%c/%s] %s%s", level[prio & 7], tag ? tag : "", msg, (*msg && msg[strlen(msg) - 1] == '\n') ? "" : "\n");
    fflush(stderr);
}
IMP(__android_log_write) { android_log(I(0), S(1), S(2)); RET(1); }
IMP(__android_log_print) {
    ArgSrc a = rt_args_regs(c, 3, 0);
    char *s = format_alloc(S(2), &a, 0);
    android_log(I(0), S(1), s);
    free(s);
    RET(1);
}
IMP(__android_log_vprint) {
    ArgSrc a = rt_args_valist(A(3));
    char *s = format_alloc(S(2), &a, 0);
    android_log(I(0), S(1), s);
    free(s);
    RET(1);
}
IMP(__android_log_assert) {
    ArgSrc a = rt_args_regs(c, 3, 0);
    char *s = A(2) ? format_alloc(S(2), &a, 0) : _strdup("");
    rt_fatal("android assert (%s) [%s]: %s", A(0) ? S(0) : "", A(1) ? S(1) : "", s);
}
IMP(openlog) { }
IMP(closelog) { }
IMP(syslog) { }

/* ------------------------------------------------------------ sockets / fds (no network service) */

IMP(socket) { RET(-1); }
IMP(bind) { RET(-1); }
IMP(connect) { RET(-1); }
IMP(listen) { RET(-1); }
IMP(accept) { RET(-1); }
IMP(recvfrom) { RET(-1); }
IMP(sendto) { RET(-1); }
IMP(setsockopt) { RET(-1); }
IMP(shutdown) { RET(-1); }
IMP(select) { RET(-1); }
IMP(getaddrinfo) { RET(-2); }
IMP(inet_ntop) { RET(0); }
IMP(inet_pton) { RET(0); }
IMP(fcntl) { RET(-1); }
IMP(close) { RET(-1); }
IMP(open) { RET(-1); }
IMP(read) { RET(-1); }
IMP(lseek) { RET(-1); }
IMP(__FD_SET_chk) { }
IMP(__FD_ISSET_chk) { RET(0); }
IMP(opendir) { RET(0); }
IMP(readdir) { RET(0); }
IMP(closedir) { RET(0); }
IMP(__readlink_chk) { RET(-1); }

/* ------------------------------------------------------------ pthread */

typedef struct GThread {
    HANDLE handle;
    uint64_t fn, arg, ret;
    DWORD id;
} GThread;
static __thread GThread *self_thread;
static GThread main_thread;

#define KEY_MAX 256
static uint64_t key_dtor[KEY_MAX];
static LONG key_next = 1;
static __thread uint64_t key_val[KEY_MAX];

static DWORD WINAPI thread_main(void *param) {
    GThread *t = param;
    self_thread = t;
    CPU *c = rt_cpu();
    t->ret = rt_invoke(c, t->fn, 1, t->arg);
    for (int pass = 0; pass < 4; pass++)
        for (int k = 1; k < KEY_MAX; k++)
            if (key_val[k] && key_dtor[k]) {
                uint64_t v = key_val[k];
                key_val[k] = 0;
                rt_invoke(c, key_dtor[k], 1, v);
            }
    return 0;
}

IMP(pthread_create) {
    GThread *t = calloc(1, sizeof *t);
    t->fn = A(2);
    t->arg = A(3);
    if (rt_verbose) { char b[256]; rt_log("pthread_create(%s)\n", rt_describe(t->fn, b, sizeof b)); }
    t->handle = CreateThread(0, 64u << 20, thread_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION, &t->id);
    M64(A(0)) = (uint64_t)t;
    RET(t->handle ? 0 : 11);
}
IMP(pthread_join) {
    GThread *t = P(0);
    WaitForSingleObject(t->handle, INFINITE);
    if (A(1)) M64(A(1)) = t->ret;
    RET(0);
}
IMP(pthread_detach) { RET(0); }
IMP(pthread_self) {
    if (!self_thread) self_thread = &main_thread;
    RET(self_thread);
}
IMP(pthread_equal) { RETI(A(0) == A(1)); }
IMP(pthread_attr_init) { memset(P(0), 0, 56); RET(0); }
IMP(pthread_attr_destroy) { RET(0); }
IMP(pthread_attr_setdetachstate) { RET(0); }
IMP(pthread_attr_setstacksize) { RET(0); }
IMP(pthread_setname_np) { RET(0); }
IMP(pthread_setschedparam) { RET(0); }
IMP(sched_get_priority_max) { RET(0); }
IMP(sched_get_priority_min) { RET(0); }

IMP(pthread_key_create) {
    LONG k = InterlockedIncrement(&key_next) - 1;
    if (k >= KEY_MAX) rt_fatal("out of pthread keys");
    key_dtor[k] = A(1);
    MS32(A(0)) = k;
    RET(0);
}
IMP(pthread_key_delete) { key_dtor[I(0) & (KEY_MAX - 1)] = 0; RET(0); }
IMP(pthread_getspecific) { RET(key_val[I(0) & (KEY_MAX - 1)]); }
IMP(pthread_setspecific) { key_val[I(0) & (KEY_MAX - 1)] = A(1); RET(0); }
IMP(pthread_once) {
    volatile LONG *o = P(0);
    for (;;) {
        LONG s = InterlockedCompareExchange(o, 1, 0);
        if (s == 2) break;
        if (s == 0) { rt_invoke(c, A(1), 0); InterlockedExchange(o, 2); WakeByAddressAll((void *)o); break; }
        LONG one = 1;
        WaitOnAddress(o, &one, 4, 10);
    }
    RET(0);
}

/* pthread_mutex_t (40 bytes): [0] type bits as in Bionic, [1] lock word, [2] owner, [3] recursion */
static void mutex_lock(volatile LONG *w) {
    DWORD tid = GetCurrentThreadId();
    if ((w[0] & 0xc000) == 0x4000 && (DWORD)w[2] == tid) { w[3]++; return; }
    if (InterlockedCompareExchange(&w[1], 1, 0) != 0) {
        do {
            LONG two = 2;
            if (w[1] == 2 || InterlockedCompareExchange(&w[1], 2, 1) != 0) WaitOnAddress(&w[1], &two, 4, INFINITE);
        } while (InterlockedCompareExchange(&w[1], 2, 0) != 0);
    }
    w[2] = (LONG)tid;
    w[3] = 1;
}
static int mutex_trylock(volatile LONG *w) {
    DWORD tid = GetCurrentThreadId();
    if ((w[0] & 0xc000) == 0x4000 && (DWORD)w[2] == tid) { w[3]++; return 0; }
    if (InterlockedCompareExchange(&w[1], 1, 0) != 0) return 16;
    w[2] = (LONG)tid;
    w[3] = 1;
    return 0;
}
static void mutex_unlock(volatile LONG *w) {
    if ((w[0] & 0xc000) == 0x4000 && w[3] > 1) { w[3]--; return; }
    w[2] = 0;
    w[3] = 0;
    if (InterlockedExchange(&w[1], 0) == 2) WakeByAddressSingle((void *)&w[1]);
}
IMP(pthread_mutex_init) {
    LONG *w = P(0);
    memset(w, 0, 40);
    if (A(1)) w[0] = (LONG)((M64(A(1)) & 3) << 14);
    RET(0);
}
IMP(pthread_mutex_destroy) { RET(0); }
IMP(pthread_mutex_lock) { mutex_lock(P(0)); RET(0); }
IMP(pthread_mutex_trylock) { RET(mutex_trylock(P(0))); }
IMP(pthread_mutex_unlock) { mutex_unlock(P(0)); RET(0); }
IMP(pthread_mutexattr_init) { M64(A(0)) = 0; RET(0); }
IMP(pthread_mutexattr_destroy) { RET(0); }
IMP(pthread_mutexattr_settype) { M64(A(0)) = A(1) & 3; RET(0); }

static int cond_wait(volatile LONG *cv, volatile LONG *mx, DWORD ms) {
    LONG seq = cv[1];
    LONG saved = mx[3];
    mx[3] = 1;
    mutex_unlock(mx);
    BOOL ok = WaitOnAddress(&cv[1], &seq, 4, ms);
    mutex_lock(mx);
    mx[3] = saved;
    return ok ? 0 : 110; /* ETIMEDOUT */
}
IMP(pthread_cond_init) { memset(P(0), 0, 48); RET(0); }
IMP(pthread_cond_destroy) { RET(0); }
IMP(pthread_cond_wait) { cond_wait(P(0), P(1), INFINITE); RET(0); }
IMP(pthread_cond_timedwait) {
    int64_t deadline = MS64(A(2)) * 1000000000 + MS64(A(2) + 8);
    int64_t left = (deadline - unix_100ns() * 100) / 1000000;
    RET(cond_wait(P(0), P(1), left <= 0 ? 0 : (DWORD)left));
}
IMP(pthread_cond_signal) {
    volatile LONG *cv = P(0);
    InterlockedIncrement(&cv[1]);
    WakeByAddressSingle((void *)&cv[1]);
    RET(0);
}
IMP(pthread_cond_broadcast) {
    volatile LONG *cv = P(0);
    InterlockedIncrement(&cv[1]);
    WakeByAddressAll((void *)&cv[1]);
    RET(0);
}

/* pthread_rwlock_t: [0] state: 0 free, >0 readers, -1 writer */
IMP(pthread_rwlock_rdlock) {
    volatile LONG *w = P(0);
    for (;;) {
        LONG s = w[0];
        if (s >= 0 && InterlockedCompareExchange(w, s + 1, s) == s) break;
        if (s < 0) WaitOnAddress(w, &s, 4, 1);
    }
    RET(0);
}
IMP(pthread_rwlock_wrlock) {
    volatile LONG *w = P(0);
    for (;;) {
        LONG s = w[0];
        if (s == 0 && InterlockedCompareExchange(w, -1, 0) == 0) break;
        if (s != 0) WaitOnAddress(w, &s, 4, 1);
    }
    RET(0);
}
IMP(pthread_rwlock_unlock) {
    volatile LONG *w = P(0);
    if (w[0] < 0) InterlockedExchange(w, 0);
    else InterlockedDecrement(w);
    WakeByAddressAll((void *)w);
    RET(0);
}

/* sem_t: [0] count */
IMP(sem_init) { M32(A(0)) = (uint32_t)A(2); RET(0); }
IMP(sem_destroy) { RET(0); }
IMP(sem_open) {
    uint32_t *s = g_malloc(16);
    memset(s, 0, 16);
    if (A(1) & 0100) s[0] = (uint32_t)A(3);
    RET(s);
}
IMP(sem_close) { RET(0); }
IMP(sem_unlink) { RET(0); }
IMP(sem_post) {
    volatile LONG *s = P(0);
    InterlockedIncrement(s);
    WakeByAddressSingle((void *)s);
    RET(0);
}
IMP(sem_wait) {
    volatile LONG *s = P(0);
    for (;;) {
        LONG v = *s;
        if (v > 0) {
            if (InterlockedCompareExchange(s, v - 1, v) == v) break;
        } else {
            WaitOnAddress(s, &v, 4, INFINITE);
        }
    }
    RET(0);
}

/* setjmp/longjmp need the lifter's cooperation; until a caller needs the
 * non-local path, setjmp records nothing and longjmp is fatal. */
IMP(setjmp) { RET(0); }
IMP(longjmp) { rt_fatal("guest longjmp (error path of an embedded decoder)"); }

void rt_libc_init(void) {
    init_ctype();
    rt_register_data("__sF", guest_sF);
    rt_register_data("stdout", &guest_stdout);
    rt_register_data("stderr", &guest_stderr);
    rt_register_data("_ctype_", &ctype_ptr);
    timeBeginPeriod(1);
    (void)qpc_scale;
}
