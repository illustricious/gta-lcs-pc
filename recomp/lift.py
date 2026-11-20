"""Static recompiler: ARM64 ELF shared object -> C translation units.

Every executable instruction of the module is translated ahead of time.
Nothing is interpreted at run time; the output only depends on recomp.h and
on host implementations of the module's imports.

Usage: python recomp/lift.py <lib.so> <outdir> --prefix G --base 0x200000000
"""
import argparse
import bisect
import hashlib
import struct
import sys
import time
from collections import defaultdict
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile

sys.path.insert(0, str(Path(__file__).parent))
from a64 import Translator, Unsupported, hx, split_top  # noqa: E402

# Host wrappers retain the generated implementation as <entry>_orig.
# Calls and dispatch tables keep the public entry name, so both direct and
# indirect guest calls pass through the wrapper. Addresses are version-pinned.
# Functions replaced by host code: the recompiled body becomes <entry>_orig.
# 0x496d04..0x496e80: CSimpleModelInfo LOD queries, ported to Rust (recomp/engine/src/lod.rs).
HOST_HOOKS = {'G': {0x370b30, 0x527bf4, 0x41e0cc, 0x48ca2c, 0x4106e8,
                    0x496d04, 0x496d20, 0x496d80, 0x496dc4, 0x496e2c, 0x496e80,
                    0x4772fc,
                    # CHud radar getters and CRadar helpers, ported to Rust (recomp/engine/src/hud.rs)
                    0x48411c, 0x48416c, 0x4841c8, 0x484224, 0x35a400, 0x358b34, 0x358b98, 0x358bd8,
                    # CTimeCycle and FillGaps<T>, rewritten in Rust (recomp/engine/src/timecycle.rs)
                    0x36d570, 0x3705f0, 0x37060c, 0x370630, 0x36d194, 0x36f090, 0x36f66c, 0x36fd14,
                    0x3703bc, 0x370428, 0x370494, 0x370518, 0x37058c,
                    # CPad action getters with per-action bindings (recomp/engine/src/pad.rs)
                    0x345e84, 0x345c20, 0x345b70, 0x345d94, 0x345de4, 0x345c88, 0x345cbc, 0x3464e4,
                    0x345684, 0x345780, 0x345e1c, 0x345e48, 0x346038, 0x346090, 0x346758, 0x345a20,
                    0x3452dc, 0x345490, 0x344ea0, 0x3458c4, 0x345938, 0x3459ac,
                    0x563238,  # CalcScreenSize: world cull counters (rt/engine_bridge.c)
                    0x46f818,  # CDraw::SetFOV: FOV scale (recomp/engine/src/camera.rs)
                    0x39d28c, 0x39bfd0, 0x39ef48,  # cWorldStream swap / RemoveOldSectorModels / Render: sector cross-fade (rt/world_fade.c)
                    0x4623d0, 0x4623f0, 0x462410, 0x462430, 0x462450, 0x462470,  # population ranges x draw distance (rt/population.c)
                    0x543b7c,  # Touchscreen::Render: on-screen controls hidden (rt/tweaks.c)
                    # CCamera / CCam queries and setters (recomp/engine/src/camera.rs)
                    0x3135fc, 0x329e10, 0x329ee8, 0x329dd4, 0x329de0, 0x328164, 0x329dec, 0x3299c4, 0x32e3f4,
                    0x329e18, 0x329eac, 0x32e1f8, 0x31fb84, 0x313a78, 0x32807c, 0x328088, 0x327324, 0x32af08,
                    0x32dffc, 0x326a20, 0x32bf70, 0x32e080, 0x32e438, 0x32e410, 0x32e220, 0x32e310}}  # CFont::PrintString: text capture for host-drawn engine menu pages (rt/frontend.c)

CHUNK = 0x8000  # bytes of guest text per generated C file
TERMINATORS = {'b', 'ret', 'br', 'brk', 'udf', '.byte'}
BRANCHES = {'b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz'}
PADDING = {'nop', 'udf', '.byte'}


def parse_eh_frame(elf):
    """Return {start: end} for every FDE."""
    sec = elf.get_section_by_name('.eh_frame')
    if sec is None:
        return {}
    data, base = sec.data(), sec['sh_addr']
    out, cies, pos = {}, {}, 0
    while pos + 4 <= len(data):
        length = struct.unpack_from('<I', data, pos)[0]
        if length == 0:
            break
        start = pos + 4
        if length == 0xffffffff:
            raise ValueError('64-bit DWARF in .eh_frame')
        cid = struct.unpack_from('<I', data, start)[0]
        if cid == 0:
            p = start + 4
            p += 1  # version
            aug_end = data.index(b'\0', p)
            aug = data[p:aug_end].decode()
            p = aug_end + 1
            enc = 0
            if aug.startswith('z'):
                def uleb(p):
                    v = s = 0
                    while True:
                        b = data[p]
                        p += 1
                        v |= (b & 0x7f) << s
                        s += 7
                        if not b & 0x80:
                            return v, p
                _, p = uleb(p)  # code align
                while data[p] & 0x80:  # data align (sleb)
                    p += 1
                p += 1
                _, p = uleb(p)  # return register
                _, p = uleb(p)  # augmentation length
                for ch in aug[1:]:
                    if ch == 'R':
                        enc = data[p]
                        p += 1
                    elif ch == 'P':
                        penc = data[p]
                        p += 1
                        p += {0: 8, 2: 2, 3: 4, 4: 8, 0xa: 2, 0xb: 4, 0xc: 8}[penc & 0xf]
                    elif ch == 'L':
                        p += 1
            cies[pos] = enc
        else:
            enc = cies[start - cid]
            p = start + 4
            fmt = enc & 0xf
            if fmt in (0xb, 0x3):
                size, code = 4, '<i' if fmt == 0xb else '<I'
            elif fmt in (0xc, 0x4, 0x0):
                size, code = 8, '<q' if fmt == 0xc else '<Q'
            else:
                raise ValueError('FDE pointer encoding %#x' % enc)
            begin = struct.unpack_from(code, data, p)[0]
            if (enc & 0x70) == 0x10:
                begin += base + p
            rng = struct.unpack_from(code.replace('i', 'I').replace('q', 'Q'), data, p + size)[0]
            if rng:
                out[begin] = max(out.get(begin, 0), begin + rng)
        pos = start + length
    return out


class Module:
    def __init__(self, path, prefix, base):
        self.path, self.prefix, self.base = Path(path), prefix, base
        raw = self.path.read_bytes()
        self.sha = hashlib.sha256(raw).hexdigest()
        self.f = open(path, 'rb')
        self.elf = ELFFile(self.f)
        self.exec_secs = [s for s in self.elf.iter_sections()
                          if s['sh_type'] == 'SHT_PROGBITS' and s['sh_flags'] & 4 and s['sh_size']]
        self.lo = min(s['sh_addr'] for s in self.exec_secs)
        self.hi = max(s['sh_addr'] + s['sh_size'] for s in self.exec_secs)
        self.insns = {}   # addr -> (mnemonic, op_str)
        self.sec_ranges = []
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        md.skipdata = True
        for s in self.exec_secs:
            a0 = s['sh_addr']
            self.sec_ranges.append((a0, a0 + s['sh_size'], s.name))
            for addr, size, mn, ops in md.disasm_lite(s.data(), a0):
                self.insns[addr] = (mn, ops)
        self._symbols()
        self._plt()
        self._regions()

    def in_exec(self, a):
        return any(lo <= a < hi for lo, hi, _ in self.sec_ranges)

    def _symbols(self):
        dynsym = self.elf.get_section_by_name('.dynsym')
        self.sym_by_index = list(dynsym.iter_symbols())
        self.func_syms = {}   # addr -> name
        self.sym_ends = {}
        for s in self.sym_by_index:
            if s['st_info']['type'] == 'STT_FUNC' and s['st_shndx'] != 'SHN_UNDEF' and self.in_exec(s['st_value']):
                a = s['st_value']
                self.func_syms.setdefault(a, s.name)
                if s['st_size']:
                    self.sym_ends[a] = max(self.sym_ends.get(a, 0), a + s['st_size'])
        self.fdes = parse_eh_frame(self.elf)
        # pointers into code stored in data
        self.code_ptrs = set()
        self.imports = {}  # name -> 'func' | 'data'
        for name in ('.rela.dyn', '.rela.plt'):
            sec = self.elf.get_section_by_name(name)
            if sec is None:
                continue
            for r in sec.iter_relocations():
                t = r['r_info_type']
                if t == 1027:  # R_AARCH64_RELATIVE
                    if self.in_exec(r['r_addend']) and r['r_addend'] % 4 == 0:
                        self.code_ptrs.add(r['r_addend'])
                elif t in (257, 1025, 1026):
                    s = self.sym_by_index[r['r_info_sym']]
                    if s['st_shndx'] == 'SHN_UNDEF':
                        kind = 'func' if (s['st_info']['type'] == 'STT_FUNC' or t == 1026) else 'data'
                        if self.imports.get(s.name) != 'func':
                            self.imports[s.name] = kind
                    elif s['st_info']['type'] == 'STT_FUNC':
                        self.code_ptrs.add(s['st_value'] + r['r_addend'])
        init = self.elf.get_section_by_name('.init_array')
        self.init_array_va = (init['sh_addr'], init['sh_size']) if init else (0, 0)

    def _plt(self):
        """Map each PLT stub address to ('imp', name) or ('int', va)."""
        self.plt = {}
        slot_sym = {}
        sec = self.elf.get_section_by_name('.rela.plt')
        if sec is not None:
            for r in sec.iter_relocations():
                slot_sym[r['r_offset']] = self.sym_by_index[r['r_info_sym']]
        plt = self.elf.get_section_by_name('.plt')
        if plt is None:
            return
        a, end = plt['sh_addr'], plt['sh_addr'] + plt['sh_size']
        self.plt_range = (a, end)
        while a + 16 <= end:
            m0, o0 = self.insns.get(a, ('', ''))
            m1, o1 = self.insns.get(a + 4, ('', ''))
            m3, o3 = self.insns.get(a + 12, ('', ''))
            if m0 == 'bti':  # BTI-prefixed stubs
                m0, o0 = self.insns.get(a + 4, ('', ''))
                m1, o1 = self.insns.get(a + 8, ('', ''))
            if m0 == 'adrp' and m1 == 'ldr' and o0.startswith('x16') and o1.startswith('x17'):
                page = int(o0.split('#')[1], 0)
                off = int(o1.split('#')[1].rstrip(']'), 0) if '#' in o1 else 0
                s = slot_sym.get(page + off)
                if s is not None:
                    if s['st_shndx'] == 'SHN_UNDEF':
                        self.plt[a] = ('imp', s.name)
                    else:
                        self.plt[a] = ('int', s['st_value'])
                    a += 16
                    continue
            a += 4

    def _regions(self):
        starts = set(self.func_syms) | set(self.fdes) | set(self.plt)
        starts = {a for a in starts if a in self.insns}
        ends = {}
        for a in starts:
            e = self.fdes.get(a) or self.sym_ends.get(a)
            if a in self.plt:
                e = a + 16
            ends[a] = e
        order = sorted(starts)
        regions = []
        for i, a in enumerate(order):
            nxt = order[i + 1] if i + 1 < len(order) else None
            sec_end = next(hi for lo, hi, _ in self.sec_ranges if lo <= a < hi)
            e = ends[a] or nxt or sec_end
            if nxt is not None:
                e = min(e, nxt)
            e = min(e, sec_end)
            regions.append((a, e))
        # code outside any known function: one region per contiguous run
        covered = regions
        gaps = []
        for lo, hi, _ in self.sec_ranges:
            pos = lo
            for a, e in [r for r in covered if lo <= r[0] < hi]:
                if a > pos:
                    gaps.append((pos, a))
                pos = max(pos, e)
            if pos < hi:
                gaps.append((pos, hi))
        self.gap_regions = []
        for lo, hi in gaps:
            a = lo
            while a < hi and self.insns.get(a, ('.byte',))[0] in PADDING:
                a += 4
            e = hi
            while e > a and self.insns.get(e - 4, ('.byte',))[0] in PADDING:
                e -= 4
            if a < e:
                self.gap_regions.append((a, e))
        self.regions = sorted(regions + self.gap_regions)
        self.region_starts = [r[0] for r in self.regions]

    def region_of(self, a):
        i = bisect.bisect_right(self.region_starts, a) - 1
        if i >= 0 and self.regions[i][0] <= a < self.regions[i][1]:
            return self.regions[i]
        return None

    # -------------------------------------------------------------- analysis
    def analyse(self, extra_entries=()):
        targets = set(self.code_ptrs) | set(extra_entries)
        lo, hi = self.init_array_va
        for a, e in self.regions:
            adrp = {}
            addr = a
            last = None
            while addr < e:
                mn, ops = self.insns[addr]
                last = mn
                if mn in BRANCHES or mn.startswith('b.'):
                    t = int(ops.rsplit('#', 1)[1], 0)
                    if not (a <= t < e) or mn == 'bl':
                        targets.add(t)
                elif mn == 'adrp':
                    reg, val = ops.split(', #')
                    adrp[reg] = int(val, 0)
                elif mn == 'adr':
                    t = int(ops.rsplit('#', 1)[1], 0)
                    if self.in_exec(t) and t % 4 == 0:
                        targets.add(t)
                else:
                    parts = ops.split(', ')
                    if mn == 'add' and len(parts) == 3 and parts[1] in adrp and parts[2].startswith('#'):
                        t = adrp[parts[1]] + int(parts[2][1:], 0)
                        if self.in_exec(t) and t % 4 == 0:
                            targets.add(t)
                    if parts and parts[0] in adrp and mn not in ('str', 'stp', 'cmp', 'strb', 'strh'):
                        del adrp[parts[0]]
                addr += 4
            if last not in TERMINATORS and e in self.insns:
                targets.add(e)
        self.entries = set(self.region_starts)
        self.extra = defaultdict(list)
        self.unmapped = set()
        for t in targets:
            r = self.region_of(t)
            if r is None or t not in self.insns:
                self.unmapped.add(t)
                continue
            self.entries.add(t)
            if t != r[0]:
                self.extra[r[0]].append(t)

    def fname(self, a):
        return '%s_%x' % (self.prefix, a)

    def target_name(self, t):
        """C function to call for a direct transfer to guest address t."""
        p = self.plt.get(t)
        if p:
            if p[0] == 'imp':
                return 'imp_' + p[1]
            t = p[1]
        if t in self.entries:
            return self.fname(t)
        return None


class RegionCtx:
    def __init__(self, mod, start, end):
        self.mod, self.start, self.end = mod, start, end
        self.labels = set()
        self.all_labels = False

    def addr_const(self, va):
        return hx(self.mod.base + va)

    def _transfer(self, t):
        name = self.mod.target_name(t)
        if name is None:
            return 'rt_trap(c, %s, "branch to unmapped %#x");' % (hx(self.mod.base + self.cur), t)
        return '%s(c);' % name

    def jump(self, t):
        if self.start <= t < self.end:
            self.labels.add(t)
            return 'goto L_%x;' % t
        return '{ %s return; }' % self._transfer(t)

    def call(self, t):
        return '{ X(30) = %s; %s }' % (self.addr_const(self.cur + 4), self._transfer(t))

    def indirect_jump(self, expr):
        self.all_labels = True
        return '{ t_ptr_ = %s; goto dispatch_; }' % expr


def emit_region(mod, tr_stats, start, end):
    p = mod.plt.get(start)
    name = mod.fname(start)
    sym = mod.func_syms.get(start, '')
    if p:  # PLT stub: bind statically
        tgt = mod.target_name(start) or 'rt_missing_plt'
        return '/* plt %s */\nvoid %s(CPU *c) { %s(c); }\n' % (p[1] if p[0] == 'imp' else hex(p[1]), name, tgt)
    ctx = RegionCtx(mod, start, end)
    tr = Translator(ctx)
    body = []
    addr = start
    last = None
    while addr < end:
        mn, ops = mod.insns[addr]
        ctx.cur = addr
        last = mn
        if mn == '.byte':
            stmt = 'rt_trap(c, %s, "data in code");' % hx(mod.base + addr)
        else:
            try:
                stmt = tr.translate(addr, mn, ops)
            except Unsupported as ex:
                tr_stats[str(ex).split(' ')[0]] += 1
                stmt = 'rt_trap(c, %s, "unsupported: %s %s");' % (hx(mod.base + addr), mn, ops.replace('"', "'"))
            except Exception as ex:  # malformed operand shape: report, keep going
                tr_stats['error:' + mn] += 1
                stmt = 'rt_trap(c, %s, "translator error: %s %s");' % (hx(mod.base + addr), mn, ops.replace('"', "'"))
        body.append((addr, stmt, '%s %s' % (mn, ops)))
        addr += 4
    extra = sorted(set(mod.extra.get(start, ())))
    for t in extra:
        ctx.labels.add(t)
    out = []
    if sym:
        out.append('/* %s */' % sym)
    multi = bool(extra)
    decl = '    uint64_t t_ptr_ = 0;' if ctx.all_labels else None
    if multi:
        out.append('static void %s_body(CPU *c, uint64_t pc_) {' % name)
        if decl:
            out.append(decl)
        out.append('    switch (pc_) { %s default: break; }' % ' '.join('case %s: goto L_%x;' % (hx(t), t) for t in extra))
    else:
        out.append('void %s(CPU *c) {' % name)
        out.append('    RT_ENTER(%s);' % hx(mod.base + start))
        if decl:
            out.append(decl)
    for addr, stmt, text in body:
        lab = 'L_%x: ' % addr if (ctx.all_labels or addr in ctx.labels) else ''
        out.append('    %s%s /* %x %s */' % (lab, stmt, addr, text.replace('*/', '* /')))
    if last not in TERMINATORS:
        nxt = mod.target_name(end)
        if nxt:
            out.append('    %s(c); return;' % nxt)
        else:
            out.append('    rt_trap(c, %s, "fell off the end of a function");' % hx(mod.base + end))
    if ctx.all_labels:
        cases = ' '.join('case %s: goto L_%x;' % (hx(mod.base + a), a) for a, _, _ in body)
        out.append('    return;')
        out.append('dispatch_: switch (t_ptr_) { %s default: rt_call(c, t_ptr_); return; }' % cases)
    out.append('}')
    text = '\n'.join(out)
    if multi:
        text += '\nvoid %s(CPU *c) { RT_ENTER(%s); %s_body(c, 0); }' % (name, hx(mod.base + start), name)
        for t in extra:
            text += '\nvoid %s(CPU *c) { RT_ENTER(%s); %s_body(c, %s); }' % (mod.fname(t), hx(mod.base + t), name, hx(t))
    for hooked in HOST_HOOKS.get(mod.prefix, set()):
        entry = mod.fname(hooked)
        text = text.replace('void %s(CPU *c)' % entry, 'void %s_orig(CPU *c)' % entry)
    return text + '\n'


def write_if_changed(path, text):
    if path.exists() and path.read_text() == text:
        return False
    path.write_text(text)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('lib')
    ap.add_argument('outdir')
    ap.add_argument('--prefix', default='G')
    ap.add_argument('--base', type=lambda v: int(v, 0), default=0x200000000)
    ap.add_argument('--entries', help='file with extra entry addresses (hex, one per line)')
    args = ap.parse_args()
    t0 = time.time()
    mod = Module(args.lib, args.prefix, args.base)
    extra = set()
    if args.entries and Path(args.entries).exists():
        for line in Path(args.entries).read_text().split():
            extra.add(int(line, 16))
    mod.analyse(extra)
    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)
    P = args.prefix
    stats = defaultdict(int)
    files = defaultdict(list)
    for a, e in mod.regions:
        files[a // CHUNK].append(emit_region(mod, stats, a, e))
    head = '/* Generated by recomp/lift.py from %s (sha256 %s). Do not edit. */\n#include "recomp.h"\n#include "%s_funcs.h"\n\n' % (
        mod.path.name, mod.sha, P)
    changed = 0
    names = set()
    for idx, parts in sorted(files.items()):
        fn = '%s_%04x.c' % (P, idx)
        names.add(fn)
        changed += write_if_changed(out / fn, head + '\n'.join(parts))
    for old in out.glob('%s_[0-9a-f][0-9a-f][0-9a-f][0-9a-f].c' % P):
        if old.name not in names:
            old.unlink()
    protos = ['/* Generated. */', '#pragma once']
    for a in sorted(mod.entries):
        protos.append('void %s(CPU *c);' % mod.fname(a))
    for name in sorted(n for n, k in mod.imports.items() if k == 'func'):
        protos.append('void imp_%s(CPU *c);' % name)
    write_if_changed(out / ('%s_funcs.h' % P), '\n'.join(protos) + '\n')
    tab = [head, 'const RtEntry %s_entries[] = {' % P]
    for a in sorted(mod.entries):
        tab.append('    {%s, %s},' % (hx(a), mod.fname(a)))
    tab.append('};')
    tab.append('const unsigned %s_entry_count = sizeof(%s_entries) / sizeof(%s_entries[0]);' % (P, P, P))
    tab.append('const RtImport %s_imports[] = {' % P)
    for name in sorted(n for n, k in mod.imports.items() if k == 'func'):
        tab.append('    {"%s", imp_%s},' % (name, name))
    tab.append('    {0, 0}};')
    tab.append('const char %s_sha256[] = "%s";' % (P, mod.sha))
    tab.append('const uint64_t %s_base = %s, %s_text_lo = %s, %s_text_hi = %s;' % (
        P, hx(mod.base), P, hx(mod.lo), P, hx(mod.hi)))
    write_if_changed(out / ('%s_table.c' % P), '\n'.join(tab) + '\n')
    (out / ('%s_imports.txt' % P)).write_text(
        ''.join('%s %s\n' % (k, n) for n, k in sorted(mod.imports.items())))
    dynsym = mod.sym_by_index
    exp = []
    for sy in dynsym:
        if sy['st_shndx'] != 'SHN_UNDEF' and sy.name and sy['st_info']['type'] == 'STT_FUNC' and sy['st_value'] in mod.entries:
            exp.append('%s %x\n' % (sy.name, sy['st_value']))
    write_if_changed(out / ('%s_exports.txt' % P), ''.join(sorted(set(exp))))
    total = len(mod.insns)
    bad = sum(stats.values())
    print('%s: %d instructions, %d regions (%d outside symbols/FDEs), %d entries, %d files (%d rewritten), %.1fs' % (
        mod.path.name, total, len(mod.regions), len(mod.gap_regions), len(mod.entries), len(files), changed,
        time.time() - t0))
    print('untranslated: %d (%.4f%%)' % (bad, 100.0 * bad / total))
    for k, v in sorted(stats.items(), key=lambda kv: -kv[1])[:40]:
        print('   %6d %s' % (v, k))
    if mod.unmapped:
        print('branch targets outside any region: %d e.g. %s' % (
            len(mod.unmapped), ', '.join(hex(a) for a in sorted(mod.unmapped)[:8])))


if __name__ == '__main__':
    main()
