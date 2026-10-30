#!/usr/bin/env python3
"""Linear-disassemble every .pdata function and record a compact event stream:
  ('lea'|'mov', reg, target_rva)   RIP-relative loads (strings/vtables/funcs/globals)
  ('call', target_rva)             direct calls
  ('icall', op_str)                indirect calls
  ('jmp', target_rva)              direct tail jumps out of the function
Saves pickle: {fn_start: (fn_end, [events...], [insn_rvas_of_events])}
Usage: scan_code.py <pe> <out.pkl>"""
import sys, pickle, capstone, os
sys.path.insert(0, os.path.dirname(__file__))
import xrefs
from capstone import x86

def scan(path):
    db = xrefs.build(path)
    img, base = db['img'], db['base']
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
    out = {}
    for (s, e) in db['functions']:
        ev = []
        code = bytes(img[s:e])
        for ins in md.disasm(code, base + s):
            a = ins.address - base
            m = ins.mnemonic
            if m == 'call' or m == 'jmp':
                op = ins.operands[0]
                if op.type == x86.X86_OP_IMM:
                    t = op.imm - base
                    if m == 'call' or not (s <= t < e):
                        ev.append((m, t, a))
                elif m == 'call':
                    if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                        ev.append(('icall_rip', ins.address + ins.size + op.mem.disp - base, a))
                    else:
                        ev.append(('icall', ins.op_str, a))
                continue
            if len(ins.operands) == 2:
                o0, o1 = ins.operands
                if o1.type == x86.X86_OP_MEM and o1.mem.base == x86.X86_REG_RIP and o0.type == x86.X86_OP_REG:
                    t = ins.address + ins.size + o1.mem.disp - base
                    ev.append((m, ins.reg_name(o0.reg), t, a))
                elif o0.type == x86.X86_OP_MEM and o0.mem.base == x86.X86_REG_RIP:
                    t = ins.address + ins.size + o0.mem.disp - base
                    ev.append(('store', t, a))
                elif m == 'mov' and o0.type == x86.X86_OP_MEM and o1.type == x86.X86_OP_REG:
                    ev.append(('st', ins.reg_name(o0.mem.base) if o0.mem.base else '', o0.mem.disp, ins.reg_name(o1.reg), a))
                elif m == 'mov' and o0.type == x86.X86_OP_REG and o1.type == x86.X86_OP_REG:
                    ev.append(('mr', ins.reg_name(o0.reg), ins.reg_name(o1.reg), a))
        out[s] = (e, ev)
    return db, out

if __name__ == '__main__':
    db, out = scan(sys.argv[1])
    pickle.dump(out, open(sys.argv[2], 'wb'))
    print(len(out), 'functions scanned;', sum(len(v[1]) for v in out.values()), 'events')
