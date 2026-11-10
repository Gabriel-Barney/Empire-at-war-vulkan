#!/usr/bin/env python3
"""s78n_extent.py - the target footprint half-extents E of TACTICAL_COMBAT sec. 5.4.2 step 5 for a space unit type,
computed from the installed game's model the way the port computes them (battle_catalog.cpp unit_desc: |model bbox
max.x| x Scale_Factor and |max.y| x Scale_Factor; model bounds as engine/assets model.cpp builds them: every render
submesh's vertex box, moved by its attach bone's bind-pose world matrix unless skinned; collision / shadow-volume /
proxy shaders skipped; hidden meshes kept).

  s78n_extent.py [--game DIR] [TYPE ...]        prints E_x, E_y per type (default: the kit's tractor targets)

range_extents() gives the corrected extents of sec. 5.4.5 (collision-bound half size: model Y along the facing,
model X across) and two facing bones on the model's long axis, used by the second version of the space trials.

Used by build_s78n_mod.py to place the tractor-beam targets, and stored in config.json for the analysis.
Read only; standard library + ../common/ted_owners.py + tools/formats/python/alo.py.
"""
import argparse
import os
import re
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(HERE, '..', 'common'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'formats', 'python'))
import ted_owners  # noqa: E402
import alo  # noqa: E402

FOC_ZIP = os.path.join(ROOT, 'extracted', 'foc_data_text.zip')
TYPE_RE = r'<(\w+)\s+Name="%s"\s*>(.*?)</\1\s*>'


def type_fields(name, fields, zpath=FOC_ZIP):
    """{field: text} of a type from the extracted FoC XML, following Variant_Of_Existing_Type (5 levels)."""
    out = {}
    with zipfile.ZipFile(zpath) as z:
        texts = [z.read(n).decode('latin-1') for n in z.namelist() if n.lower().endswith('.xml')]
    seen = 0
    cur = name
    while cur and seen < 6:
        body = None
        for t in texts:
            m = re.search(TYPE_RE % re.escape(cur), t, re.S)
            if m:
                body = m.group(2)
                break
        if body is None:
            break
        for f in fields:
            if f not in out:
                v = re.search(r'<%s>(.*?)</%s>' % (f, f), body, re.S)
                if v:
                    out[f] = ' '.join(v.group(1).split())
        var = re.search(r'<Variant_Of_Existing_Type>\s*([^<\s]+)\s*<', body)
        cur = var.group(1) if var else None
        seen += 1
    return out


def _mul(a, b):
    """3x4 affine matrices (rows), a * b (b applied first)."""
    out = []
    for r in range(3):
        row = []
        for c in range(4):
            v = sum(a[r][k] * b[k][c] for k in range(3))
            if c == 3:
                v += a[r][3]
            row.append(v)
        out.append(row)
    return out


def _xf(m, p):
    return tuple(m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3] for r in range(3))


def model_bounds(data):
    """(min, max) of the model the port's way, or None."""
    m = alo.parse_alo(data)
    if m.get('kind') != 'model':
        return None
    bones = m['bones']
    world = []
    for b in bones:
        loc = [list(r) for r in b['matrix']]
        p = b.get('parent', -1)
        world.append(loc if p < 0 or p >= len(world) else _mul(world[p], loc))
    if not world:
        world = [[[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]]]
    # object index -> bone (connections: (object, bone)); mesh k is the k-th "mesh" object
    obj_bone = {}
    for o, bone in m['connections']:
        obj_bone.setdefault(o, bone)
    mesh_obj = {}
    for oi, (kind, idx) in enumerate(m['objects']):
        if kind == 'mesh':
            mesh_obj[idx] = oi
    lo = [1e30] * 3
    hi = [-1e30] * 3
    any_box = False
    for mi, mesh in enumerate(m['meshes']):
        bone = obj_bone.get(mesh_obj.get(mi, -1), 0)
        if bone >= len(world):
            bone = 0
        for sm in mesh['submeshes']:
            sh = ((sm.get('material') or {}).get('shader') or '').lower()
            if 'collision' in sh or 'shadowvolume' in sh or sh in ('aldefault.fx', 'default.fx'):
                continue
            verts = sm.get('vertices') or []
            if not verts:
                continue
            fmt = (sm.get('vertex_format') or '')
            skinned = ('skin' in fmt.lower() or 'B4I4' in fmt) and bool(sm.get('bone_map'))
            vlo = [min(v[k] for v in verts) for k in range(3)]
            vhi = [max(v[k] for v in verts) for k in range(3)]
            corners = [(vhi[0] if i & 1 else vlo[0], vhi[1] if i & 2 else vlo[1], vhi[2] if i & 4 else vlo[2])
                       for i in range(8)]
            if not skinned:
                corners = [_xf(world[bone], c) for c in corners]
            for c in corners:
                for k in range(3):
                    lo[k] = min(lo[k], c[k])
                    hi[k] = max(hi[k], c[k])
            any_box = True
    if not any_box:
        return None
    return lo, hi


def _bone_world(m):
    world = []
    for b in m['bones']:
        loc = [list(r) for r in b['matrix']]
        p = b.get('parent', -1)
        world.append(loc if p < 0 or p >= len(world) else _mul(world[p], loc))
    if not world:
        world = [[[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]]]
    return world


def collision_half(data):
    """TACTICAL_COMBAT sec. 5.4.5 (retail 0x12def0): the collision bound of the model, the union of every
    collision-enabled mesh's 0x402 box through its bone's bind-pose matrix (hidden meshes included); returns
    (centre, half) with half = |max - min| / 2, or None when no mesh has the collision bit (retail: half 1)."""
    m = alo.parse_alo(data)
    if m.get('kind') != 'model':
        return None
    world = _bone_world(m)
    obj_bone = {}
    for o, bone in m['connections']:
        obj_bone.setdefault(o, bone)
    mesh_obj = {idx: oi for oi, (kind, idx) in enumerate(m['objects']) if kind == 'mesh'}
    lo = [1e30] * 3
    hi = [-1e30] * 3
    seen = False
    for mi, mesh in enumerate(m['meshes']):
        if not mesh.get('collision') or not mesh.get('bbox'):
            continue
        bone = obj_bone.get(mesh_obj.get(mi, -1), 0)
        if bone >= len(world):
            bone = 0
        vlo, vhi = mesh['bbox']
        for i in range(8):
            corner = (vhi[0] if i & 1 else vlo[0], vhi[1] if i & 2 else vlo[1], vhi[2] if i & 4 else vlo[2])
            c = _xf(world[bone], corner)
            for k in range(3):
                lo[k] = min(lo[k], c[k])
                hi[k] = max(hi[k], c[k])
        seen = True
    if not seen:
        return None
    return [(lo[k] + hi[k]) / 2 for k in range(3)], [abs(hi[k] - lo[k]) / 2 for k in range(3)]


def nose_bones(data):
    """Two bones on the model's long axis for a facing read through Lua Get_Bone_Position: (front, rear, {name:
    (x, y)} bind-pose XY), front = toward model -Y (the nose, sec. 5.4.5 "Axes"). Only bones whose name is unique in
    the model, |x| < 2 and not a particle / fire-point / light bone; missile_target00 / 01 when both qualify. None
    when fewer than two qualify or they are under 50 apart."""
    m = alo.parse_alo(data)
    if m.get('kind') != 'model':
        return None
    world = _bone_world(m)
    names = [b.get('name') or '' for b in m['bones']]
    cand = {}
    for i, n in enumerate(names):
        low = n.lower()
        if not n or names.count(n) != 1 or low.startswith(('p_', 'pe_', 'fp_', 'spot', 'omni')) or ' ' in n:
            continue
        if i >= len(world) or abs(world[i][0][3]) >= 2.0:
            continue
        cand[n] = (world[i][0][3], world[i][1][3])
    if 'missile_target00' in cand and 'missile_target01' in cand:
        pair = sorted(['missile_target00', 'missile_target01'], key=lambda n: cand[n][1])
    elif len(cand) >= 2:
        order = sorted(cand, key=lambda n: cand[n][1])
        pair = [order[0], order[-1]]
    else:
        return None
    front, rear = pair
    if cand[rear][1] - cand[front][1] < 50.0:
        return None
    return front, rear, {front: cand[front], rear: cand[rear]}


def range_extents(game, type_name):
    """sec. 5.4.5 extents and the facing bones of a type: {'type', 'model', 'scale', 'along', 'across', 'half',
    'bones', 'note'}. along = Custom_Hard_XExtent x scale, else the collision-bound model Y half x scale (used when
    |f.u| > 0.7071); across = Custom_Hard_YExtent x scale, else the model X half x scale. None when unreadable."""
    f = type_fields(type_name, ['Space_Model_Name', 'Scale_Factor', 'Custom_Hard_XExtent', 'Custom_Hard_YExtent'])
    sc = float(f.get('Scale_Factor', '1') or 1)
    if not sc > 0:
        sc = 1.0
    res = {'type': type_name, 'model': f.get('Space_Model_Name'), 'scale': sc, 'along': None, 'across': None,
           'half': None, 'bones': None, 'note': ''}
    hx = float(f.get('Custom_Hard_XExtent', '0') or 0)
    hy = float(f.get('Custom_Hard_YExtent', '0') or 0)
    if not res['model']:
        res['note'] = 'no Space_Model_Name'
        return res
    try:
        gf = model_files(game)
        data, src = gf.read('DATA\\ART\\MODELS\\' + res['model'].upper())
        if data is None:
            res['note'] = 'model %s not found in the install' % res['model']
            return res
        b = collision_half(data)
        half = b[1] if b else [1.0, 1.0, 1.0]
        res['half'] = [round(x, 3) for x in half]
        res['along'] = hx * sc if hx > 0 else half[1] * sc
        res['across'] = hy * sc if hy > 0 else half[0] * sc
        res['bones'] = nose_bones(data)
        res['note'] = 'sec. 5.4.5: collision-bound half %s x Scale_Factor %.2f (%s, %s)%s' % (
            res['half'], sc, res['model'], src, '' if b else '; no collision mesh: half 1')
    except (OSError, ValueError, KeyError, IndexError, alo.FormatError) as e:
        res['note'] = 'model read failed: %s' % e
    return res


def model_files(game):
    """ted_owners.GameFiles plus the two Models.meg archives (GameFiles lists patches, maps and config only); the
    patch MEGs stay ahead of Models.meg in each install folder."""
    gf = ted_owners.GameFiles(game)
    out = []
    for sub in ('corruption', 'GameData'):
        out.extend(x for x in gf.megs if x[0].startswith(sub + ':'))
        p = os.path.join(game, sub, 'Data', 'Models.meg')
        if os.path.exists(p):
            out.append((sub + ':Models.meg', p, None))
    gf.megs = out
    return gf


def extents(game, type_name):
    """{'type', 'model', 'scale', 'ex', 'ey', 'min', 'max', 'note'}; ex / ey None when the model is not readable."""
    f = type_fields(type_name, ['Space_Model_Name', 'Scale_Factor', 'Custom_Hard_XExtent', 'Custom_Hard_YExtent'])
    sc = float(f.get('Scale_Factor', '1') or 1)
    if not sc > 0:
        sc = 1.0
    res = {'type': type_name, 'model': f.get('Space_Model_Name'), 'scale': sc, 'ex': None, 'ey': None,
           'min': None, 'max': None, 'note': ''}
    hx = float(f.get('Custom_Hard_XExtent', '0') or 0)
    hy = float(f.get('Custom_Hard_YExtent', '0') or 0)
    if hx > 0 and hy > 0:
        res.update(ex=hx * sc, ey=hy * sc, note='Custom_Hard_X/YExtent x Scale_Factor')
        return res
    if not res['model']:
        res['note'] = 'no Space_Model_Name'
        return res
    try:
        gf = model_files(game)
        data, src = gf.read('DATA\\ART\\MODELS\\' + res['model'].upper())
        if data is None:
            res['note'] = 'model %s not found in the install' % res['model']
            return res
        b = model_bounds(data)
        if b is None:
            res['note'] = 'model %s has no render geometry' % res['model']
            return res
        lo, hi = b
        res['min'] = [round(x, 3) for x in lo]
        res['max'] = [round(x, 3) for x in hi]
        res['ex'] = abs(hi[0]) * sc
        res['ey'] = abs(hi[1]) * sc
        res['note'] = '|model bbox max| x Scale_Factor (%s, %s)' % (res['model'], src)
    except (OSError, ValueError, KeyError, IndexError, alo.FormatError) as e:
        res['note'] = 'model read failed: %s' % e
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=ted_owners.DEFAULT_GAME)
    ap.add_argument('types', nargs='*', default=['Corellian_Corvette', 'Nebulon_B_Frigate', 'Star_Destroyer'])
    a = ap.parse_args()
    for t in a.types:
        r = extents(a.game, t)
        ex = '%.1f' % r['ex'] if r['ex'] is not None else '?'
        ey = '%.1f' % r['ey'] if r['ey'] is not None else '?'
        print('%-22s E_x %7s  E_y %7s  box %s..%s  (%s)' % (t, ex, ey, r['min'], r['max'], r['note']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
