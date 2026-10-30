#!/usr/bin/env python3
# Usage (Windows, Python 3 stdlib only):  py megindex.py "F:\...\GameData\Data\models.meg" eaw/models -o models_index.json
"""megindex.py - dump only the header + index of a MEG archive (any size) to JSON.

Reads just the filename/record tables via seeks; payload data is never read, so it works
on archives of any size (e.g. EaW models.meg, 624 MB). Output is the same per-archive
shape as catalog.json ({label: {file_size, version, encrypted, problems, entries}}) and can
be merged with:  python -I megtool.py ...  or by hand.

  py megindex.py <file.meg> [label] [-o out.json]
"""
import json
import os
import struct
import sys
import zlib

MAGIC = 0x3F7D70A4


def read_exact(f, n):
    b = f.read(n)
    if len(b) != n:
        raise ValueError('truncated archive')
    return b


def index(path):
    fsize = os.path.getsize(path)
    out = {'file_size': fsize, 'version': None, 'encrypted': False, 'problems': [], 'entries': []}
    with open(path, 'rb') as f:
        a, b = struct.unpack('<II', read_exact(f, 8))
        rec20_v3 = False
        if a in (0xFFFFFFFF, 0x8FFFFFFF) and b == MAGIC:
            data_start, n_names, n_files, name_tab = struct.unpack('<IIII', read_exact(f, 16))
            if a == 0x8FFFFFFF:
                out['version'], out['encrypted'] = 3, True
                out['problems'].append('encrypted v3 archive: tables not readable without key')
                return out
            if 24 + name_tab + n_files * 20 == data_start:
                out['version'], rec20_v3 = 3, True
            else:
                out['version'] = 2
                f.seek(20)
        else:
            out['version'] = 1
            n_names, n_files = a, b
        if n_names * 2 + n_files * 20 > fsize:
            raise ValueError('header counts exceed file size - not a MEG?')
        names = []
        for _ in range(n_names):
            (ln,) = struct.unpack('<H', read_exact(f, 2))
            names.append(read_exact(f, ln).decode('latin-1'))
        recs = read_exact(f, n_files * 20)
        prev = -1
        for i in range(n_files):
            if rec20_v3:
                flags, crc, idx, size, off, ni = struct.unpack_from('<HIIIIH', recs, i * 20)
            else:
                crc, idx, size, off, ni = struct.unpack_from('<5I', recs, i * 20)
                flags = 0
            name = names[ni] if ni < len(names) else '<bad name index %d>' % ni
            e = {'name': name, 'size': size, 'offset': off, 'crc': '%08X' % crc}
            if flags:
                e['flags'] = flags
            out['entries'].append(e)
            if zlib.crc32(name.upper().replace('/', '\\').encode('latin-1')) & 0xFFFFFFFF != crc:
                out['problems'].append('crc mismatch: ' + name)
            if crc < prev:
                out['problems'].append('unsorted at ' + name)
            prev = crc
            if off + size > fsize:
                out['problems'].append('oob: ' + name)
    return out


def main(argv):
    args = [a for a in argv if a != '-o']
    outp = argv[argv.index('-o') + 1] if '-o' in argv else None
    if outp:
        args.remove(outp)
    if not args:
        print(__doc__)
        return 2
    path = args[0]
    label = args[1] if len(args) > 1 else os.path.splitext(os.path.basename(path))[0]
    res = {label: index(path)}
    txt = json.dumps(res, indent=1)
    if outp:
        with open(outp, 'w') as f:
            f.write(txt)
        r = res[label]
        print('%s: v%s, %d entries, %d problems -> %s' % (label, r['version'], len(r['entries']),
                                                          len(r['problems']), outp))
    else:
        print(txt)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
