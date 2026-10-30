#!/usr/bin/env python3
"""Recover serialized member lists from ChunkWriter-based Save functions.
Writer API (FoC x64): Begin_Chunk 0x21fe20(w,id) / End_Chunk 0x21feb0(w) / Begin_Micro_Chunk 0x21ffa0(w,id) /
Write 0x2200b0(w, ptr, size[,..]) / End_Micro_Chunk 0x220030(w) / Write_String 0x220140(w, str, ..).
For each Save function we emit (chunk_id, micro_id, this_offset, size) for every member written directly,
plus sub-object saves (calls receiving this+off and the writer). Usage:
  serializer_fields.py <decomp_dir> <out.json> name=rva[,rva...] ...
"""
import sys, os, re, json
sys.path.insert(0, os.path.dirname(__file__))
import decomp_index as di

def parse(body):
    chunk = None; micro = None; out = []; sub = []
    tokens = re.finditer(r'FUN_14021fe20\(\w+,(0x[0-9a-f]+|\d+)\)|FUN_14021ffa0\(\w+,(0x[0-9a-f]+|\d+)\)|'
                         r'FUN_1402200b0\(\w+,(?:\(\w+ \*\))?(?:\(longlong\))?param_1 \+ (0x[0-9a-f]+|\d+),(0x[0-9a-f]+|\d+)|'
                         r'FUN_1402200b0\(\w+,param_1,(0x[0-9a-f]+|\d+)|'
                         r'FUN_140220140\(\w+,(?:\(\w+ \*\))?(?:\(longlong\))?param_1 \+ (0x[0-9a-f]+|\d+)|'
                         r'(FUN_14[0-9a-f]+|[A-Za-z_]\w*::\w+)\((?:\(\w+ \*\))?(?:\(longlong\))?param_1 \+ (0x[0-9a-f]+|\d+),\s*param_2', body)
    for m in tokens:
        if m.group(1): chunk = int(m.group(1), 0); micro = None
        elif m.group(2): micro = int(m.group(2), 0)
        elif m.group(3): out.append({'chunk': chunk, 'micro': micro, 'offset': int(m.group(3), 0), 'size': int(m.group(4), 0), 'kind': 'raw'})
        elif m.group(5): out.append({'chunk': chunk, 'micro': micro, 'offset': 0, 'size': int(m.group(5), 0), 'kind': 'raw'})
        elif m.group(6): out.append({'chunk': chunk, 'micro': micro, 'offset': int(m.group(6), 0), 'size': 0x20, 'kind': 'string'})
        elif m.group(7): sub.append({'chunk': chunk, 'micro': micro, 'offset': int(m.group(8), 0), 'saver': m.group(7), 'kind': 'subobject'})
    return out, sub

def main(ddir, outj, specs):
    idx = di.build(ddir)
    res = {}
    for spec in specs:
        name, rvas = spec.split('=')
        fields, subs = [], []
        for r in rvas.split(','):
            b = di.body(idx, int(r, 16))
            f, s = parse(b); fields += f; subs += s
        res[name] = {'functions': rvas.split(','), 'fields': fields, 'subobjects': subs}
        print(name, len(fields), 'raw fields', len(subs), 'sub-objects')
    json.dump(res, open(outj, 'w'), indent=1)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3:])
