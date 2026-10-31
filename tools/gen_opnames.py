#!/usr/bin/env python3
"""Generate src/dxbc/opnames.inc (opcode names in enum order) from the Microsoft header."""
import re
import sys

header, out = sys.argv[1:]
text = open(header).read()
body = text[text.index('typedef enum D3D10_SB_OPCODE_TYPE'):]
body = body[body.index('{') + 1:body.index('} D3D10_SB_OPCODE_TYPE;')]
names = []
for line in body.splitlines():
    line = line.split('//')[0].strip().rstrip(',').strip()
    if not line:
        continue
    assert '=' not in line, line
    name = re.sub(r'^D3D(10|10_1|11|11_1|WDDM1_3)_SB_OPCODE_', '', line).lower()
    names.append(name)
assert names[-1] == 'd3d10_sb_num_opcodes'
names.pop()
open(out, 'w').write(''.join(f'"{n}",\n' for n in names))
print(len(names), 'opcodes')
