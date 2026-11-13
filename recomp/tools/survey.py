"""Survey of the ARM64 inputs: dependencies, sections, and the instruction mix.

Usage: python recomp/tools/survey.py <lib.so> [--hist]
"""
import sys
from collections import Counter
from pathlib import Path

from elftools.elf.elffile import ELFFile


def main():
    path = Path(sys.argv[1])
    with path.open('rb') as f:
        elf = ELFFile(f)
        dyn = elf.get_section_by_name('.dynamic')
        print(path.name, 'needed:', [t.needed for t in dyn.iter_tags() if t.entry.d_tag == 'DT_NEEDED'])
        for s in elf.iter_sections():
            if s['sh_flags'] & 2:
                print('  %-22s va=%#9x size=%#9x flags=%#x type=%s' % (s.name, s['sh_addr'], s['sh_size'], s['sh_flags'], s['sh_type']))
        for seg in elf.iter_segments():
            print('  seg %-14s va=%#9x filesz=%#9x memsz=%#9x flags=%d' % (seg['p_type'], seg['p_vaddr'], seg['p_filesz'], seg['p_memsz'], seg['p_flags']))
        if '--hist' in sys.argv:
            from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
            md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
            md.skipdata = True
            hist = Counter()
            for name in ('.text', '.plt'):
                s = elf.get_section_by_name(name)
                if s is None:
                    continue
                for _, _, mn, _ in md.disasm_lite(s.data(), s['sh_addr']):
                    hist[mn] += 1
            total = sum(hist.values())
            print('instructions', total, 'distinct', len(hist))
            for mn, n in hist.most_common():
                print('%8d %s' % (n, mn))


main()
