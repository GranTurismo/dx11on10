#!/usr/bin/env python3
"""Compare two 24-bit BMP screenshots: prints difference stats and writes an amplified diff BMP.

usage: compare.py <a.bmp> <b.bmp> [diff.bmp]
"""
import struct
import sys


def read_bmp(path):
    data = open(path, 'rb').read()
    offset = struct.unpack_from('<I', data, 10)[0]
    w, h = struct.unpack_from('<ii', data, 18)
    bpp = struct.unpack_from('<H', data, 28)[0]
    assert bpp == 24, path
    row = (w * 3 + 3) & ~3
    return w, h, row, data[offset:offset + row * abs(h)]


def main():
    wa, ha, row, a = read_bmp(sys.argv[1])
    wb, hb, _, b = read_bmp(sys.argv[2])
    assert (wa, ha) == (wb, hb), 'sizes differ'
    total = 0
    differing = 0
    worst = 0
    out = bytearray(len(a))
    for y in range(abs(ha)):
        for x in range(wa):
            i = y * row + x * 3
            d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
            total += d
            worst = max(worst, d)
            if d > 8:
                differing += 1
            v = min(255, d * 8)
            out[i:i + 3] = bytes((v, v, v))
    pixels = wa * abs(ha)
    print(f'mean difference {total / pixels:.3f}, worst {worst}, pixels differing by >8: '
          f'{differing} ({100.0 * differing / pixels:.3f}%)')
    if len(sys.argv) > 3:
        header = open(sys.argv[1], 'rb').read()[:54]
        open(sys.argv[3], 'wb').write(header + bytes(out))


if __name__ == '__main__':
    main()
