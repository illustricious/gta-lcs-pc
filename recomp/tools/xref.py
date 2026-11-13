"""Find code that materialises a given VA with adrp+add (or adrp+ldr for GOT slots) in the generated C.

Usage: python recomp/tools/xref.py <prefix> <hex-va> [...]
"""
import glob
import re
import sys

prefix = sys.argv[1]
targets = [int(a, 16) for a in sys.argv[2:]]
names = {}
for line in open('build/recomp/%s/%s_exports.txt' % (prefix, prefix)):
    n, a = line.split()
    names.setdefault(int(a, 16), n)
starts = sorted(names)
import bisect


def func(a):
    i = bisect.bisect_right(starts, a) - 1
    return '%s+%#x' % (names[starts[i]], a - starts[i]) if i >= 0 else '?'


for fn in sorted(glob.glob('build/recomp/%s/%s_0*.c' % (prefix, prefix))):
    lines = open(fn).read().split('\n')
    for i, l in enumerate(lines):
        m = re.search(r'/\* ([0-9a-f]+) adrp (x\d+), #(0x[0-9a-f]+) \*/', l)
        if not m:
            continue
        page, reg = int(m.group(3), 16), m.group(2)
        for k in range(1, 8):
            if i + k >= len(lines):
                break
            n = re.search(r'/\* ([0-9a-f]+) (add|ldr) (\w+), (?:\[)?%s, #(0x[0-9a-f]+)\]? \*/' % reg, lines[i + k])
            if n and page + int(n.group(4), 16) in targets:
                print('%s  %s  %s -> %#x   %s' % (n.group(1), n.group(2), reg, page + int(n.group(4), 16), func(int(n.group(1), 16))))
                break
