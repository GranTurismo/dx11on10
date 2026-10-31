# Packs PNGs (icon_<size>.png) into a Windows .ico: python3 tools/make_ico.py <png-dir> <out.ico>
import os, struct, sys
src, out = sys.argv[1], sys.argv[2]
sizes = sorted(int(f[5:-4]) for f in os.listdir(src) if f.startswith('icon_') and f.endswith('.png'))
images = [open(os.path.join(src, f'icon_{s}.png'), 'rb').read() for s in sizes]
header = struct.pack('<HHH', 0, 1, len(sizes))
offset = 6 + 16 * len(sizes)
entries = b''
for s, data in zip(sizes, images):
    entries += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
    offset += len(data)
open(out, 'wb').write(header + entries + b''.join(images))
print(f'{out}: {sizes}')
