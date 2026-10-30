#!/usr/bin/env python3
"""Group RTTI classes into engine subsystems and collect candidate entry points from
'Class::Method' strings referenced by code. Writes subsystems.json (consumed by docs).
Usage: subsystems.py <exe> <code.pkl> <rtti.json> <out.json>"""
import sys, os, re, json, collections
sys.path.insert(0, os.path.dirname(__file__))
from name_fn import Namer

RULES = [  # first match wins; order matters
    ('lua_script', r'^Lua|LuaVar|LuaUserVar|LuaScript|LuaMember|ThreadValue|GlobalValue|GetEvent|BlockStatus$|BlockingStatus|^Find[A-Z]|^Evaluate|DumpCallStack|Wrapper$|StoryPlot|^Is(Point|Campaign)|WeightedTypeList|^Get(Next|Most|StoryPlot)|^(Wait|Purge|Assemble|Produce|Spawn|Check|Give|Add|Remove|Cancel)[A-Z].*Class$'),
    ('story', r'^Story|StorySubPlot|^TheStory'),
    ('audio', r'SFX|Speech|Music|Audio|Sound|MSS|Bink|Movie|Volume'),
    ('ai_planning', r'^AI|Goal|Plan(Definition|Behavior|Instantiation)|PotentialPlan|InstantiationMarshaller|TaskForce|Budget|FreeStore|TheAI|Template|Learning|TacticalAIManager|ServicedAISystem|ExecutionPathFinder'),
    ('ai_perception', r'Perception|Perceptual|Evaluator|HintZone|TargetLocation|Reachability|FOWSampler|ThreatCenter|ProjectByUnitRange'),
    ('pathfinding_movement', r'PathFinder|Movement|Locomotor|Formation|Facing|Obstacle|DistanceConstraint|Avoid|MovementClass|MoveTo|Move(Object|Through)|Spline|Retreat'),
    ('physics_collision', r'Collision|Cull|TreeCull|TerrainHeight|HeightSampler|Plane(Collision)?Body|BoxCollision'),
    ('combat_targeting', r'Target|HardPoint|Weapon|Projectile|Combatant|Damage|Shield|Death|Explosion|Turret|Bomb|Barrage|Attack|Clash|Debris|Invulnerab|Stun|Burning'),
    ('abilities', r'Ability|SpecialWeapon|SuperWeapon|SpecialAbility'),
    ('gameobject_behaviors', r'Behavior|DataPack|^GameObject|ObjectTracking|^RootClass|^IRootClass|ObjectUnderConstruction|Production|Garrison|Transport|Squadron|Squad|Fleet|Team|Planetary|Planet|Marker|Hint|Beacon|CapturePoint|CashPoint|Reinforce|Landing|Indigenous|Spawn|Upgrade|DummyBehavior'),
    ('simulation_core', r'GameMode|SpaceMode|LandMode|GalacticMode|Frame|Event(Class|Queue|Factory)|ScheduledEvent|Signal|SaveLoad|Chunk(Reader|Writer)|Player(Class|Profile)?$|Faction|GameObjectManager|TheGameScoring|Difficulty|SetupPhase|AutoResolve|IAutoResolve|Interpolator|Cubic|Linear|MainThread|SingletonManager|^Thread|LoadingThread|Random'),
    ('renderer', r'^al|D3d|^Scene|Scene(2d)?Class|RtsScene|Texture|Model|Draw|Camera|Render|Light|Particle|Emitter|Mesh|Shader|ID3DX|LensFlare|Terrain|SkyDome|Fog|FOW|LetterBox|Fade|Cinematic|TextureRect|StringTexture|Teletype|Line(Sub)?Class|WaypointLine|FillRect|Lightning|SurfaceFX|Wind'),
    ('ui', r'GUI|Component|Dialog|Tooltip|Gadget|Button|Icon|Bar|Shell|Log(Window)?|Window|Console|DebugTree|MousePointer|Radar|Select|Command(Bar)?'),
    ('networking', r'Net|Packet|Socket|Connection|Steam|NAT|Lobby|Internet|Chat|IPAddress|Callback|CallResult|Peer'),
    ('file_io', r'File|MegaFile|Database|XML|Profile|Registry|Conversion|EnumConversion'),
    ('memory', r'Pool|Alloc|Heap|RefCount|SmartPtr|DynamicVector|LinkedList|Hash|Map'),
]
SUBSYS_ORDER = [r[0] for r in RULES] + ['misc']

def classify(name):
    m = re.match(r'^(?:SingletonInstance|Singleton)<V?(.*)>$', name)
    if m: return classify(m.group(1))
    base = re.sub(r'<.*', '', name)
    if re.match(r'^(_EngineParam|al[A-Z0-9])', base): return 'renderer'
    for k, pat in RULES:
        if re.search(pat, base): return k
    if re.search(r'<.*(Pool|Alloc|DynamicVector|LinkedList)', name): return 'memory'
    return 'misc'

def main(exe, pkl, rttij, out):
    n = Namer(exe, pkl, rttij)
    rtti = json.load(open(rttij))
    groups = collections.defaultdict(list)
    cls_sub = {}
    for c in rtti:
        s = classify(c['name']); cls_sub[c['name']] = s
        vt = sorted(c['vtables'], key=lambda v: v['this_offset'])
        groups[s].append({'name': c['name'], 'vtable': vt[0]['rva'] if vt else None, 'slots': vt[0]['slots'] if vt else 0,
                          'parent': c.get('parent')})
    # entry points from Class::Method strings
    ep = collections.defaultdict(lambda: collections.defaultdict(set))
    PAT = re.compile(r'\b([A-Z][A-Za-z0-9_]+(?:Class|Wrapper|Manager|System|Behavior|Mode|Impl|Struct)?)::([A-Za-z_~][A-Za-z0-9_]*)')
    for f in n.code:
        for s in n.strs(f, 30):
            for m in PAT.finditer(s):
                cl, me = m.group(1), m.group(2)
                ep[cl][me].add(hex(f))
    eps = collections.defaultdict(list)
    for cl, ms in ep.items():
        s = cls_sub.get(cl) or classify(cl)
        for me, fs in ms.items():
            eps[s].append({'class': cl, 'method': me, 'functions': sorted(fs)[:4]})
    res = {s: {'classes': sorted(groups.get(s, []), key=lambda x: x['name'].lower()),
               'entry_points': sorted(eps.get(s, []), key=lambda x: (x['class'], x['method']))} for s in SUBSYS_ORDER}
    json.dump(res, open(out, 'w'), indent=1)
    for s in SUBSYS_ORDER: print(f'{s:24s} classes={len(res[s]["classes"]):4d} entry_points={len(res[s]["entry_points"])}')

if __name__ == '__main__':
    main(*sys.argv[1:5])
