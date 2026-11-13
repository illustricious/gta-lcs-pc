/* Host runtime interface: everything the recompiled modules need from
 * Windows.  Nothing in the runtime implements game logic. */
#pragma once
#include "recomp.h"
#include <stdarg.h>

#define IMP(name) void imp_##name(CPU *c)
#define A(n) (c->x[n])
#define P(n) ((void *)c->x[n])
#define S(n) ((const char *)c->x[n])
#define I(n) ((int32_t)c->x[n])
#define FA(n) (c->v[n].f[0])
#define DA(n) (c->v[n].df[0])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define RETI(v) (c->x[0] = (uint64_t)(int64_t)(int32_t)(v))
#define RETF(v) SET_S(0, v)
#define RETD(v) SET_D(0, v)

typedef struct RtModuleDesc {
    const char *prefix, *file, *sha256;
    uint64_t base, text_lo, text_hi;
    const RtEntry *entries;
    unsigned entry_count;
    const RtImport *imports;
} RtModuleDesc;
unsigned rt_module_descs(RtModuleDesc *d);

/* core */
void rt_init(const char *lib_dir);
void rt_run_init_arrays(void);
void rt_missing(CPU *c, const char *name);
void rt_log(const char *fmt, ...);
__declspec(noreturn) void rt_fatal(const char *fmt, ...);
CPU *rt_cpu(void);                       /* CPU of the calling host thread (created on demand) */
CPU *rt_cpu_new(void);
uint64_t rt_invoke(CPU *c, uint64_t fn, int nargs, ...); /* integer/pointer arguments only */
uint64_t rt_sym(const char *name);       /* guest address of an exported symbol, 0 if absent */
uint64_t rt_need(const char *name);      /* same, fatal if absent */
uint64_t rt_thunk(RtFunc fn, const char *name); /* guest-callable address for a host function */
const char *rt_describe(uint64_t addr, char *buf, size_t cap);
void rt_register_data(const char *name, void *addr);
extern int rt_verbose;
extern int rt_gui; /* no console: errors are shown in a message box */

/* guest variadic arguments */
typedef struct ArgSrc {
    uint64_t *gr;
    int gr_left;
    V128 *vr;
    int vr_left;
    uint64_t *stack;
} ArgSrc;
ArgSrc rt_args_regs(CPU *c, int first_gr, int first_vr);
ArgSrc rt_args_valist(uint64_t va_list_ptr);
uint64_t rt_arg_int(ArgSrc *a);
double rt_arg_double(ArgSrc *a);
int rt_vformat(char *out, size_t cap, const char *fmt, ArgSrc *a);

/* guest stdio handles */
void *rt_host_file(uint64_t guest_file);

/* platform (rt_android.c) */
void rt_android_init(void);
extern void *rt_gl_fn[];
extern const char *const rt_gl_names[];
extern const RtFunc rt_gl_shims[];
void rt_gl_log(CPU *c, const char *name);
