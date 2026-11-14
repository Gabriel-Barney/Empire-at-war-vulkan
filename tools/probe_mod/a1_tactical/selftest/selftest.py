#!/usr/bin/env python3
"""selftest.py - offline end-to-end check of both A1 probes (no game needed).

  selftest.py [--game DIR | --source-zip extracted\\foc_data_text.zip] [--keep]

1. Lua: the hooked BusyTacticalFreeStore.lua (shipped script from the install or the zip + fs_hook.lua) runs under a
   stock Lua 5.0.2 (common\\lua50.py) with stubbed engine functions. A harness plays the engine's free-store service
   (every 3rd frame, AI_TACTICAL §5.3) for two battles: land with the float reading of `now`, space with the exact
   reading, and prints the probe's records. Checks: LOAD, BASEDEF, MAIN, FSS, OUS, ADD, FAC present; the shipped
   functions still run (the stubs count their calls).
2. Reader: fake_game.py lays out the documented structures (a campaign land battle: Empire human defender, Underworld
   attacker landing on a Neutral Reinforcement_Point_Plus4_Cap it takes at frame 2, +0x404 0 -> 4; then a space
   battle) and keeps the Lua records in memory from their frame on; a1_reader.py records it (--test-base).
3. Parser: a1_parse.py must report land FSS F=600, 1200 -> "CONFIRMED", float/p0; space FSS F=603 -> "REFUTED",
   exact/p0; first unit pass F=63; the attacker (id 6) owns 0 points at first sight, first owns one at F=2,
   +0x404 = 4 and Base_Population_Capacity = 0 at F=300; the defender's line.
Output: out\\selftest\\ (lua logs, scenario.json, reader.log, report.txt). Ends with SELFTEST OK / FAILED.
"""
import argparse
import json
import os
import subprocess
import sys
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REPO = os.path.abspath(os.path.join(ROOT, '..', '..', '..'))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(os.path.dirname(ROOT), 'common'))
import lua50  # noqa: E402
import a1_parse  # noqa: E402
import build_a1_mod  # noqa: E402

OUT = os.path.join(ROOT, 'out', 'selftest')
LUA = lua50.LUA50
PY = sys.executable

STUBS = r'''
A1T = { now = 0, F = 0, mode = "Land", calls = { fss = 0, ous = 0 } }
GetCurrentTime = setmetatable({ Frame = function() return A1T.F + 2 end }, { __call = function() return A1T.now end })
function require(name) end
function Common_Base_Definitions() end
function ScriptExit() end
function PumpEvents() coroutine.yield() end
FindTarget = { Reachable_Target = function() A1T.calls.fss = A1T.calls.fss + 1 return nil end }
function EvaluatePerception() return 1 end
function Get_Game_Mode() return A1T.mode end
function TestValid(o) return o ~= nil end
PlayerObject = { Get_ID = function() return 6 end, Get_Faction_Name = function() return "UNDERWORLD" end,
	Get_Space_Station = function() return nil end }
A1T.ids = { Rebel = 0, Empire = 1, Pirates = 2, Neutral = 3, Underworld = 6 }
function Find_Player(name)
	local id = A1T.ids[name]
	if id == nil then return nil end
	return { Get_ID = function() return id end }
end
function make_unit(name)
	return {
		Get_Type = function() return { Get_Name = function() return name end } end,
		Is_Category = function(c) return false end,
		Get_Attack_Target = function() return nil end,
		Has_Active_Orders = function() A1T.calls.ous = A1T.calls.ous + 1 return true end,
	}
end
'''

DRIVER = r'''
dofile(A1T_SCRIPT)
A1T.mode = A1T_MODE
Base_Definitions()
local units = { make_unit("UNDERWORLD_MERC"), make_unit("UNDERWORLD_MERC"), make_unit("MZ8_PULSE_CANNON_TANK") }
for i, u in ipairs(units) do On_Unit_Added(u) end
local co = coroutine.create(main)
for i, s in ipairs(A1T_SCHEDULE) do
	A1T.F = s[1]
	A1T.now = s[2]
	if s[3] == 1 then
		local ok, err = coroutine.resume(co)
		if not ok then print("RESUME_ERROR " .. tostring(err)) end
	end
	if s[4] == 1 then
		for j, u in ipairs(units) do On_Unit_Service(u) end
	end
end
for i, r in ipairs(A1P_LOG) do print(r) end
print("CALLS fss=" .. A1T.calls.fss .. " ous=" .. A1T.calls.ous)
'''


def lua_schedule(model, frames):
    fss = set(a1_parse.fires(model, 0, 20.0, frames))
    ous = set(a1_parse.fires(model, 0, 2.0, frames))
    rows = []
    for F in range(0, frames, 3):
        if F in fss or F in ous:
            now = a1_parse.now_of(model, F)
            rows.append('{%d, %.17g, %d, %d}' % (F, float(now), int(F in fss), int(F in ous)))
    return 'A1T_SCHEDULE = {\n' + ',\n'.join(rows) + '\n}\n'


def run_lua(script, mode, model, frames, tag):
    path = os.path.join(OUT, 'harness_%s.lua' % tag)
    with open(path, 'w') as f:
        f.write(STUBS)
        f.write('A1T_SCRIPT = [[%s]]\nA1T_MODE = "%s"\n' % (script, mode))
        f.write(lua_schedule(model, frames))
        f.write(DRIVER)
    r = subprocess.run([LUA, path], capture_output=True, text=True)
    with open(os.path.join(OUT, 'lua_%s.txt' % tag), 'w') as f:
        f.write(r.stdout + r.stderr)
    recs = [line for line in r.stdout.splitlines() if line.startswith('A1P1|')]
    calls = [line for line in r.stdout.splitlines() if line.startswith('CALLS')]
    return r.returncode, recs, calls, r.stdout + r.stderr


def check(cond, what, fails):
    print(('  ok    ' if cond else '  FAIL  ') + what)
    if not cond:
        fails.append(what)


def scenario(land_recs, space_recs):
    def rec_rows(recs):
        rows = []
        for text in recs:
            d = a1_parse.rec_fields(text)
            F = a1_parse.frame_of(d) or 0
            rows.append({'frame': F, 'parts': text.split('|')})
        return rows

    land_objects = [
        {'id': 500, 'type': 'REINFORCEMENT_POINT_PLUS4_CAP', 'owner': 3, 'x': 1760, 'y': 700},
        {'id': 501, 'type': 'REINFORCEMENT_POINT_PLUS3_CAP', 'owner': 3, 'x': 1800, 'y': 2100},
        {'id': 502, 'type': 'REINFORCEMENT_POINT_PLUS3_CAP', 'owner': 3, 'x': 900, 'y': 1500},
        {'id': 510, 'type': 'E_GROUND_BARRACKS', 'owner': 1, 'x': 1820, 'y': 2170},
    ] + [{'type': 'SQUAD_STORMTROOPER', 'owner': 1, 'x': 1823, 'y': 2175} for _ in range(6)] + \
        [{'type': 'SKIRMISH_BUILD_PAD', 'owner': 3, 'x': 1000 + 10 * i, 'y': 1000} for i in range(30)] + \
        [{'type': 'UNDERWORLD_MERC', 'owner': 6, 'x': 1763, 'y': 696, 'frame': 1} for _ in range(4)]
    return {
        'fps': 400,
        'players': [
            {'id': 0, 'faction': 'REBEL', 'popcap': 0},
            {'id': 1, 'faction': 'EMPIRE', 'human': 1, 'popcap': 10},
            {'id': 2, 'faction': 'PIRATES', 'popcap': 0},
            {'id': 3, 'faction': 'NEUTRAL', 'neutral': 1, 'popcap': 0},
            {'id': 6, 'faction': 'UNDERWORLD', 'popcap': 0},
        ],
        'modes': [
            {'type': 0, 'stack': 1, 'frames': 60},
            {'type': 1, 'stack': 2, 'frames': 1300, 'slow_frames': 30, 'objects': land_objects,
             'events': [{'frame': 2, 'kind': 'owner', 'obj': 500, 'value': 6},
                        {'frame': 2, 'kind': 'popcap', 'player': 6, 'value': 4},
                        {'frame': 450, 'kind': 'owner', 'obj': 501, 'value': 1}],
             'records': rec_rows(land_recs)},
            {'type': 0, 'stack': 1, 'frames': 60},
            {'type': 2, 'stack': 1, 'game_type': 3, 'frames': 1300,
             'objects': [{'type': 'UM12_STAR_BASE', 'owner': 6}], 'records': rec_rows(space_recs)},
            {'type': 0, 'stack': 1, 'frames': 60},
        ],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=build_a1_mod.ted_owners.DEFAULT_GAME)
    ap.add_argument('--source-zip')
    a = ap.parse_args()
    if not a.source_zip and not os.path.isdir(os.path.join(a.game, 'corruption')):
        a.source_zip = os.path.join(REPO, 'extracted', 'foc_data_text.zip')
    os.makedirs(OUT, exist_ok=True)
    fails = []
    print('1. Lua 5.0.2 harness')
    if not lua50.find_lua():
        print('  FAIL  lua50.exe could not be built (%s)' % lua50.BUILD_LOG)
        return 1
    data, src = build_a1_mod.freestore_text(a.game, a.source_zip)
    script = os.path.join(OUT, 'BusyTacticalFreeStore.lua')
    with open(script, 'wb') as f:
        f.write(data)
    print('  hooked script from %s (%d bytes)' % (src, len(data)))
    rc, land, calls_l, txt_l = run_lua(script, 'Land', 'float', 1300, 'land')
    check(rc == 0 and 'RESUME_ERROR' not in txt_l, 'land harness ran (rc=%d)' % rc, fails)
    rc2, space, calls_s, txt_s = run_lua(script, 'Space', 'exact', 1300, 'space')
    check(rc2 == 0 and 'RESUME_ERROR' not in txt_s, 'space harness ran (rc=%d)' % rc2, fails)
    kinds = {a1_parse.rec_fields(r)['kind'] for r in land}
    check({'LOAD', 'BASEDEF', 'MAIN', 'FSS', 'OUS', 'ADD', 'FAC'} <= kinds, 'land record kinds %s' % sorted(kinds), fails)
    check(calls_l and 'fss=4' in calls_l[0] and 'ous=' in calls_l[0] and not calls_l[0].endswith('ous=0'),
          'shipped FreeStoreService / On_Unit_Service bodies still run (%s)' % (calls_l[0] if calls_l else '-'), fails)
    fss_l = [a1_parse.frame_of(a1_parse.rec_fields(r)) for r in land if '|FSS|' in r]
    check(fss_l == [600, 1200], 'land FSS frames from the records %s' % fss_l, fails)
    t600 = [a1_parse.rec_fields(r)['t'] for r in land if '|FSS|' in r][:1]
    check(t600 == ['20.0000019'], 'float t printed at F=600: %s' % t600, fails)

    print('2. fake game -> a1_reader.py')
    sc = scenario(land, space)
    sc_path = os.path.join(OUT, 'scenario.json')
    with open(sc_path, 'w') as f:
        json.dump(sc, f)
    caps = os.path.join(OUT, 'caps.json')
    with open(caps, 'w') as f:
        json.dump(build_a1_mod.caps_table(a.game, a.source_zip), f)
    log = os.path.join(OUT, 'reader.log')
    if os.path.exists(log):
        os.remove(log)
    game = subprocess.Popen([PY, '-I', os.path.join(HERE, 'fake_game.py'), sc_path], stdout=subprocess.PIPE, text=True)
    first = game.stdout.readline().split()
    base, pid = first[1], first[3]
    reader = subprocess.Popen([PY, '-I', os.path.join(ROOT, 'a1_reader.py'), '--out', log, '--status',
                               os.path.join(OUT, 'reader_status.json'), '--caps', caps, '--pid', pid,
                               '--test-base', base, '--scan-sec', '0.5', '--first-scan-frame', '150'])
    game.wait(timeout=120)
    reader.wait(timeout=60)
    text = open(log).read()
    check('EXIT reason=game_exited' in text, 'reader saw the game exit', fails)
    check(text.count('MODE n=') == 2, 'reader saw 2 tactical modes', fails)

    print('3. parser')
    res = a1_parse.cmd_parse(argparse.Namespace(log=log, out=os.path.join(OUT, 'report.txt'),
                                                json=os.path.join(OUT, 'report.json')))
    inst = res['freestore']['instances']
    land_i = [i for i in inst if i['mode'] == 'Land']
    space_i = [i for i in inst if i['mode'] == 'Space']
    check(len(land_i) == 1 and land_i[0]['fss'] == [600, 1200] and 'float/p0' in land_i[0]['fss_models']
          and any('CONFIRMED' in v for v in land_i[0]['verdict']), 'land free store: 600, 1200, CONFIRMED, float/p0',
          fails)
    check(len(space_i) == 1 and space_i[0]['fss'] == [603, 1206] and 'exact/p0' in space_i[0]['fss_models']
          and any('REFUTED' in v for v in space_i[0]['verdict']), 'space free store: 603, 1206, REFUTED, exact/p0', fails)
    check(land_i and land_i[0]['ous'][:1] == [63], 'first unit pass F=63', fails)
    bats = res['landing']['battles']
    check(len(bats) == 1 and bats[0]['campaign'], 'one campaign land battle', fails)
    if bats:
        p6 = bats[0]['players'].get('6', {})
        cp300 = [c for c in p6.get('checkpoints', []) if c['frame'] == 300]
        check(p6.get('points_at_first_sight') == 0 and p6.get('first_frame_owning') == 2,
              'attacker 6: 0 points at first sight, owns one from F=2 (%s)' % {k: p6.get(k) for k in (
                  'points_at_first_sight', 'first_frame_owning')}, fails)
        check(cp300 and cp300[0]['popcap'] == 4 and cp300[0]['held'] == 4 and cp300[0]['base'] == 0,
              'attacker 6 at F=300: +0x404 4, held 4, base 0 (%s)' % cp300, fails)
        holders = [pid for pid, pl in bats[0]['players'].items() if pl['first_frame_owning'] is not None]
        check('6' in holders, 'attacker 6 holds a landing point (%s)' % holders, fails)
    print('SELFTEST %s' % ('OK' if not fails else 'FAILED: ' + '; '.join(fails)))
    return 0 if not fails else 1


if __name__ == '__main__':
    sys.exit(main())
