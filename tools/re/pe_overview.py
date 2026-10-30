#!/usr/bin/env python3
"""PE overview: sections, imports by DLL, exports, version info, rich header, RTTI/Lua markers.
Usage: pe_overview.py <pe> [--json out.json]"""
import sys, json, re, hashlib
import pefile

def main(path, out=None):
    pe = pefile.PE(path)
    data = open(path, 'rb').read()
    r = {'file': path, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    fh, oh = pe.FILE_HEADER, pe.OPTIONAL_HEADER
    r['machine'] = hex(fh.Machine); r['timestamp'] = fh.TimeDateStamp
    r['image_base'] = hex(oh.ImageBase); r['entry_rva'] = hex(oh.AddressOfEntryPoint)
    r['linker'] = f"{oh.MajorLinkerVersion}.{oh.MinorLinkerVersion}"
    r['subsystem'] = oh.Subsystem
    r['sections'] = [{'name': s.Name.rstrip(b'\0').decode(errors='replace'), 'va': hex(s.VirtualAddress),
                      'vsize': hex(s.Misc_VirtualSize), 'rawsize': hex(s.SizeOfRawData),
                      'flags': hex(s.Characteristics), 'entropy': round(s.get_entropy(), 2)} for s in pe.sections]
    imps = {}
    for e in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
        imps[e.dll.decode()] = [(i.name.decode() if i.name else f'#{i.ordinal}') for i in e.imports]
    for e in getattr(pe, 'DIRECTORY_ENTRY_DELAY_IMPORT', []):
        imps['[delay] ' + e.dll.decode()] = [(i.name.decode() if i.name else f'#{i.ordinal}') for i in e.imports]
    r['imports'] = imps
    if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
        r['exports'] = [{'name': (x.name.decode() if x.name else None), 'ord': x.ordinal, 'rva': hex(x.address)}
                        for x in pe.DIRECTORY_ENTRY_EXPORT.symbols]
    vi = {}
    for fi in getattr(pe, 'FileInfo', []) or []:
        for e in fi:
            for st in getattr(e, 'StringTable', []):
                for k, v in st.entries.items(): vi[k.decode()] = v.decode(errors='replace')
    r['version_info'] = vi
    rich = pe.parse_rich_header()
    if rich:
        vals = rich['values']
        r['rich'] = [{'prodid': vals[i] >> 16, 'build': vals[i] & 0xffff, 'count': vals[i+1]} for i in range(0, len(vals), 2)]
    if hasattr(pe, 'DIRECTORY_ENTRY_DEBUG'):
        dbg = []
        for d in pe.DIRECTORY_ENTRY_DEBUG:
            e = {'type': d.struct.Type}
            if d.entry and hasattr(d.entry, 'PdbFileName'):
                e['pdb'] = d.entry.PdbFileName.rstrip(b'\0').decode(errors='replace')
            dbg.append(e)
        r['debug'] = dbg
    # markers
    r['rtti_type_descriptors'] = len(re.findall(rb'\.\?A[VU][A-Za-z0-9_@?$]+@@', data))
    r['lua_markers'] = sorted(set(m.decode() for m in re.findall(rb'(?:Lua \d\.\d[\.\d]*[^\0]{0,60}|\$Lua[^\0]{0,80}|\$Id: l[a-z]+\.c[^\0]{0,60})', data)))
    r['tbb_markers'] = sorted(set(m.decode(errors='replace') for m in re.findall(rb'tbb[A-Za-z_:]{3,60}', data)))[:50]
    if out:
        json.dump(r, open(out, 'w'), indent=1)
    return r

if __name__ == '__main__':
    out = sys.argv[sys.argv.index('--json') + 1] if '--json' in sys.argv else None
    r = main(sys.argv[1], out)
    print(json.dumps({k: v for k, v in r.items() if k != 'imports'}, indent=1)[:6000])
    for d, f in r['imports'].items(): print(d, len(f))
