#!/usr/bin/env python3
"""Filter patch_sigs.json to unique >=12-byte signatures, build per-function context and the
address-locality index used by make_docs.py. Usage: postprocess_patch.py <out_dir> (run from work dir)."""
import sys, os, json, pickle, pefile
sys.path.insert(0, os.path.dirname(__file__))
from name_fn import Namer
from locality import build_index
out = sys.argv[1] if len(sys.argv) > 1 else 'out'
r = json.load(open(f'{out}/patch_sigs.json'))
img = bytes(pefile.PE('bin/StarWarsG_foc.exe', fast_load=True).get_memory_mapped_image())
filt = {k: [h for h in v if h['sig_len'] >= 12 and img.count(img[int(h['rva'], 16):int(h['rva'], 16) + h['sig_len']]) == 1] for k, v in r.items()}
json.dump(filt, open(f'{out}/patch_sigs_filtered.json', 'w'), indent=1)
n = Namer('bin/StarWarsG_foc.exe', f'{out}/code_foc.pkl', f'{out}/rtti_foc.json')
fns = {int(h['function'], 16) for v in filt.values() for h in v if h['function']}
json.dump({hex(f): n.describe(f) for f in sorted(fns)}, open(f'{out}/patched_fn_context.json', 'w'), indent=1)
pickle.dump(build_index(n), open(f'{out}/locality.pkl', 'wb'))
print(sum(len(v) for v in filt.values()), 'verified hook sites in', len(fns), 'functions')
