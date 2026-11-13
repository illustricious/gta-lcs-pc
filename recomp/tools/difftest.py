"""Differential test of the translator against Unicorn (ARM64 oracle).

For every distinct (mnemonic, operand shape) in the module a few real
instructions are sampled, run on random states in Unicorn, and compared with
the compiled C translation of the same instruction.

Usage: python recomp/tools/difftest.py <lib.so> <workdir> [--per-shape 3] [--states 6] [--only mnemonic]
"""
import argparse
import random
import re
import struct
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from a64 import Translator, Unsupported, hx, parse_ops, Mem, Reg  # noqa: E402
from lift import Module  # noqa: E402

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM  # noqa: E402
from unicorn import arm64_const as A  # noqa: E402

SCR, SCR_SIZE = 0x50000000, 0x10000
MID = SCR + 0x8000
BASE = 0x200000000
SKIP = {'bl', 'blr', 'br', 'ret', 'brk', 'udf', '.byte', 'svc'}
REG = re.compile(r'\b([xwbhsdqv])([0-9]|[12][0-9]|3[01])\b')
NUM = re.compile(r'#-?(0x[0-9a-f]+|[0-9]+(\.[0-9]+)?(e[+-]?[0-9]+)?)')
STATE = struct.Struct('<32Q64Q4I')
F32 = [0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 3.14159274, 1e10, -1e10, 1e-10, 255.0, 65535.5, 2147483648.0, -2147483904.0,
       4294967296.0, 1e30, float('inf'), float('-inf'), float('nan'), 123456.789, -0.75, 1.5, 2.5, -2.5, 0.49999997]


def shape(ops):
    return re.sub(r'\[[0-9]+\]', '[I]', REG.sub(lambda m: m.group(1) + 'N', NUM.sub('#N', ops)))


class Ctx:
    cur = 0

    def addr_const(self, va):
        return hx(BASE + va)

    def jump(self, t):
        return '{ taken_ = 1; }'

    def call(self, t):
        return ';'

    def indirect_jump(self, e):
        return ';'


def rnd64(rng):
    k = rng.random()
    if k < 0.15:
        return rng.choice([0, 1, 2, 0xff, 0x7f, 0x80, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff, 1 << 63,
                           (1 << 63) - 1, (1 << 64) - 1, 0x100000000])
    if k < 0.3:
        return rng.getrandbits(8)
    if k < 0.4:
        return (-rng.getrandbits(8)) & ((1 << 64) - 1)
    if k < 0.55:
        return rng.getrandbits(32)
    return rng.getrandbits(64)


def rnd_vec(rng, mode):
    if mode == 'f':
        vals = [rng.choice(F32) if rng.random() < 0.5 else rng.uniform(-1000, 1000) for _ in range(4)]
        return struct.unpack('<2Q', struct.pack('<4f', *vals))
    if mode == 'd':
        vals = [float(rng.choice(F32)) if rng.random() < 0.5 else rng.uniform(-1e6, 1e6) for _ in range(2)]
        return struct.unpack('<2Q', struct.pack('<2d', *vals))
    if mode == 'small':
        return struct.unpack('<2Q', bytes(rng.choice([0, 1, 2, 3, 7, 8, 15, 31, 33, 0xff, 0xf8, 0x80]) if rng.random() < 0.5
                                          else rng.getrandbits(8) for _ in range(16)))
    return rng.getrandbits(64), rng.getrandbits(64)


def make_state(rng, mn, ops_text):
    x = [rnd64(rng) for _ in range(32)]
    x[31] = MID
    ops = parse_ops(ops_text) if ops_text else []
    for o in ops:
        if isinstance(o, Mem):
            x[o.base.n] = MID + (rng.randrange(-64, 64) if o.base.n != 31 else 0)
            if o.index is not None and not o.index.zr:
                x[o.index.n] = rng.randrange(0, 17)
    # post-index by register
    if ops and isinstance(ops[-1], Reg) and ops[-1].gpr and any(isinstance(o, Mem) for o in ops[:-1]) and not ops[-1].zr:
        pass
    fp = mn.startswith('f') or mn in ('scvtf', 'ucvtf')
    dbl = bool(re.search(r'\bd\d|\.2d|\.d\[', ops_text))
    v = []
    for _ in range(32):
        if fp:
            v.extend(rnd_vec(rng, 'd' if dbl else 'f'))
        else:
            v.extend(rnd_vec(rng, 'small' if rng.random() < 0.3 else 'raw'))
    flags = [rng.getrandbits(1) for _ in range(4)]
    return x, v, flags


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('lib')
    ap.add_argument('work')
    ap.add_argument('--per-shape', type=int, default=3)
    ap.add_argument('--states', type=int, default=6)
    ap.add_argument('--only')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--clang', default=r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang.exe')
    args = ap.parse_args()
    rng = random.Random(args.seed)
    mod = Module(args.lib, 'G', BASE)
    by_shape = defaultdict(list)
    for addr, (mn, ops) in mod.insns.items():
        if mn in SKIP or (args.only and mn != args.only):
            continue
        by_shape[(mn, shape(ops))].append(addr)
    cases = []
    for key in sorted(by_shape):
        addrs = by_shape[key]
        for a in rng.sample(addrs, min(args.per_shape, len(addrs))):
            cases.append(a)
    tr = Translator(Ctx())
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    src = ['#include <stdio.h>', '#include <stdlib.h>', '#include <windows.h>', '#include "recomp.h"',
           'static int taken_;',
           'void rt_call(CPU *c, uint64_t t) { (void)c; (void)t; }',
           'void rt_trap(CPU *c, uint64_t pc, const char *why) { (void)c; fprintf(stderr, "trap %llx %s\\n", pc, why); exit(3); }']
    states = []
    for k, a in enumerate(cases):
        mn, ops = mod.insns[a]
        stmt = tr.translate(a, mn, ops)
        src.append('static void case_%d(CPU *c) { %s } /* %x %s %s */' % (k, stmt, a, mn, ops))
        for _ in range(args.states):
            states.append((k, make_state(rng, mn, ops)))
    src.append('static void (*const cases[])(CPU *) = {%s};' % ', '.join('case_%d' % k for k in range(len(cases))))
    src.append(r'''
int main(int argc, char **argv) {
    unsigned char *scr = VirtualAlloc((void *)0x50000000ull, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (scr != (void *)0x50000000ull) { fprintf(stderr, "scratch map failed\n"); return 2; }
    static unsigned char pattern[0x10000];
    for (unsigned i = 0; i < 0x10000; i++) pattern[i] = (unsigned char)(i * 131 + (i >> 8) * 17 + 5);
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    struct { unsigned k; uint64_t x[32]; uint64_t v[64]; unsigned f[4]; } rec;
    static CPU cpu;
    CPU *c = &cpu;
    while (fread(&rec.k, 4, 1, in) == 1) {
        fread(rec.x, 8, 32, in); fread(rec.v, 8, 64, in); fread(rec.f, 4, 4, in);
        memcpy(scr, pattern, 0x10000);
        memcpy(c->x, rec.x, sizeof rec.x); memcpy(c->v, rec.v, sizeof rec.v);
        c->nf = rec.f[0]; c->zf = rec.f[1]; c->cf = rec.f[2]; c->vf = rec.f[3];
        c->tpidr = 0x50008000ull; c->excl_addr = 0; taken_ = 0;
        cases[rec.k](c);
        rec.f[0] = c->nf; rec.f[1] = c->zf; rec.f[2] = c->cf; rec.f[3] = c->vf;
        fwrite(c->x, 8, 32, out); fwrite(c->v, 8, 64, out); fwrite(rec.f, 4, 4, out);
        unsigned n = 0;
        for (unsigned i = 0; i < 0x10000; i++) n += scr[i] != pattern[i];
        fwrite(&taken_, 4, 1, out); fwrite(&n, 4, 1, out);
        for (unsigned i = 0; i < 0x10000; i++) if (scr[i] != pattern[i]) { unsigned short o = (unsigned short)i; fwrite(&o, 2, 1, out); fwrite(scr + i, 1, 1, out); }
    }
    fclose(out);
    return 0;
}''')
    (work / 'difftest.c').write_text('\n'.join(src))
    with open(work / 'states.bin', 'wb') as f:
        for k, (x, v, fl) in states:
            f.write(struct.pack('<I', k) + STATE.pack(*x, *v, *fl))
    exe = work / 'difftest.exe'
    subprocess.check_call([args.clang, '-O1', '-march=haswell', '-ffp-contract=off', '-fno-strict-aliasing', '-fwrapv',
                           '-w', '-I', str(ROOT / 'rt'), '-o', str(exe), str(work / 'difftest.c')])
    subprocess.check_call([str(exe), str(work / 'states.bin'), str(work / 'results.bin')])

    pattern = bytes((i * 131 + (i >> 8) * 17 + 5) & 0xff for i in range(SCR_SIZE))
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.ctl_set_cpu_model(A.UC_CPU_ARM64_MAX)
    uc.mem_map(SCR, SCR_SIZE)
    code_lo = (BASE + mod.lo) & ~0xfff
    code_hi = (BASE + mod.hi + 0xfff) & ~0xfff
    uc.mem_map(code_lo, code_hi - code_lo)
    for s in mod.exec_secs:
        uc.mem_write(BASE + s['sh_addr'], s.data())
    uc.reg_write(A.UC_ARM64_REG_CPACR_EL1, 3 << 20)
    xregs = [A.UC_ARM64_REG_X0 + i for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30, A.UC_ARM64_REG_SP]
    vregs = [A.UC_ARM64_REG_V0 + i for i in range(32)]
    res = open(work / 'results.bin', 'rb')
    fails = defaultdict(list)
    nan_only = 0
    for k, (x, v, fl) in states:
        a = cases[k]
        mn, ops = mod.insns[a]
        out = STATE.unpack(res.read(STATE.size))
        taken, ndiff = struct.unpack('<II', res.read(8))
        cdiff = {}
        for _ in range(ndiff):
            o, b = struct.unpack('<HB', res.read(3))
            cdiff[o] = b
        uc.mem_write(SCR, pattern)
        for i, r in enumerate(xregs):
            uc.reg_write(r, x[i])
        for i, r in enumerate(vregs):
            uc.reg_write(r, v[2 * i] | (v[2 * i + 1] << 64))
        uc.reg_write(A.UC_ARM64_REG_NZCV, (fl[0] << 31) | (fl[1] << 30) | (fl[2] << 29) | (fl[3] << 28))
        uc.reg_write(A.UC_ARM64_REG_TPIDR_EL0, 0x50008000)
        try:
            uc.emu_start(BASE + a, BASE + a + 4, count=1)
        except Exception as ex:
            fails[(mn, shape(ops))].append('%#x %s %s: unicorn %s' % (a, mn, ops, ex))
            continue
        ux = [uc.reg_read(r) for r in xregs]
        uv = []
        for r in vregs:
            val = uc.reg_read(r)
            uv += [val & ((1 << 64) - 1), val >> 64]
        nzcv = uc.reg_read(A.UC_ARM64_REG_NZCV)
        uf = [(nzcv >> 31) & 1, (nzcv >> 30) & 1, (nzcv >> 29) & 1, (nzcv >> 28) & 1]
        pc = uc.reg_read(A.UC_ARM64_REG_PC)
        umem = bytes(uc.mem_read(SCR, SCR_SIZE))
        udiff = {i: umem[i] for i in range(SCR_SIZE) if umem[i] != pattern[i]} if umem != pattern else {}
        problems = []
        for i in range(32):
            if out[i] != ux[i]:
                problems.append('x%d c=%#x arm=%#x (in %#x)' % (i, out[i], ux[i], x[i]))
        vbad = False
        for i in range(32):
            c_lo, c_hi, u_lo, u_hi = out[32 + 2 * i], out[33 + 2 * i], uv[2 * i], uv[2 * i + 1]
            if (c_lo, c_hi) != (u_lo, u_hi):
                cb, ub = struct.pack('<2Q', c_lo, c_hi), struct.pack('<2Q', u_lo, u_hi)
                cf, ufl = struct.unpack('<4f', cb), struct.unpack('<4f', ub)
                cd, ud = struct.unpack('<2d', cb), struct.unpack('<2d', ub)
                nan32 = all(p == q or (p != p and q != q) for p, q in zip(cf, ufl)) and any(p != p for p in cf)
                nan64 = all(p == q or (p != p and q != q) for p, q in zip(cd, ud)) and any(p != p for p in cd)
                if nan32 or nan64:
                    vbad = True
                    continue
                problems.append('v%d c=%032x arm=%032x (in %032x)' % (i, c_lo | (c_hi << 64), u_lo | (u_hi << 64),
                                                                 v[2 * i] | (v[2 * i + 1] << 64)))
        if list(out[96:100]) != uf:
            problems.append('nzcv c=%s arm=%s' % (list(out[96:100]), uf))
        if cdiff != udiff:
            problems.append('mem c=%s arm=%s' % (sorted(cdiff.items())[:6], sorted(udiff.items())[:6]))
        is_branch = mn.startswith('b.') or mn in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz')
        if is_branch and bool(taken) != (pc != BASE + a + 4):
            problems.append('branch c=%d arm pc=%#x' % (taken, pc))
        if problems:
            fails[(mn, shape(ops))].append('%#x %s %s: %s' % (a, mn, ops, '; '.join(problems[:4])))
        elif vbad:
            nan_only += 1
    print('%d cases, %d states, %d shapes failing, %d states differ only in NaN payload' % (
        len(cases), len(states), len(fails), nan_only))
    for key in sorted(fails):
        print('FAIL %s %s (%d)' % (key[0], key[1], len(fails[key])))
        for line in fails[key][:2]:
            print('     ' + line)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
