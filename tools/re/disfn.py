#!/usr/bin/env python3
"""Disassemble a function (by RVA) with string annotations. Usage: disfn.py <pe> <rva_hex> [maxinsns]"""
import sys, capstone, struct
sys.path.insert(0, __import__('os').path.dirname(__file__))
import xrefs
def dis(db, rva, maxn=400):
    img, base = db['img'], db['base']
    f = [x for x in db['functions'] if x[0] <= rva < x[1]]
    start, end = (f[0] if f else (rva, rva + 0x400))
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = False
    out = []
    for i, ins in enumerate(md.disasm(bytes(img[start:end]), base + start)):
        if i >= maxn: break
        ann = ''
        if 'rip' in ins.op_str:
            try:
                d = int(ins.op_str.split('rip')[1].split(']')[0].replace(' ', ''), 16)
                t = ins.address + ins.size + d - base
                s = db['strings'].get(t)
                ann = f'  ; {t:#x} ' + (repr(s['s'][:70]) if s else '')
            except Exception: pass
        out.append(f'{ins.address-base:#x}: {ins.mnemonic} {ins.op_str}{ann}')
    return out
if __name__ == '__main__':
    db = xrefs.build(sys.argv[1])
    print('\n'.join(dis(db, int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 400)))
