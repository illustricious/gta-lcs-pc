"""Regression check: run the actual lifted CTimer against synthetic frame times.

Requires a completed lift and the normal build dependencies. Extracts the
unchanged original timer function, replaces only its OS clock dependency,
then checks the production timing wrapper at multiple refresh rates/modes.
"""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / '.tools' / 'py'))
sys.path.insert(0, str(ROOT / 'recomp'))
from elftools.elf.elffile import ELFFile
from build import LLVM

out = ROOT / 'build' / 'timing-test'
out.mkdir(exist_ok=True)
source = (ROOT / 'build/recomp/G/G_006e.c').read_text()
marker = 'void G_370b30_orig(CPU *c) {'
if marker not in source:
    marker = 'void G_370b30(CPU *c) {'
body = source[source.index(marker):]
body = body[:body.index('\n}\n') + 3]
body = body.replace('void G_370b30(', 'void G_370b30_orig(')

lib = ROOT / 'local/extracted/split_config.arm64_v8a/lib/arm64-v8a/libGame.so'
with lib.open('rb') as f:
    elf = ELFFile(f)
    symbols = elf.get_section_by_name('.dynsym')
    got = []
    for section in elf.iter_sections():
        if section.header.sh_type != 'SHT_RELA':
            continue
        for reloc in section.iter_relocations():
            addr = reloc['r_offset']
            if 0x7f9000 <= addr < 0x7fb000 and reloc['r_info_sym']:
                sym = symbols.get_symbol(reloc['r_info_sym'])
                if sym['st_shndx'] != 'SHN_UNDEF':
                    got.append('M64(B + %#x) = B + %#x;' % (addr, sym['st_value']))

test = r'''
#include "rt.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#define B 0x200000000ull
int rt_verbose;
void rt_log(const char *fmt, ...) { (void)fmt; }
static float elapsed_ms;
void G_2d1c94(CPU *c) { MF32(c->x[0] + 0x20) = elapsed_ms; }
void G_370b30(CPU *c);
static void check(int yes, const char *msg) { if (!yes) { fprintf(stderr, "FAIL: %s\n", msg); exit(1); } }
'''
test += body
test += r'''
int main(void) {
    check(VirtualAlloc((void *)B, 0x1000000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == (void *)B, "fixed guest data map");
'''
test += '\n'.join(got)
test += r'''
    CPU c = {0};
    c.x[31] = (uint64_t)malloc(65536) + 65536;
    const unsigned rates[] = {30, 60, 120, 144, 240};
    for (unsigned r = 0; r < sizeof rates / sizeof rates[0]; r++) {
        double expected = 0, actual = 0, original = 0;
        for (unsigned frame = 0; frame < rates[r] * 10; frame++) {
            /* The original engine clock has integer millisecond precision. */
            elapsed_ms = (float)(((frame + 1) * 1000 / rates[r]) - (frame * 1000 / rates[r]));
            MF32(B + 0x8722d8) = 1;
            G_370b30_orig(&c);
            original += MF32(B + 0x8722e8) / 50.0;
            G_370b30(&c);
            actual += MF32(B + 0x8722e8) / 50.0;
            expected += elapsed_ms / 1000.0;
        }
        printf("%u Hz: measured %.6fs original %.6fs corrected %.6fs\n", rates[r], expected, original, actual);
        check(fabs(actual - expected) < 0.002, "animation clock matches elapsed time");
        if (rates[r] >= 120) check(original - expected > 1.0, "regression reproduces original high-refresh drift");
    }
    elapsed_ms = 8;
    MF32(B + 0x8722d8) = 0.5f;
    G_370b30(&c);
    check(fabs(MF32(B + 0x8722e8) - 0.2f) < 0.00001f, "slow motion scale preserved");
    M8(B + 0x8722f4) = 1;
    G_370b30(&c);
    check(MF32(B + 0x8722e8) == 0, "user pause preserved");
    M8(B + 0x8722f4) = 0;
    M8(B + 0x8722f5) = 1;
    G_370b30(&c);
    check(MF32(B + 0x8722e8) == 0, "code pause preserved");
    M8(B + 0x8722f5) = 0;
    M8(B + 0x8990e0) = 1;
    G_370b30(&c);
    check(MF32(B + 0x8722dc) == 1, "chase recording fixed timestep preserved");
    M8(B + 0x8990e0) = 0;
    M8(B + 0x9b60cc) = 1;
    G_370b30(&c);
    check(fabs(MF32(B + 0x8722e8) - 0.2f) < 0.00001f, "snapshot original timer behavior preserved");
    puts("timing regression checks passed");
    return 0;
}
'''
(out / 'test.c').write_text(test)
subprocess.run([str(LLVM / 'clang.exe'), '-O2', '-w', '-I', str(ROOT / 'recomp/rt'),
                str(out / 'test.c'), str(ROOT / 'recomp/rt/timing.c'), '-o', str(out / 'test.exe')], check=True)
subprocess.run([str(out / 'test.exe')], check=True)
