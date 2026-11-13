"""Build recomp/res/lcs.ico from the launcher icons of the user's own APK (PNG-in-ICO)."""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RES = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'local' / 'extracted' / 'base' / 'res'
OUT = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / 'recomp' / 'res' / 'lcs.ico'


def main():
    images = []
    for d in sorted(RES.glob('mipmap-*dpi-v4')):
        p = d / 'ic_launcher.png'
        if p.exists():
            data = p.read_bytes()
            w, h = struct.unpack('>II', data[16:24])
            if w <= 256:
                images.append((w, h, data))
    if not images:
        sys.exit('launcher icon not found under %s' % RES)
    images.sort()
    out = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for w, h, data in images:
        out += struct.pack('<BBBBHHII', w & 255, h & 255, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, _, data in images:
        out += data
    OUT.parent.mkdir(parents=True, exist_ok=True)
    if not OUT.exists() or OUT.read_bytes() != out:
        OUT.write_bytes(out)
    print('icon: %s (%s)' % (OUT.name, ', '.join('%dx%d' % (w, h) for w, h, _ in images)))


main()
