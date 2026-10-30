#!/usr/bin/env python3
# Usage (Windows, Python 3 stdlib only):  py alomats.py "<GameData>\Data\models.meg" -o eaw_alo_materials.json
"""alomats.py - dump ALO material data (shader + parameters per sub-mesh) straight from a MEG.

Streams the archive by seek (works on the 624 MB EaW models.meg), reads every *.ALO entry,
and walks its chunk tree. It never extracts files to disk.

ALO chunk header: u32 type, u32 size; size bit31 set = container of sub-chunks.
  0x400   mesh            0x401 mesh name (C string)
  0x10100 sub-mesh material container
    0x10101 shader filename (C string)
    0x10102 int param     \\
    0x10103 float param    |  "mini-chunks": u8 id, u8 size, payload
    0x10104 float3 param   |  id 1 = parameter name (C string), id 2 = value
    0x10105 texture param  |  (texture value = C string filename)
    0x10106 float4 param  /
Particle-system ALOs (no 0x400 meshes) are reported with "meshes": [].

  py alomats.py <file.meg> [-o out.json] [--filter GLOB]   (default filter *.ALO)
"""
import fnmatch
import json
import struct
import sys
import zlib  # noqa: F401  (kept stdlib-only; crc not needed here)

PARAM_TYPES = {0x10102: 'int', 0x10103: 'float', 0x10104: 'float3', 0x10105: 'texture', 0x10106: 'float4'}


def meg_index(f):
    f.seek(0)
    a, b = struct.unpack('<II', f.read(8))
    if a in (0xFFFFFFFF, 0x8FFFFFFF):
        raise SystemExit('only v1 MEG supported by this script (EaW/FoC retail are all v1)')
    names = []
    for _ in range(a):
        (ln,) = struct.unpack('<H', f.read(2))
        names.append(f.read(ln).decode('latin-1'))
    recs = f.read(20 * b)
    for i in range(b):
        crc, idx, size, off, ni = struct.unpack_from('<5I', recs, 20 * i)
        yield names[ni], off, size


def cstr(b):
    return b.split(b'\0', 1)[0].decode('latin-1')


def chunks(buf, start, end):
    o = start
    while o + 8 <= end:
        t, s = struct.unpack_from('<II', buf, o)
        container = bool(s & 0x80000000)
        s &= 0x7FFFFFFF
        body = o + 8
        if body + s > end:
            return
        yield t, container, body, body + s
        o = body + s


def minis(buf, start, end):
    o = start
    while o + 2 <= end:
        mid, ms = buf[o], buf[o + 1]
        yield mid, buf[o + 2:o + 2 + ms]
        o += 2 + ms


def param_value(kind, raw):
    if kind == 'texture':
        return cstr(raw)
    if kind == 'int' and len(raw) >= 4:
        return struct.unpack_from('<i', raw)[0]
    n = {'float': 1, 'float3': 3, 'float4': 4}.get(kind, 0)
    if n and len(raw) >= 4 * n:
        v = list(struct.unpack_from('<%df' % n, raw))
        return [round(x, 6) for x in v] if n > 1 else round(v[0], 6)
    return raw.hex()


def parse_material(buf, s, e):
    mat = {'shader': None, 'params': {}}
    for t, _, b0, b1 in chunks(buf, s, e):
        if t == 0x10101:
            mat['shader'] = cstr(buf[b0:b1])
        elif t in PARAM_TYPES:
            name, raw = None, b''
            for mid, payload in minis(buf, b0, b1):
                if mid == 1:
                    name = cstr(payload)
                elif mid == 2:
                    raw = payload
            if name is not None:
                mat['params'][name] = {'type': PARAM_TYPES[t], 'value': param_value(PARAM_TYPES[t], raw)}
    return mat


def parse_alo(buf):
    meshes = []
    for t, cont, b0, b1 in chunks(buf, 0, len(buf)):
        if t == 0x400 and cont:
            mesh = {'name': None, 'materials': []}
            for t2, c2, s2, e2 in chunks(buf, b0, b1):
                if t2 == 0x401:
                    mesh['name'] = cstr(buf[s2:e2])
                elif t2 == 0x10100 and c2:
                    mesh['materials'].append(parse_material(buf, s2, e2))
            meshes.append(mesh)
    return meshes


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    path = argv[0]
    out = argv[argv.index('-o') + 1] if '-o' in argv else None
    pat = argv[argv.index('--filter') + 1].upper() if '--filter' in argv else '*.ALO'
    res, errors = {}, {}
    shaders = {}
    with open(path, 'rb') as f:
        entries = [e for e in meg_index(f) if fnmatch.fnmatchcase(e[0].upper(), pat)]
        for name, off, size in entries:
            f.seek(off)
            buf = f.read(size)
            try:
                meshes = parse_alo(buf)
            except Exception as ex:  # keep going; report per file
                errors[name] = repr(ex)
                continue
            res[name] = {'size': size, 'meshes': meshes}
            for m in meshes:
                for mat in m['materials']:
                    shaders[mat['shader']] = shaders.get(mat['shader'], 0) + 1
    doc = {'source': path, 'alo_count': len(res), 'shader_usage': dict(sorted(shaders.items(), key=lambda kv: -kv[1])),
           'errors': errors, 'alo': res}
    txt = json.dumps(doc, separators=(',', ':'))
    if out:
        with open(out, 'w') as fo:
            fo.write(txt)
        print('%d ALO, %d distinct shaders, %d errors -> %s' % (len(res), len(shaders), len(errors), out))
    else:
        print(txt)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
