#!/usr/bin/env python3
"""r1_model_sheet.py - offline part of the R1 hardpoint probe: what the B.10 rule (docs/re/SPACE_AND_OPEN.md B.10, the
port's engine/render/src/hardpoints.cpp) predicts for each hardpoint of one unit type, read from the shipped XML and ALO.

  r1_model_sheet.py [--type Star_Destroyer] [--game DIR] [--source-zip extracted\\foc_data_text.zip] [--json OUT]

For each hardpoint in the type's <HardPoints> list (list order = hardpoint index): its Attachment_Bone, Damage_Decal
mesh (found? hidden in the ALO?), what lies in the Damage_Particles bone subtree (shown on destruction) and in the
Engine_Particles subtree (hidden on destruction), Model_To_Attach (kept by the port) and the Death_Breakoff_Prop model.
Also lists the owner model's _ALT / LOD meshes (the port's damage `alt` index needs them).

Read only. XML: the install's Data\\XML (loose / MEGs, ../common/ted_owners.py GameFiles) or the text zip. Models:
corruption then GameData, loose Data\\Art\\Models first, then 64Patch / Patch2 / Patch / Models MEGs.
This answers only what the data says; the runtime questions (is the attached turret really kept, timing, owner death,
Lua name lookup) need the in-game run (README.md).
"""
import argparse
import json
import os
import re
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PROBE_MOD = os.path.dirname(HERE)
ROOT = os.path.abspath(os.path.join(PROBE_MOD, '..', '..'))
sys.path.insert(0, os.path.join(PROBE_MOD, 'common'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'meg'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'formats', 'python'))
import ted_owners  # noqa: E402
from megtool import MegArchive  # noqa: E402
import alo  # noqa: E402

DEFAULT_GAME = ted_owners.DEFAULT_GAME
TYPE_RE = re.compile(r'<(\w+)\s+Name\s*=\s*"([^"]+)"\s*>(.*?)</\1\s*>', re.S)
HP_TAGS = ('Type', 'Health', 'Is_Destroyable', 'Is_Targetable', 'Model_To_Attach', 'Attachment_Bone', 'Collision_Mesh',
           'Damage_Decal', 'Damage_Particles', 'Engine_Particles', 'Engine_Death_Hide_Engine_Particles',
           'Death_Breakoff_Prop', 'Death_Explosion_Particles')


class XmlSource:
    """All Data\\XML text: from the install (every *.XML the GameFiles index knows) or from the text zip."""

    def __init__(self, game=None, source_zip=None):
        self.texts = []
        if source_zip:
            with zipfile.ZipFile(source_zip) as zf:
                for n in zf.namelist():
                    if n.upper().endswith('.XML'):
                        self.texts.append((n, zf.read(n).decode('latin-1', 'replace')))
            return
        gf = ted_owners.GameFiles(game)
        names = set()
        for i in range(len(gf.megs)):
            _a, idx = gf._arch(i)
            names.update(n for n in idx if n.startswith('DATA\\XML\\') and n.endswith('.XML'))
        for base in gf.loose:
            d = os.path.join(base, 'Data', 'XML')
            if os.path.isdir(d):
                names.update('DATA\\XML\\' + f.upper() for f in os.listdir(d) if f.upper().endswith('.XML'))
        for n in sorted(names):
            data, _src = gf.read(n)
            if data is not None:
                self.texts.append((n, data.decode('latin-1', 'replace')))

    def entities(self):
        """{upper name: (tag, name, body, file)}; the first definition wins (as in OBJECT_TYPES sec. 3.2)."""
        out = {}
        for fname, text in self.texts:
            for tag, name, body in TYPE_RE.findall(text):
                out.setdefault(name.strip().upper(), (tag, name.strip(), body, fname))
        return out


def tag_value(body, tag):
    m = re.search(r'<%s\s*>(.*?)</%s\s*>' % (tag, tag), body, re.S | re.I)
    if m:
        return ' '.join(m.group(1).split())
    if re.search(r'<%s\s*/>' % tag, body, re.I):
        return ''
    return None


def type_field(ents, name, tag, depth=0):
    """A type's field, following Variant_Of_Existing_Type (up to 5 levels)."""
    e = ents.get(name.upper())
    if e is None or depth > 5:
        return None
    v = tag_value(e[2], tag)
    if v is not None:
        return v
    var = tag_value(e[2], 'Variant_Of_Existing_Type')
    return type_field(ents, var, tag, depth + 1) if var else None


class ModelFiles:
    """Data\\Art\\Models\\<name> from the install: corruption, then GameData; loose first, then the MEGs."""

    MEGS = ('64Patch.meg', 'Patch2.meg', 'Patch.meg', 'Models.meg')

    def __init__(self, game):
        self.game = game
        self.archives = []
        for sub in ('corruption', 'GameData'):
            self.archives.append(('loose:' + sub, os.path.join(game, sub, 'Data', 'Art', 'Models')))
            for m in self.MEGS:
                p = os.path.join(game, sub, 'Data', m)
                if os.path.isfile(p):
                    self.archives.append((sub + ':' + m, p))
        self._open = {}

    def read(self, name):
        key = 'DATA\\ART\\MODELS\\' + name.upper()
        for label, path in self.archives:
            if label.startswith('loose:'):
                p = os.path.join(path, name)
                if os.path.isfile(p):
                    return open(p, 'rb').read(), label
                continue
            if path not in self._open:
                a = MegArchive(path)
                self._open[path] = (a, {e.name.upper().replace('/', '\\'): e for e in a.entries})
            a, idx = self._open[path]
            e = idx.get(key)
            if e is not None:
                return a.data[e.offset:e.offset + e.size], label
        return None, None


def mesh_bones(m):
    """Per mesh index its bone (from the 0x602 connections; -1 when not connected)."""
    obj_bone = {}
    for obj, bone in m['connections']:
        obj_bone[obj] = bone
    out = [-1] * len(m['meshes'])
    for oi, (kind, k) in enumerate(m['objects']):
        if kind == 'mesh':
            out[k] = obj_bone.get(oi, -1)
    return out


def in_subtree(m, bone, root):
    """B.10 answer 3: bone == root or root an ancestor, walking up to but not including bone 0."""
    if root <= 0 or bone <= 0:
        return False
    b, n = bone, 0
    while b > 0 and n < 1000:
        if b == root:
            return True
        b = m['bones'][b].get('parent', -1)
        n += 1
    return False


def bone_index(m, name):
    if not name:
        return -1
    for i, b in enumerate(m['bones']):
        if b.get('name', '').upper() == name.upper():
            return i
    return -1


def subtree(m, mb, root):
    meshes = [{'name': m['meshes'][i].get('name', '?'), 'alo_hidden': m['meshes'][i].get('hidden', False)}
              for i in range(len(m['meshes'])) if in_subtree(m, mb[i], root)]
    proxies = [{'name': p['name'], 'bone': m['bones'][p['bone']].get('name', '?') if p['bone'] < len(m['bones']) else '?'}
               for p in m['proxies'] if in_subtree(m, p['bone'], root)]
    return {'meshes': meshes, 'proxies': proxies}


def sheet(type_name, xml, models):
    ents = xml.entities()
    if type_name.upper() not in ents:
        raise SystemExit('type %s not found in the XML' % type_name)
    model_name = type_field(ents, type_name, 'Space_Model_Name') or type_field(ents, type_name, 'Land_Model_Name')
    hps = [h.strip() for h in (type_field(ents, type_name, 'HardPoints') or '').split(',') if h.strip()]
    out = {'type': type_name, 'model': model_name, 'hardpoints': [], 'model_source': None,
           'death_clone': type_field(ents, type_name, 'Death_Clone'),
           'damaged_smoke': type_field(ents, type_name, 'Damaged_Smoke_Asset_Name')}
    m = None
    if models is not None and model_name:
        data, src = models.read(model_name)
        if data is not None:
            m = alo.parse_alo(data)
            out['model_source'] = src
    mb = mesh_bones(m) if m else []
    if m:
        out['alt_meshes'] = sorted({x.get('name', '?') for x in m['meshes'] if '_ALT' in x.get('name', '').upper()})
        out['lod_meshes'] = sorted({x.get('name', '?') for x in m['meshes'] if '_LOD' in x.get('name', '').upper()})
        out['bones'] = len(m['bones'])
        out['meshes'] = len(m['meshes'])
        out['proxies'] = len(m['proxies'])
    for i, name in enumerate(hps):
        e = ents.get(name.upper())
        rec = {'index': i, 'name': name, 'known': e is not None}
        if e:
            for t in HP_TAGS:
                rec[t] = tag_value(e[2], t)
            bp = rec.get('Death_Breakoff_Prop')
            if bp:
                rec['breakoff_model'] = type_field(ents, bp, 'Space_Model_Name')
        if m and e:
            rec['attach_bone_index'] = bone_index(m, rec.get('Attachment_Bone'))
            dec = rec.get('Damage_Decal') or ''
            if dec:
                idx = [k for k, x in enumerate(m['meshes']) if x.get('name', '').upper() == dec.upper()]
                rec['decal'] = {'found': bool(idx), 'mesh_index': idx[0] if idx else None,
                                'alo_hidden': m['meshes'][idx[0]].get('hidden') if idx else None,
                                'bone': (m['bones'][mb[idx[0]]].get('name') if idx and mb[idx[0]] >= 0 else None)}
            dp = bone_index(m, rec.get('Damage_Particles'))
            rec['damage_particles_bone'] = dp
            rec['shown_on_death'] = subtree(m, mb, dp) if dp > 0 else None
            ep = bone_index(m, rec.get('Engine_Particles'))
            rec['engine_particles_bone'] = ep
            hide = (rec.get('Engine_Death_Hide_Engine_Particles') or 'yes').strip().lower() not in ('no', 'false', '0')
            rec['hidden_on_death'] = subtree(m, mb, ep) if ep > 0 and hide else None
        out['hardpoints'].append(rec)
    return out


def text(s):
    lines = ['R1 model sheet: %s, model %s (%s)' % (s['type'], s['model'], s.get('model_source') or 'model not read'),
             '  death clone: %s; damaged smoke: %s' % (s.get('death_clone'), s.get('damaged_smoke'))]
    if 'meshes' in s:
        lines.append('  %d bones, %d meshes, %d particle proxies; _ALT meshes: %s; _LOD meshes: %s' % (
            s['bones'], s['meshes'], s['proxies'], ', '.join(s['alt_meshes']) or 'none', ', '.join(s['lod_meshes']) or 'none'))
    for h in s['hardpoints']:
        lines.append('[%d] %s  %s  health %s  destroyable %s' % (h['index'], h['name'], h.get('Type'), h.get('Health'),
                                                               h.get('Is_Destroyable')))
        lines.append('     attach %s (bone %s)  Model_To_Attach %s  -> port keeps it on destruction' % (
            h.get('Attachment_Bone'), h.get('attach_bone_index'), h.get('Model_To_Attach') or '-'))
        lines.append('     Collision_Mesh %s  breakoff %s (model %s)' % (h.get('Collision_Mesh'), h.get('Death_Breakoff_Prop') or '-',
                                                                     h.get('breakoff_model') or '-'))
        d = h.get('decal')
        lines.append('     Damage_Decal %s: %s' % (h.get('Damage_Decal') or '-', 'n/a' if d is None else (
            'mesh %s, ALO hidden %s, bone %s -> SHOWN on destruction' % (d['mesh_index'], d['alo_hidden'], d['bone'])
            if d['found'] else 'NOT FOUND in the model')))
        for key, label in (('shown_on_death', 'Damage_Particles %s subtree SHOWN' % h.get('Damage_Particles')),
                           ('hidden_on_death', 'Engine_Particles %s subtree HIDDEN' % h.get('Engine_Particles'))):
            st = h.get(key)
            if st is None:
                continue
            lines.append('     %s: meshes [%s] proxies [%s]' % (
                label, ', '.join('%s%s' % (x['name'], '(alo-hidden)' if x['alo_hidden'] else '') for x in st['meshes']),
                ', '.join('%s@%s' % (x['name'], x['bone']) for x in st['proxies'])))
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--type', default='Star_Destroyer')
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--source-zip')
    ap.add_argument('--json')
    a = ap.parse_args()
    xml = XmlSource(None if a.source_zip else a.game, a.source_zip)
    models = ModelFiles(a.game) if os.path.isdir(os.path.join(a.game, 'corruption')) else None
    s = sheet(a.type, xml, models)
    print(text(s))
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(s, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
