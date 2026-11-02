#!/usr/bin/env python3
"""oi_selftest.py - offline check of the OI ability probe kit (no game needed).

  oi_selftest.py [--game DIR | --source-zip extracted\\foc_data_text.zip]

1. Lua: the hooked BusyTacticalFreeStore.lua (shipped script + OIP_CFG + oi_hook.lua, exactly what build_oi_mod.py
   installs) runs under a stock Lua 5.0.2 (build_lua50.cmd builds out\\tools\\lua50.exe), which is also the syntax
   check. The harness plays the engine: Base_Definitions, then the free-store thread (main) resumed on a free-store
   service (every 3rd frame) only when now - last resume > ServiceRate, On_Unit_Service per AI unit every 60 frames;
   a stub object model (Spawn_Unit, abilities, Despawn, ...) with two planted failures: Activate_Ability with a
   position argument raises (must give APIERR and the next argument kind must still run) and the type "Yoda" is
   missing (the Yoda_Team fallback must be used). A MAL stunned at a water point (z = 0) dies 2 frames later.
   Run A: AI faction REBEL -> ARM, TICK every 3 frames, all 7 scenarios start and end in order, the Rancor is kept
   from POUND for EAT, DONE, ServiceRate back to 20 and the shipped FreeStoreService running again, no ERR, every
   record matches oi_reader's pattern. Run B: AI faction EMPIRE -> SKIP, no ARM, the shipped bodies run.
2. The run A records, wrapped as an oi_reader log without memory data, go through oi_parse.py: 7 scenarios, the
   Lua-only fallback must give MAL_LAND SURVIVES, MAL_SHALLOW / MAL_DEEP DIES_AT_ONCE.
3. oi_reader.py --selftest and oi_parse.py --selftest.
Output: out\\selftest\\ (harness, Lua output, reader-style log, report). Ends with OI SELFTEST OK / FAILED.
"""
import argparse
import os
import re
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REPO = os.path.abspath(os.path.join(ROOT, '..', '..', '..'))
sys.path.insert(0, ROOT)
import build_oi_mod  # noqa: E402
import oi_parse  # noqa: E402

OUT = os.path.join(ROOT, 'out', 'selftest')
LUA = os.path.join(ROOT, 'out', 'tools', 'lua50.exe')
RECORD_RE = re.compile(r'OIP1\|([A-Z]+)\|([ -~]{0,385}?)\|END')  # = oi_reader.RECORD_RE on text

STUBS = r'''
H = { F = 0, fss = 0, ous = 0, next_id = 100, objs = {}, faction = "REBEL" }
GetCurrentTime = function() return H.F / 30 end
function require(name) end
function Common_Base_Definitions() end
function ScriptExit() end
function PumpEvents() coroutine.yield() end
FindTarget = { Reachable_Target = function() H.fss = H.fss + 1 return nil end }
function EvaluatePerception() return 1 end
function Get_Game_Mode() return "Land" end
function TestValid(o) return o ~= nil and o.Is_Valid() end
function Create_Thread(name) end

function make_player(id, fac)
	return { Get_ID = function() return id end, Get_Faction_Name = function() return fac end }
end
PlayerObject = make_player(0, "REBEL")
local players = { Hostile = make_player(4, "HOSTILE"), Empire = make_player(1, "EMPIRE") }
function Find_Player(n) return players[n] end

function Create_Position(x, y, z)
	return { Get_XYZ = function() return x, y, z end }
end

local types = { MAL_ROCKET_VEHICLE = 1, HAN_SOLO = 1, RANCOR = 1, T4B_TANK = 1, STORMTROOPER = 1,
	REBEL_INFILTRATOR = 1, YODA_TEAM = 1 }
function Find_Object_Type(n)
	if types[string.upper(n)] then return { name = string.upper(n) } end
	return nil
end
function Find_All_Objects_Of_Type(n) return {} end

function make_obj(tname, x, y, z, owner)
	H.next_id = H.next_id + 1
	local o = { id = H.next_id, tname = tname, x = x, y = y, z = z, owner = owner, valid = true, hull = 1.0,
		ready = {}, active = {}, ab_fire = {} }
	o.Is_Valid = function() return o.valid end
	o.Get_Position = function() return Create_Position(o.x, o.y, o.z) end
	o.Get_Owner = function() return o.owner end
	o.Get_Hull = function() return o.hull end
	o.Get_Type = function() return { Get_Name = function() return tname end } end
	o.Make_Invulnerable = function(b) end
	o.Prevent_AI_Usage = function(b) end
	o.Prevent_All_Fire = function(b) end
	o.Suspend_Locomotor = function(b) end
	o.Move_To = function(t) end
	o.Attack_Target = function(t) if not TestValid(t) then error("bad target") end end
	o.Despawn = function() o.valid = false end
	o.Is_Category = function(c) return false end
	o.Get_Attack_Target = function() return nil end
	o.Has_Active_Orders = function() H.ous = H.ous + 1 return true end
	o.Is_Ability_Ready = function(ab) return o.ready[ab] ~= false end
	o.Is_Ability_Active = function(ab) return o.active[ab] == true end
	o.Activate_Ability = function(ab, arg)
		if ab == "SPOILER_LOCK" or ab == "SPREAD_OUT" then return end
		if type(arg) == "table" and arg.Get_XYZ and not arg.Is_Valid then error("position argument not accepted") end
		o.ab_fire[ab] = H.F + 1
	end
	table.insert(H.objs, o)
	return o
end

function Spawn_Unit(t, where, player)
	local x, y, z
	if where.Get_Position then x, y, z = where.Get_Position().Get_XYZ() else x, y, z = where.Get_XYZ() end
	return { make_obj(t.name, x, y, z, player) }
end

-- one engine frame: ability state; Han's stun kills a MAL standing at a water point (z == 0) 2 frames later
function H.frame()
	for i, o in ipairs(H.objs) do
		for ab, f in pairs(o.ab_fire) do
			if H.F == f then
				o.ready[ab] = false
				o.active[ab] = (ab == "BERSERKER")
				if ab == "AREA_EFFECT_STUN" then
					for j, v in ipairs(H.objs) do
						if v.tname == "MAL_ROCKET_VEHICLE" and v.valid and v.z == 0 and
							(v.x - o.x) * (v.x - o.x) + (v.y - o.y) * (v.y - o.y) < 200 * 200 then
							v.die_at = H.F + 2
						end
					end
				end
			end
		end
		if o.die_at == H.F then o.valid = false end
	end
end
'''

DRIVER = r'''
dofile(H_SCRIPT)
PlayerObject = make_player(0, H_FACTION)
Base_Definitions()
local unit = make_obj("REBEL_TANK", 2000, 2000, 10, PlayerObject)
On_Unit_Added(unit)
local co = coroutine.create(main)
local last = -1000
local resumes = 0
for F = 0, H_FRAMES do
	H.F = F
	H.frame()
	if math.mod(F, 3) == 0 and (F / 30 - last) > ServiceRate then
		last = F / 30
		resumes = resumes + 1
		local ok, err = coroutine.resume(co)
		if not ok then print("RESUME_ERROR " .. tostring(err)) end
	end
	if math.mod(F, 60) == 1 then
		On_Unit_Service(unit)
	end
end
for i, r in ipairs(OIP_LOG) do print(r) end
print("STATE service_rate=" .. tostring(ServiceRate) .. " fss=" .. H.fss .. " ous=" .. H.ous .. " resumes=" .. resumes ..
	" done=" .. tostring(OIP_DONE) .. " on=" .. tostring(OIP_ON))
'''


def check(cond, what, fails):
    print(('  ok    ' if cond else '  FAIL  ') + what)
    if not cond:
        fails.append(what)


def run_lua(script, faction, frames, tag):
    path = os.path.join(OUT, 'harness_%s.lua' % tag)
    with open(path, 'w') as f:
        f.write(STUBS)
        f.write('H_SCRIPT = [[%s]]\nH_FACTION = "%s"\nH_FRAMES = %d\n' % (script, faction, frames))
        f.write(DRIVER)
    r = subprocess.run([LUA, path], capture_output=True, text=True)
    text = r.stdout + r.stderr
    with open(os.path.join(OUT, 'lua_%s.txt' % tag), 'w') as f:
        f.write(text)
    recs = [line for line in r.stdout.splitlines() if line.startswith('OIP1|')]
    state = next((line for line in r.stdout.splitlines() if line.startswith('STATE')), '')
    return r.returncode, recs, state, text


def kind(r):
    return r.split('|')[1]


def field(r, k):
    for p in r.split('|'):
        if p.startswith(k + '='):
            return p[len(k) + 1:]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=build_oi_mod.ted_owners.DEFAULT_GAME)
    ap.add_argument('--source-zip')
    a = ap.parse_args()
    if not a.source_zip and not os.path.isdir(os.path.join(a.game, 'corruption')):
        a.source_zip = os.path.join(REPO, 'extracted', 'foc_data_text.zip')
    os.makedirs(OUT, exist_ok=True)
    fails = []
    print('1. Lua 5.0.2 harness')
    if not os.path.exists(LUA):
        subprocess.run(['cmd', '/c', os.path.join(HERE, 'build_lua50.cmd')], capture_output=True)
    if not os.path.exists(LUA):
        print('  FAIL  lua50.exe could not be built (out\\tools\\lua50_build.txt)')
        print('OI SELFTEST FAILED')
        return 1
    cfg = build_oi_mod.map_config(a.game, build_oi_mod.DEFAULT_MAP, build_oi_mod.DEFAULT_FACTION)
    data, src = build_oi_mod.freestore_text(a.game, cfg, a.source_zip)
    script = os.path.join(OUT, 'BusyTacticalFreeStore.lua')
    with open(script, 'wb') as f:
        f.write(data)
    print('  hooked script from %s (%d bytes), map points_ok=%s' % (src, len(data), cfg['points_ok']))
    check(all(b < 128 for b in data), 'hooked script is ASCII', fails)
    if not cfg['points_ok']:
        print('  note: no readable map; the harness then spawns at the AI unit and the water checks are skipped')
    frames = 450 + 3 * 903 + 603 + 903 + 603 + 453 + 900  # + 900: two 20 s services after DONE
    rc, recs, state, text = run_lua(script.replace('\\', '/'), 'REBEL', frames, 'rebel')
    check(rc == 0 and 'RESUME_ERROR' not in text and 'error' not in text.lower().replace('call=error', ''),
          'run A (REBEL) ran under Lua 5.0.2 (rc=%d)' % rc, fails)
    kinds = [kind(r) for r in recs]
    want = {'LOAD', 'ARM', 'TICK', 'HB', 'VICTIMS', 'SCN', 'SPAWN', 'SETUP', 'WATER', 'ACT', 'APIERR', 'FIRED', 'ORDER',
            'P', 'DONE'}
    check(want <= set(kinds), 'record kinds %s (missing %s)' % (sorted(set(kinds)), sorted(want - set(kinds))), fails)
    check('ERR' not in kinds and 'SKIP' not in kinds, 'no ERR / SKIP record', fails)
    bad = [r for r in recs if not RECORD_RE.fullmatch(r)]
    check(not bad, '%d records, all match the reader pattern (longest %d chars)%s' % (
        len(recs), max((len(r) for r in recs), default=0), (' bad: ' + bad[0][:120]) if bad else ''), fails)
    ticks = [int(field(r, 'f')) for r in recs if kind(r) == 'TICK' and field(r, 'src') == 'fss']
    gaps = sorted({ticks[i + 1] - ticks[i] for i in range(len(ticks) - 1)})
    check(len(ticks) >= 8 and gaps == [3], 'FreeStoreService TICKs every 3 frames after ARM (%s)' % ticks[:6], fails)
    scn = [(field(r, 'scn'), field(r, 'ev')) for r in recs if kind(r) == 'SCN']
    names = ['MAL_LAND', 'MAL_SHALLOW', 'MAL_DEEP', 'RANCOR_POUND', 'RANCOR_EAT', 'GRENADE', 'BERSERKER']
    check(scn == [(n, e) for n in names for e in ('start', 'end')], 'SCN start/end in order: %s' % [
        s[0] for s in scn if s[1] == 'start'], fails)
    first_start = next((int(field(r, 'f')) for r in recs if kind(r) == 'SCN'), None)
    check(first_start is not None and 450 <= first_start <= 453, 'first scenario starts at F=%s (start_frame 450)' %
          first_start, fails)
    pound_end = next((r for r in recs if kind(r) == 'SCN' and field(r, 'scn') == 'RANCOR_POUND' and
                      field(r, 'ev') == 'end'), '')
    check(field(pound_end, 'kept') == 'rancor,', 'Rancor kept from POUND for EAT (%s)' % field(pound_end, 'kept'), fails)
    eat_spawns = [r for r in recs if kind(r) == 'SPAWN' and field(r, 'scn') == 'RANCOR_EAT']
    check(len(eat_spawns) == 3 and all(field(r, 'ok') == '1' and field(r, 'how') == 'near' for r in eat_spawns),
          'three troopers spawned next to the kept Rancor', fails)
    yoda = [r for r in recs if kind(r) == 'SPAWN' and field(r, 'tag') == 'yoda']
    check([field(r, 'ok') for r in yoda] == ['0', '1'] and field(yoda[-1], 'type') == 'Yoda_Team',
          'missing type Yoda -> SPAWN ok=0, then Yoda_Team', fails)
    apierr = [r for r in recs if kind(r) == 'APIERR']
    check(any('Activate_Ability:AREA_EFFECT_STUN:position' in (field(r, 'api') or '') for r in apierr),
          'planted Activate_Ability(position) failure recorded as APIERR', fails)
    mal_acts = [(field(r, 'scn'), field(r, 'arg'), field(r, 'call')) for r in recs if kind(r) == 'ACT' and
                field(r, 'ab') == 'AREA_EFFECT_STUN']
    check(mal_acts[:2] == [('MAL_LAND', 'position', 'error'), ('MAL_LAND', 'object', 'ok')] and
          len(mal_acts) == 6, 'stun attempts go on after the failure: %s' % mal_acts, fails)
    fired = [field(r, 'scn') for r in recs if kind(r) == 'FIRED']
    check(fired == ['MAL_LAND', 'MAL_SHALLOW', 'MAL_DEEP', 'GRENADE', 'BERSERKER'], 'FIRED in %s' % fired, fails)
    orders = [r for r in recs if kind(r) == 'ORDER' and field(r, 'scn') == 'MAL_LAND']
    check(len(orders) == 3, 'MAL_LAND move orders after the stun: %d' % len(orders), fails)
    check('service_rate=20' in state and 'done=true' in state and 'on=false' in state,
          'after DONE: %s' % state, fails)
    m = re.search(r'fss=(\d+) ous=(\d+)', state)
    check(m is not None and int(m.group(1)) >= 2 and int(m.group(2)) >= 1,
          'shipped FreeStoreService / On_Unit_Service run again after DONE (%s)' % (m.group(0) if m else '-'), fails)

    rc2, recs2, state2, text2 = run_lua(script.replace('\\', '/'), 'EMPIRE', 1500, 'empire')
    k2 = [kind(r) for r in recs2]
    m2 = re.search(r'fss=(\d+) ous=(\d+)', state2)
    check(rc2 == 0 and 'RESUME_ERROR' not in text2 and k2 == ['LOAD', 'SKIP'] and m2 and int(m2.group(1)) > 0,
          'run B (EMPIRE): records %s, shipped bodies ran (%s)' % (k2, m2.group(0) if m2 else '-'), fails)

    print('2. Lua records -> oi_parse.py (no memory data)')
    log = os.path.join(OUT, 'reader_lua_only.log')
    with open(log, 'w') as f:
        f.write('OIREADER 1 selftest\nGAME pid=0 base=0x0 identity=selftest\n')
        f.write('MODE n=1 ptr=0x1 type=1 game_type=3 stack=1 frame=0 gframe=0\n')
        for i, r in enumerate(recs):
            f.write('LUA n=1 frame=%d addr=0x%x rec=%s\n' % (frames, 0x1000 + i, r))
        f.write('MODE_END n=1 last_frame=%d\nEXIT reason=game_exited modes=1\n' % frames)
    res = oi_parse.cmd_parse(argparse.Namespace(log=log, out=os.path.join(OUT, 'report_lua_only.txt'),
                                                json=os.path.join(OUT, 'report_lua_only.json')))
    run = res['runs'][0] if res['runs'] else {'scenarios': []}
    got = [r['name'] for r in run['scenarios']]
    check(got == names, 'parser sees the 7 scenarios (%s)' % got, fails)
    v = {r['name']: r.get('verdict') for r in run['scenarios'] if r['name'].startswith('MAL_')}
    if cfg['points_ok']:
        check(v == {'MAL_LAND': 'SURVIVES', 'MAL_SHALLOW': 'DIES_AT_ONCE', 'MAL_DEEP': 'DIES_AT_ONCE'},
              'Lua-only verdicts %s' % v, fails)
    print('  report: %s' % os.path.join(OUT, 'report_lua_only.txt'))

    print('3. reader and parser selftests')
    for script_name in ('oi_reader.py', 'oi_parse.py'):
        r = subprocess.run([sys.executable, '-I', os.path.join(ROOT, script_name), '--selftest'], capture_output=True,
                           text=True)
        last = (r.stdout.strip().splitlines() or ['-'])[-1]
        check(r.returncode == 0 and last.endswith('OK'), '%s --selftest: %s' % (script_name, last), fails)
    print('OI SELFTEST %s' % ('OK' if not fails else 'FAILED (%d)' % len(fails)))
    return 0 if not fails else 1


if __name__ == '__main__':
    sys.exit(main())
