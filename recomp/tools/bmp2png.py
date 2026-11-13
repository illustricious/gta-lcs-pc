"""Convert 24-bit BMP screenshots written by lcs.exe --shots to PNG."""
import struct
import sys
import zlib


def convert(src, dst):
    d = open(src, 'rb').read()
    w, h = struct.unpack_from('<ii', d, 18)
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(h - 1, -1, -1):
        r = bytearray(d[54 + y * stride:54 + y * stride + w * 3])
        r[0::3], r[2::3] = r[2::3], r[0::3]
        rows.append(bytes([0]) + bytes(r))
    raw = b''.join(rows)

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))

    open(dst, 'wb').write(bytes([137, 80, 78, 71, 13, 10, 26, 10]) + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                          + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


for name in sys.argv[1:]:
    convert(name, name[:-4] + '.png')
