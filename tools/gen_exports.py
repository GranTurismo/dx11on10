#!/usr/bin/env python3
"""Generate the export forwarding stubs for the proxy d3d11.dll.

Every export of the real d3d11.dll gets a stub that jumps through dx_table[i].
The table starts out pointing at a resolver, which asks dx_resolve_export(i)
for the target (our own implementation or the real system function) and
patches the table, so later calls are a single indirect jump.

usage: gen_exports.py <exports.txt> <x64|x86> <out.S> <out.def> <out.inc>
"""
import sys


def main():
    src, arch, out_s, out_def, out_inc = sys.argv[1:]
    exports = []
    for line in open(src):
        line = line.split('#')[0].strip()
        if line:
            ordinal, name = line.split()
            exports.append((int(ordinal), name))

    x86 = arch == 'x86'
    u = '_' if x86 else ''
    ptr = '.long' if x86 else '.quad'
    size = 4 if x86 else 8
    s = ['    .text']
    for i, _ in enumerate(exports):
        s += [f'    .globl {u}dx_stub_{i}', f'{u}dx_stub_{i}:']
        s.append(f'    jmp *{u}dx_table+{i * size}' + ('' if x86 else '(%rip)'))
    for i, _ in enumerate(exports):
        s.append(f'{u}dx_resolve_{i}:')
        s += [f'    pushl ${i}'] if x86 else [f'    mov ${i}, %eax']
        s.append('    jmp dx_resolve_common')
    if x86:
        s += [
            'dx_resolve_common:',
            '    push %ecx',
            '    push %edx',
            '    pushl 8(%esp)',
            '    call _dx_resolve_export',
            '    add $4, %esp',
            '    pop %edx',
            '    pop %ecx',
            '    add $4, %esp',
            '    jmp *%eax',
        ]
    else:
        s += [
            'dx_resolve_common:',
            '    push %rcx',
            '    push %rdx',
            '    push %r8',
            '    push %r9',
            '    sub $0x68, %rsp',
            '    movdqu %xmm0, 0x20(%rsp)',
            '    movdqu %xmm1, 0x30(%rsp)',
            '    movdqu %xmm2, 0x40(%rsp)',
            '    movdqu %xmm3, 0x50(%rsp)',
            '    mov %eax, %ecx',
            '    call dx_resolve_export',
            '    movdqu 0x20(%rsp), %xmm0',
            '    movdqu 0x30(%rsp), %xmm1',
            '    movdqu 0x40(%rsp), %xmm2',
            '    movdqu 0x50(%rsp), %xmm3',
            '    add $0x68, %rsp',
            '    pop %r9',
            '    pop %r8',
            '    pop %rdx',
            '    pop %rcx',
            '    jmp *%rax',
        ]
    s += ['    .data', f'    .globl {u}dx_table', f'    .balign {size}', f'{u}dx_table:']
    s += [f'    {ptr} {u}dx_resolve_{i}' for i, _ in enumerate(exports)]
    open(out_s, 'w').write('\n'.join(s) + '\n')

    d = ['LIBRARY d3d11.dll', 'EXPORTS']
    d += [f'    {name} = dx_stub_{i} @{ordinal}' for i, (ordinal, name) in enumerate(exports)]
    open(out_def, 'w').write('\n'.join(d) + '\n')

    inc = [f'"{name}",' for _, name in exports]
    open(out_inc, 'w').write('\n'.join(inc) + '\n')


if __name__ == '__main__':
    main()
