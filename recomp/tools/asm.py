"""Print the original ARM64 instructions of a guest range from the generated C (comments).

Usage: python recomp/tools/asm.py <start-hex> <end-hex> [module prefix, default G]
"""
import glob
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
start, end = int(sys.argv[1], 16), int(sys.argv[2], 16)
prefix = sys.argv[3] if len(sys.argv) > 3 else 'G'
pat = re.compile(r'/\* ([0-9a-f]+) (.*?) \*/')
seen = {}
for f in glob.glob(str(ROOT / 'build' / 'recomp' / prefix / ('%s_*.c' % prefix))):
    for m in pat.finditer(open(f).read()):
        a = int(m.group(1), 16)
        if start <= a < end:
            seen.setdefault(a, m.group(2))
for a in sorted(seen):
    print('%x %s' % (a, seen[a]))
