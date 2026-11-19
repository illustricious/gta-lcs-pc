/* Runtime core: maps the original ELF data at its link base, applies the
 * relocations, binds imports and dispatches indirect guest calls to the
 * statically recompiled functions.  The ARM64 bytes mapped here are never
 * executed or interpreted; they are only read as data by the game. */
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <bcrypt.h>

#define MAX_MODULES 4
#define THUNK_BASE 0x1F0000000ull
#define THUNK_MAX 8192
#define GUEST_STACK (8u << 20)

typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Sym;
typedef struct { uint64_t offset, info; int64_t addend; } Rela;
typedef struct { uint32_t name, type; uint64_t flags, addr, offset, size; uint32_t link, info; uint64_t align, entsize; } Shdr;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Phdr;
typedef struct { uint64_t value; const char *name; } NamedAddr;

typedef struct Module {
    RtModuleDesc d;
    uint8_t *file;
    size_t file_size;
    uint64_t span;
    RtFunc *table;
    const Sym *sym;
    unsigned nsym;
    const char *str;
    const Rela *rela[2];
    size_t nrela[2];
    uint64_t init_array, init_count;
    NamedAddr *funcs; /* sorted function symbols for diagnostics */
    unsigned nfuncs;
} Module;

static Module mods[MAX_MODULES];
static unsigned nmods;
static RtFunc thunk_fn[THUNK_MAX];
static const char *thunk_name[THUNK_MAX];
static unsigned nthunks;
static CRITICAL_SECTION thunk_lock;
static NamedAddr host_data[64];
static unsigned nhost_data;
static char root_dir[MAX_PATH];
static __thread CPU *tls_cpu;
int rt_verbose;
int rt_gui;

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

static void dump_state(CPU *c);

void rt_fatal(const char *fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\nFATAL: %s\n", msg);
    if (tls_cpu) dump_state(tls_cpu);
    fflush(stderr);
    if (rt_gui) { /* started from Explorer: there is no console to read the error from */
        extern char rt_log_path[];
        strncat(msg, "\n\nLog: ", sizeof msg - strlen(msg) - 1);
        strncat(msg, rt_log_path[0] ? rt_log_path : "lcs.log", sizeof msg - strlen(msg) - 1);
        MessageBoxA(0, msg, "GTA: Liberty City Stories", MB_ICONERROR | MB_OK);
    }
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) {}
}

/* ------------------------------------------------------------ diagnostics */

static Module *module_of(uint64_t addr) {
    for (unsigned i = 0; i < nmods; i++)
        if (addr - mods[i].d.base < mods[i].span) return &mods[i];
    return 0;
}

const char *rt_describe(uint64_t addr, char *buf, size_t cap) {
    Module *m = module_of(addr);
    if (!m) {
        if (addr - THUNK_BASE < (uint64_t)nthunks * 16)
            snprintf(buf, cap, "host:%s", thunk_name[(addr - THUNK_BASE) / 16]);
        else
            snprintf(buf, cap, "%#llx", (unsigned long long)addr);
        return buf;
    }
    uint64_t va = addr - m->d.base;
    unsigned lo = 0, hi = m->nfuncs;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (m->funcs[mid].value <= va) lo = mid + 1; else hi = mid;
    }
    if (lo && va >= m->d.text_lo && va < m->d.text_hi)
        snprintf(buf, cap, "%s+%#llx <%s+%#llx>", m->d.prefix, (unsigned long long)va, m->funcs[lo - 1].name,
                 (unsigned long long)(va - m->funcs[lo - 1].value));
    else
        snprintf(buf, cap, "%s+%#llx", m->d.prefix, (unsigned long long)va);
    return buf;
}

static int readable(uint64_t a, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)a, &mi, sizeof mi)) return 0;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return (uint64_t)mi.BaseAddress + mi.RegionSize >= a + n;
}

static void dump_state(CPU *c) {
    char buf[512];
    fprintf(stderr, "guest registers:\n");
    for (int i = 0; i < 32; i += 4)
        fprintf(stderr, "  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx\n", i, (unsigned long long)c->x[i],
                i + 1, (unsigned long long)c->x[i + 1], i + 2, (unsigned long long)c->x[i + 2], i + 3,
                (unsigned long long)c->x[i + 3]);
    fprintf(stderr, "recently entered guest functions (newest first):\n");
    for (unsigned i = 0; i < 24; i++) {
        uint64_t a = c->trace[(c->trace_pos - 1 - i) & (RT_TRACE_DEPTH - 1)];
        if (!a) break;
        fprintf(stderr, "  %s\n", rt_describe(a, buf, sizeof buf));
    }
    fprintf(stderr, "guest frame chain:\n");
    uint64_t fp = c->x[29];
    fprintf(stderr, "  lr %s\n", rt_describe(c->x[30], buf, sizeof buf));
    for (int i = 0; i < 40 && fp && !(fp & 7) && readable(fp, 16); i++) {
        uint64_t lr = M64(fp + 8), next = M64(fp);
        fprintf(stderr, "  %s\n", rt_describe(lr, buf, sizeof buf));
        if (next <= fp) break;
        fp = next;
    }
    fflush(stderr);
}

void rt_trap(CPU *c, uint64_t pc, const char *why) {
    char buf[512];
    fprintf(stderr, "\nGUEST TRAP at %s: %s\n", rt_describe(pc, buf, sizeof buf), why);
    dump_state(c);
    TerminateProcess(GetCurrentProcess(), 4);
    for (;;) {}
}

void rt_missing(CPU *c, const char *name) {
    char buf[512];
    fprintf(stderr, "\nMISSING HOST IMPORT: %s (called from %s)\n", name, rt_describe(c->x[30], buf, sizeof buf));
    dump_state(c);
    TerminateProcess(GetCurrentProcess(), 5);
    for (;;) {}
}

void rt_missing_plt(CPU *c) { rt_missing(c, "<unbound PLT slot>"); }

static LONG WINAPI on_exception(EXCEPTION_POINTERS *e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_IN_PAGE_ERROR)
        return EXCEPTION_CONTINUE_SEARCH;
    static LONG once;
    if (InterlockedExchange(&once, 1)) { Sleep(INFINITE); }
    fprintf(stderr, "\nHOST EXCEPTION %08lx at rip=%p (exe base %p)", code, e->ExceptionRecord->ExceptionAddress,
            (void *)GetModuleHandleA(0));
    if (code == EXCEPTION_ACCESS_VIOLATION)
        fprintf(stderr, " %s address %p", e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (void *)e->ExceptionRecord->ExceptionInformation[1]);
    fprintf(stderr, "\n");
    if (tls_cpu && code != EXCEPTION_STACK_OVERFLOW) dump_state(tls_cpu);
    else if (tls_cpu) {
        char buf[512];
        fprintf(stderr, "last guest function: %s\n", rt_describe(tls_cpu->trace[(tls_cpu->trace_pos - 1) & (RT_TRACE_DEPTH - 1)], buf, sizeof buf));
    }
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 6);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ------------------------------------------------------------ dispatch */

uint64_t rt_thunk(RtFunc fn, const char *name) {
    EnterCriticalSection(&thunk_lock);
    for (unsigned i = 0; i < nthunks; i++)
        if (thunk_fn[i] == fn) { LeaveCriticalSection(&thunk_lock); return THUNK_BASE + (uint64_t)i * 16; }
    if (nthunks == THUNK_MAX) rt_fatal("out of host thunks");
    thunk_fn[nthunks] = fn;
    thunk_name[nthunks] = name;
    uint64_t a = THUNK_BASE + (uint64_t)nthunks++ * 16;
    LeaveCriticalSection(&thunk_lock);
    return a;
}

static void record_missing_entry(Module *m, uint64_t va) {
    char path[MAX_PATH + 64];
    snprintf(path, sizeof path, "%s\\recomp\\entries_%s.txt", root_dir, m->d.prefix);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%llx\n", (unsigned long long)va); fclose(f); }
}

void rt_call(CPU *c, uint64_t target) {
    for (unsigned i = nmods; i-- > 0;) {
        Module *m = &mods[i];
        uint64_t off = target - m->d.base - m->d.text_lo;
        if (off < m->d.text_hi - m->d.text_lo) {
            RtFunc fn = (off & 3) ? 0 : m->table[off >> 2];
            if (fn) { fn(c); return; }
            record_missing_entry(m, target - m->d.base);
            rt_trap(c, target, "indirect call to an address that is not a known function entry "
                               "(recorded in recomp/entries_*.txt; rebuild to add it)");
        }
    }
    uint64_t t = target - THUNK_BASE;
    if (t < (uint64_t)nthunks * 16 && !(t & 15)) { thunk_fn[t / 16](c); return; }
    rt_trap(c, target, "indirect call outside any recompiled module");
}

CPU *rt_cpu_new(void) {
    CPU *c = _aligned_malloc(sizeof(CPU), 64);
    memset(c, 0, sizeof *c);
    uint8_t *stack = VirtualAlloc(0, GUEST_STACK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t *tls = VirtualAlloc(0, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!stack || !tls) rt_fatal("cannot allocate guest stack");
    c->x[31] = (uint64_t)stack + GUEST_STACK - 0x100;
    c->tpidr = (uint64_t)tls + 0x1000;
    M64(c->tpidr + 0x28) = 0x5eed1e55c0ffee00ull; /* Bionic stack-protector cookie slot */
    return c;
}

CPU *rt_cpu(void) {
    if (!tls_cpu) tls_cpu = rt_cpu_new();
    return tls_cpu;
}

uint64_t rt_invoke(CPU *c, uint64_t fn, int nargs, ...) {
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs && i < 8; i++) c->x[i] = va_arg(ap, uint64_t);
    va_end(ap);
    c->x[30] = THUNK_BASE - 16; /* return address marker, never executed */
    rt_call(c, fn);
    return c->x[0];
}

/* ------------------------------------------------------------ loading */

static uint32_t sym_hash_size;
static struct { const char *name; uint64_t addr; } *sym_hash;

static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void sym_put(const char *name, uint64_t addr) {
    uint32_t i = hash_str(name) & (sym_hash_size - 1);
    while (sym_hash[i].name) {
        if (!strcmp(sym_hash[i].name, name)) return; /* first definition wins */
        i = (i + 1) & (sym_hash_size - 1);
    }
    sym_hash[i].name = name;
    sym_hash[i].addr = addr;
}

uint64_t rt_sym(const char *name) {
    uint32_t i = hash_str(name) & (sym_hash_size - 1);
    while (sym_hash[i].name) {
        if (!strcmp(sym_hash[i].name, name)) return sym_hash[i].addr;
        i = (i + 1) & (sym_hash_size - 1);
    }
    return 0;
}

uint64_t rt_need(const char *name) {
    uint64_t a = rt_sym(name);
    if (!a) rt_fatal("required guest symbol not found: %s", name);
    return a;
}

void rt_register_data(const char *name, void *addr) {
    host_data[nhost_data].name = name;
    host_data[nhost_data++].value = (uint64_t)addr;
}

static void sha256_hex(const uint8_t *data, size_t n, char out[65]) {
    BCRYPT_ALG_HANDLE alg;
    uint8_t digest[32];
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, 0, 0);
    BCryptHash(alg, 0, 0, (PUCHAR)data, (ULONG)n, digest, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", digest[i]);
}

static int cmp_named(const void *a, const void *b) {
    uint64_t x = ((const NamedAddr *)a)->value, y = ((const NamedAddr *)b)->value;
    return x < y ? -1 : x > y;
}

static void load_module(Module *m, const char *lib_dir) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\%s", lib_dir, m->d.file);
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal("cannot open %s (the original library from your own copy of the game is required)", path);
    fseek(f, 0, SEEK_END);
    m->file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    m->file = malloc(m->file_size);
    fread(m->file, 1, m->file_size, f);
    fclose(f);
    char hex[65];
    sha256_hex(m->file, m->file_size, hex);
    if (strcmp(hex, m->d.sha256))
        rt_fatal("%s does not match the build this executable was recompiled from\n  expected %s\n  found    %s", path,
                 m->d.sha256, hex);
    const uint8_t *e = m->file;
    uint64_t phoff = *(uint64_t *)(e + 0x20), shoff = *(uint64_t *)(e + 0x28);
    unsigned phnum = *(uint16_t *)(e + 0x38), shnum = *(uint16_t *)(e + 0x3c);
    const Phdr *ph = (const Phdr *)(e + phoff);
    const Shdr *sh = (const Shdr *)(e + shoff);
    for (unsigned i = 0; i < phnum; i++)
        if (ph[i].type == 1 && ph[i].vaddr + ph[i].memsz > m->span) m->span = ph[i].vaddr + ph[i].memsz;
    m->span = (m->span + 0xffff) & ~0xffffull;
    void *mem = VirtualAlloc((void *)m->d.base, m->span, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (mem != (void *)m->d.base) rt_fatal("cannot map %s at its link base %#llx", m->d.file, (unsigned long long)m->d.base);
    for (unsigned i = 0; i < phnum; i++)
        if (ph[i].type == 1) memcpy((uint8_t *)m->d.base + ph[i].vaddr, e + ph[i].offset, ph[i].filesz);
    unsigned k = 0;
    for (unsigned i = 0; i < shnum; i++) {
        if (sh[i].type == 11) { /* SHT_DYNSYM */
            m->sym = (const Sym *)(e + sh[i].offset);
            m->nsym = (unsigned)(sh[i].size / sizeof(Sym));
            m->str = (const char *)(e + sh[sh[i].link].offset);
        } else if (sh[i].type == 4 && k < 2) { /* SHT_RELA */
            m->rela[k] = (const Rela *)(e + sh[i].offset);
            m->nrela[k++] = sh[i].size / sizeof(Rela);
        } else if (sh[i].type == 14) { /* SHT_INIT_ARRAY */
            m->init_array = m->d.base + sh[i].addr;
            m->init_count = sh[i].size / 8;
        }
    }
    size_t slots = (m->d.text_hi - m->d.text_lo) / 4;
    m->table = calloc(slots, sizeof(RtFunc));
    for (unsigned i = 0; i < m->d.entry_count; i++)
        m->table[(m->d.entries[i].addr - m->d.text_lo) / 4] = m->d.entries[i].fn;
    m->funcs = malloc(m->nsym * sizeof(NamedAddr));
    for (unsigned i = 0; i < m->nsym; i++) {
        const Sym *s = &m->sym[i];
        if (!s->shndx || !s->name) continue;
        sym_put(m->str + s->name, m->d.base + s->value);
        if ((s->info & 15) == 2) {
            m->funcs[m->nfuncs].value = s->value;
            m->funcs[m->nfuncs++].name = m->str + s->name;
        }
    }
    qsort(m->funcs, m->nfuncs, sizeof(NamedAddr), cmp_named);
}

static uint64_t resolve(Module *m, const Sym *s) {
    const char *name = m->str + s->name;
    if (s->shndx) return m->d.base + s->value;
    uint64_t a = rt_sym(name);
    if (a) return a;
    int weak = (s->info >> 4) == 2;
    for (const RtImport *im = m->d.imports; im->name; im++)
        if (!strcmp(im->name, name)) {
            extern const RtFunc rt_missing_fns[];
            for (const RtFunc *f = rt_missing_fns; weak && *f; f++)
                if (*f == im->fn) return 0; /* weak reference with no provider stays null */
            return rt_thunk(im->fn, im->name);
        }
    for (unsigned i = 0; i < nhost_data; i++)
        if (!strcmp(host_data[i].name, name)) return host_data[i].value;
    if ((s->info >> 4) == 2) return 0; /* weak */
    if (rt_verbose) rt_log("rt: unresolved data symbol %s (%s)\n", name, m->d.prefix);
    return (uint64_t)VirtualAlloc(0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static void relocate(Module *m) {
    for (int t = 0; t < 2; t++) {
        for (size_t i = 0; i < m->nrela[t]; i++) {
            const Rela *r = &m->rela[t][i];
            uint64_t *where = (uint64_t *)(m->d.base + r->offset);
            uint32_t type = (uint32_t)r->info;
            const Sym *s = &m->sym[r->info >> 32];
            switch (type) {
            case 1027: *where = m->d.base + r->addend; break;
            case 257: case 1025: case 1026: *where = resolve(m, s) + r->addend; break;
            default: rt_fatal("%s: unsupported relocation type %u", m->d.file, type);
            }
        }
    }
}

static void on_invalid_parameter(const wchar_t *expr, const wchar_t *func, const wchar_t *file, unsigned line, uintptr_t r) {
    (void)expr; (void)file; (void)line; (void)r;
    rt_fatal("host C runtime rejected an argument in %ls", func ? func : L"?");
}

void rt_init(const char *lib_dir) {
    InitializeCriticalSection(&thunk_lock);
    _set_invalid_parameter_handler(on_invalid_parameter);
    AddVectoredExceptionHandler(1, on_exception);
    GetModuleFileNameA(0, root_dir, sizeof root_dir);
    for (int k = 0; k < 2; k++) {
        char *p = strrchr(root_dir, '\\');
        if (p) *p = 0;
    }
    if (!VirtualAlloc((void *)THUNK_BASE, THUNK_MAX * 16, MEM_RESERVE, PAGE_NOACCESS)) rt_fatal("cannot reserve thunk range");
    RtModuleDesc d[MAX_MODULES];
    nmods = rt_module_descs(d);
    sym_hash_size = 1 << 17;
    sym_hash = calloc(sym_hash_size, sizeof *sym_hash);
    for (unsigned i = 0; i < nmods; i++) {
        mods[i].d = d[i];
        load_module(&mods[i], lib_dir);
    }
    rt_android_init();
    for (unsigned i = 0; i < nmods; i++) relocate(&mods[i]);
}

void rt_run_init_arrays(void) {
    CPU *c = rt_cpu();
    for (unsigned i = 0; i < nmods; i++) {
        Module *m = &mods[i];
        for (uint64_t k = 0; k < m->init_count; k++) {
            uint64_t fn = M64(m->init_array + k * 8);
            if (fn && fn != ~0ull) rt_invoke(c, fn, 0);
        }
        if (rt_verbose) rt_log("rt: %s: %llu static initialisers done\n", m->d.file, (unsigned long long)m->init_count);
    }
}

/* Diagnostic: dump the guest state of the first thread after a delay (hang analysis). */
static CPU *watched;
static DWORD WINAPI watchdog_main(void *param) {
    Sleep((DWORD)(uintptr_t)param * 1000);
    fprintf(stderr, "\nWATCHDOG: main thread state after %u s\n", (unsigned)(uintptr_t)param);
    if (watched) dump_state(watched);
    TerminateProcess(GetCurrentProcess(), 7);
    return 0;
}
void rt_watchdog(unsigned seconds) {
    watched = rt_cpu();
    CreateThread(0, 0, watchdog_main, (void *)(uintptr_t)seconds, 0, 0);
}
