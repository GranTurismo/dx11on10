#!/usr/bin/env python3
"""Print the export table of a PE file: one line per export, "ordinal name" (name may be empty)."""
import struct
import sys


def exports(path):
    data = open(path, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    assert data[pe:pe + 4] == b'PE\0\0'
    nsect = struct.unpack_from('<H', data, pe + 6)[0]
    optsize = struct.unpack_from('<H', data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    ddir = opt + (96 if magic == 0x10B else 112)
    exp_rva, exp_size = struct.unpack_from('<II', data, ddir)
    sections = []
    for i in range(nsect):
        s = opt + optsize + i * 40
        vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', data, s + 8)
        sections.append((va, max(vsize, rawsize), rawptr))

    def off(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError(hex(rva))

    e = off(exp_rva)
    base, nfuncs, nnames, funcs, names, ords = struct.unpack_from('<IIIIII', data, e + 16)
    named = {}
    for i in range(nnames):
        name_rva = struct.unpack_from('<I', data, off(names) + 4 * i)[0]
        idx = struct.unpack_from('<H', data, off(ords) + 2 * i)[0]
        end = data.index(b'\0', off(name_rva))
        named[idx] = data[off(name_rva):end].decode()
    result = []
    for idx in range(nfuncs):
        rva = struct.unpack_from('<I', data, off(funcs) + 4 * idx)[0]
        if rva == 0:
            continue
        forward = None
        if exp_rva <= rva < exp_rva + exp_size:
            end = data.index(b'\0', off(rva))
            forward = data[off(rva):end].decode()
        result.append((base + idx, named.get(idx, ''), forward))
    return result


if __name__ == '__main__':
    for ordinal, name, forward in exports(sys.argv[1]):
        print(ordinal, name, ('-> ' + forward) if forward else '')
