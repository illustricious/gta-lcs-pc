"""List distinct (mnemonic, operand shape) pairs with one example each.

Usage: python recomp/tools/shapes.py <lib.so> <out.txt>
"""
import re
import sys
from collections import Counter
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile

REG = re.compile(r'\b([xwbhsdqv])([0-9]|[12][0-9]|3[01])\b')
NUM = re.compile(r'#-?(0x[0-9a-f]+|[0-9]+(\.[0-9]+)?(e[+-]?[0-9]+)?)')
IDX = re.compile(r'\[[0-9]+\]')


def shape(ops):
    ops = NUM.sub('#N', ops)
    ops = REG.sub(lambda m: m.group(1) + 'N', ops)
    ops = IDX.sub('[I]', ops)
    return ops


def main():
    path = Path(sys.argv[1])
    with path.open('rb') as f:
        elf = ELFFile(f)
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        md.skipdata = True
        count = Counter()
        example = {}
        for name in ('.text', '.plt'):
            s = elf.get_section_by_name(name)
            if s is None:
                continue
            for addr, _, mn, ops in md.disasm_lite(s.data(), s['sh_addr']):
                key = (mn, shape(ops))
                count[key] += 1
                example.setdefault(key, '%#x: %s %s' % (addr, mn, ops))
    with open(sys.argv[2], 'w') as out:
        for key in sorted(count):
            out.write('%-8s %-44s %7d  %s\n' % (key[0], key[1], count[key], example[key]))
    print(len(count), 'shapes')


main()
