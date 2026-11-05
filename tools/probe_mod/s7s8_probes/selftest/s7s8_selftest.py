#!/usr/bin/env python3
"""s7s8_selftest.py - offline check of the S7/S8 probe kit (no game needed).

  s7s8_selftest.py [--game DIR | --source-zip extracted\\foc_data_text.zip]

1. Lua: the hooked BusyTacticalFreeStore.lua (shipped script + S78_CFG + s7s8_hook.lua, exactly what
   build_s7s8_mod.py installs) runs under a stock Lua 5.0.2 (build_lua50.cmd builds out\\tools\\lua50.exe), which is
   also the syntax check. The harness plays the engine: Base_Definitions, then the free-store thread (main) resumed on
   a free-store service (every 3rd frame) only when now - last resume > ServiceRate, On_Unit_Service per AI unit
   every 60 frames; a stub object model (Spawn_Unit, Activate_Ability blocks with IsFinished / Result, Despawn,
   Take_Damage, a carrier that launches two squadrons 150 frames after its spawn, ...) with planted faults:
   Face_Immediate raises (APIERR, then the Turn_To_Face fallback), the type Moldy_Crow is missing (SPAWN ok=0), the
   AT-AT has no DEPLOY (nil, then the DEPLOY_TROOPERS fallback), and a STICKY_BOMB block whose target died stays
   pending until the unit's next Move_To.
   Run A: AI REBEL in Land -> ARM, TICK every 3 frames, the 7 land scenarios start and end in order, the human
   Saboteur is spawned for the Empire player, DONE, ServiceRate back to 20, the shipped FreeStoreService running
   again and the human marker poll still on. Run B: AI REBEL in Space -> the 3 space scenarios in order, escorts
   launched / carrier killed / intruder marks, DONE and the probe off. Run C: AI EMPIRE -> SKIP only, shipped bodies.
   Every record must match s7s8_reader's pattern.
2. Run A + B records, wrapped as a reader log without memory data, go through s7s8_parse.py (Lua-only answers).
3. s7s8_reader.py --selftest and s7s8_parse.py --selftest.
Output: out\\selftest\\ (harness, Lua output, reader-style log, report). Ends with S7S8 SELFTEST OK / FAILED.
"""
import argparse
import os
# the selftest checks the original S7/S8 scenario set and order (the s78f additions run only in game)
os.environ.setdefault("ALAMO_S78_LAND", "SPMAT,ATAT,MDU,STICKY,DT3,REINF,MUZZLE_LAND")
os.environ.setdefault("ALAMO_S78_SPACE", "MUZZLE_SPACE,SQUAD,ESCORT")
import re
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REPO = os.path.abspath(os.path.join(ROOT, '..', '..', '..'))
sys.path.insert(0, ROOT)
import build_s7s8_mod  # noqa: E402
import s7s8_parse  # noqa: E402

OUT = os.path.join(ROOT, 'out', 'selftest')
LUA = os.path.join(ROOT, 'out', 'tools', 'lua50.exe')
RECORD_RE = re.compile(r'S78P1\|([A-Z]+)\|([ -~]{0,385}?)\|END')  # = s7s8_reader.RECORD_RE on text
LAND = ['SPMAT', 'ATAT', 'MDU', 'STICKY', 'DT3', 'REINF', 'MUZZLE_LAND']
SPACE = ['MUZZLE_SPACE', 'SQUAD', 'ESCORT']
# s78g run (README "s78g run")
LAND_G = ['SPMAT_EDGE2', 'SPMAT_CTRL', 'REINF', 'REINF2', 'IDLE47W']
SPACE_G = ['GUARD_HAND', 'ESCORT']
LAND_R = ['SPMAT_EDGE3', 'IDLE47X']  # s78g rerun (2026-11-05); space rerun = GUARD_HAND alone

STUBS = r'''
H = { F = 0, fss = 0, ous = 0, next_id = 100, objs = {}, blocks = {}, mode = "Land", msgs = 0, moves = 0 }
GetCurrentTime = function() return H.F / 30 end
function require(name) end
function Common_Base_Definitions() end
function ScriptExit() end
function PumpEvents() coroutine.yield() end
FindTarget = { Reachable_Target = function() H.fss = H.fss + 1 return nil end }
function EvaluatePerception() return 1 end
function Get_Game_Mode() return H.mode end
function TestValid(o) return o ~= nil and o.Is_Valid() end
function Game_Message(id) H.msgs = H.msgs + 1 end
function Point_Camera_At(o) end

function make_player(id, fac)
	return { Get_ID = function() return id end, Get_Faction_Name = function() return fac end,
		Get_Space_Station = function() return nil end }
end
PlayerObject = make_player(2, "REBEL")
local players = { Hostile = make_player(4, "HOSTILE"), Empire = make_player(1, "EMPIRE"), Rebel = PlayerObject }
function Find_Player(n) return players[n] end

function Create_Position(x, y, z)
	return { Get_XYZ = function() return x, y, z end }
end

local types = {}
for i, n in ipairs({ "SPMAT_WALKER", "T4B_TANK", "AT_AT_WALKER", "REBEL_MOBILE_DEFENSE_UNIT", "REBEL_INFILTRATOR",
	"DARK_TROOPER_PHASEIII", "MZ8_PULSE_CANNON_TANK", "MOLDY_CROW_LANDING", "LUKE_JEDI_XWING_LANDING",
	"UNDERWORLD_INFANTRY_DROID_SHUTTLE", "UNDERWORLD_INFANTRY_DROID_SHUTTLE_LANDING",
	"TYBER_ZANN_BOARDING_SHUTTLE_PROLOGUE", "UM07_CLOUD_CITY_FLYOVER", "UM07_CLOUDCITY_REFINERY", "UNDERWORLD_SABOTEUR",
	"REBEL_X-WING_SQUADRON", "TARTAN_PATROL_CRUISER", "STAR_DESTROYER", "TIE_INTERCEPTOR_SQUADRON",
	"DUMMY_BARRAGE_TARGET", "PROJ_REMOTE_BOMB", "POD_WALKER", "DARK_TROOPER_PHASEII", "BROADSIDE_CLASS_CRUISER",
	"TIE_FIGHTER_SQUADRON" }) do
	types[n] = true
end
function Find_Object_Type(n)
	if types[string.upper(n)] then return { name = string.upper(n) } end
	return nil
end

function Find_All_Objects_Of_Type(x)
	local out = {}
	for i, o in ipairs(H.objs) do
		if o.valid then
			if type(x) == "table" and x.Get_Faction_Name then
				if o.owner == x then table.insert(out, o) end
			elseif type(x) == "string" and o.tname == string.upper(x) then
				table.insert(out, o)
			end
		end
	end
	return out
end

function new_block(fin)
	local b = { fin = fin, done = false }
	b.IsFinished = function() return b.done end
	b.Result = function() return true end
	table.insert(H.blocks, b)
	return b
end

function make_obj(tname, x, y, z, owner)
	H.next_id = H.next_id + 1
	local o = { id = H.next_id, tname = tname, x = x, y = y, z = z, owner = owner, valid = true, hull = 1.0,
		deployed = false, pending = nil, born = H.F }
	o.Is_Valid = function() return o.valid end
	o.Get_Object_ID = function() return o.id end
	o.Get_Position = function() return Create_Position(o.x, o.y, o.z) end
	o.Get_Owner = function() return o.owner end
	o.Get_Hull = function() return o.hull end
	o.Get_Type = function() return { Get_Name = function() return tname end } end
	o.Make_Invulnerable = function(b) end
	o.Set_Cannot_Be_Killed = function(b) end
	o.Prevent_AI_Usage = function(b) end
	o.Prevent_All_Fire = function(b) end
	o.Suspend_Locomotor = function(b) end
	o.Set_Single_Ability_Autofire = function(ab, b) end
	o.Face_Immediate = function(t) error("Face_Immediate not available") end
	o.Turn_To_Face = function(t) end
	o.Move_To = function(t)
		H.moves = H.moves + 1
		if o.pending then o.pending.done = true o.pending = nil end
		o.moving = true
	end
	o.Attack_Target = function(t) if not TestValid(t) then error("bad target") end end
	o.Stop = function() o.moving = false end
	o.Guard_Target = function(t) end
	o.Despawn = function() o.valid = false end
	o.Take_Damage = function(d) o.valid = false end
	o.Is_Category = function(c) return false end
	o.Get_Attack_Target = function() return nil end
	o.Has_Active_Orders = function() H.ous = H.ous + 1 return true end
	o.Is_Ability_Ready = function(ab) return true end
	o.Is_Ability_Active = function(ab) return false end
	o.Activate_Ability = function(ab, arg)
		if ab == "SPOILER_LOCK" or ab == "SPREAD_OUT" then return end
		if o.tname == "AT_AT_WALKER" then return nil end
		if ab == "DEPLOY" then
			if arg == true and o.deployed then return nil end
			o.deployed = (arg == true)
			if o.moving then
				local b = new_block(nil)
				o.pending = b
				return b
			end
			return new_block(H.F + 60)
		end
		if ab == "JET_PACK" then return new_block(H.F + 50) end
		if ab == "STICKY_BOMB" then
			local b = new_block(H.F + 90)
			b.target = arg
			b.unit = o
			return b
		end
		return nil
	end
	table.insert(H.objs, o)
	return o
end

function Spawn_Unit(t, where, player)
	local x, y, z
	if where.Get_Position then x, y, z = where.Get_Position().Get_XYZ() else x, y, z = where.Get_XYZ() end
	return { make_obj(t.name, x, y, z, player) }
end

-- one engine frame: blocks, the carrier's hangar
function H.frame()
	for i, b in ipairs(H.blocks) do
		if not b.done and b.fin == H.F then
			if b.target and not TestValid(b.target) then
				b.unit.pending = b
			else
				b.done = true
			end
		end
	end
	for i, o in ipairs(H.objs) do
		if o.tname == "STAR_DESTROYER" and o.valid and H.F == o.born + 150 then
			make_obj("TIE_INTERCEPTOR_SQUADRON", o.x + 50, o.y, o.z, o.owner)
			make_obj("TIE_INTERCEPTOR_SQUADRON", o.x - 50, o.y, o.z, o.owner)
		end
	end
end
'''

DRIVER = r'''
-- the retail free-store Lua has no math library (s78g 2026-11-05): hide it from the hooked script
local mmod = math.mod
math = nil
dofile(H_SCRIPT)
PlayerObject = make_player(2, H_FACTION)
H.mode = H_MODE
for k = 1, 3 do make_obj("EMPIRE_UNIT", 300 + 40 * k, 300, 0, Find_Player("Empire")) end
Base_Definitions()
local unit = make_obj("REBEL_TANK", 2000, 2000, 10, PlayerObject)
On_Unit_Added(unit)
local co = coroutine.create(main)
local last = -1000
local resumes = 0
for F = 0, H_FRAMES do
	H.F = F
	H.frame()
	if mmod(F, 3) == 0 and (F / 30 - last) > ServiceRate then
		last = F / 30
		resumes = resumes + 1
		local ok, err = coroutine.resume(co)
		if not ok then print("RESUME_ERROR " .. tostring(err)) end
	end
	if mmod(F, 60) == 1 then
		On_Unit_Service(unit)
	end
end
for i, r in ipairs(S78_LOG) do print(r) end
print("STATE service_rate=" .. tostring(ServiceRate) .. " fss=" .. H.fss .. " ous=" .. H.ous .. " resumes=" .. resumes ..
	" done=" .. tostring(S78_DONE) .. " on=" .. tostring(S78_ON) .. " msgs=" .. H.msgs)
'''


def check(cond, what, fails):
    print(('  ok    ' if cond else '  FAIL  ') + what)
    if not cond:
        fails.append(what)


def run_lua(script, faction, mode, frames, tag):
    path = os.path.join(OUT, 'harness_%s.lua' % tag)
    with open(path, 'w') as f:
        f.write(STUBS)
        f.write('H_SCRIPT = [[%s]]\nH_FACTION = "%s"\nH_MODE = "%s"\nH_FRAMES = %d\n' % (script, faction, mode, frames))
        f.write(DRIVER)
    r = subprocess.run([LUA, path], capture_output=True, text=True)
    text = r.stdout + r.stderr
    with open(os.path.join(OUT, 'lua_%s.txt' % tag), 'w') as f:
        f.write(text)
    recs = [line for line in r.stdout.splitlines() if line.startswith('S78P1|')]
    state = next((line for line in r.stdout.splitlines() if line.startswith('STATE')), '')
    return r.returncode, recs, state, text


def kind(r):
    return r.split('|')[1]


def field(r, k):
    for p in r.split('|'):
        if p.startswith(k + '='):
            return p[len(k) + 1:]
    return None


def common_checks(tag, rc, recs, state, text, names, fails):
    check(rc == 0 and 'RESUME_ERROR' not in text, 'run %s ran under Lua 5.0.2 (rc=%d)' % (tag, rc), fails)
    kinds = [kind(r) for r in recs]
    check('ERR' not in kinds and 'SKIP' not in kinds, 'run %s: no ERR / SKIP record%s' % (
        tag, (' (' + next(r for r in recs if kind(r) == 'ERR')[:160] + ')') if 'ERR' in kinds else ''), fails)
    bad = [r for r in recs if not RECORD_RE.fullmatch(r)]
    check(not bad, 'run %s: %d records, all match the reader pattern (longest %d chars)%s' % (
        tag, len(recs), max((len(r) for r in recs), default=0), (' bad: ' + bad[0][:120]) if bad else ''), fails)
    ticks = [int(field(r, 'f')) for r in recs if kind(r) == 'TICK' and field(r, 'src') == 'fss']
    gaps = sorted({ticks[i + 1] - ticks[i] for i in range(len(ticks) - 1)})
    check(len(ticks) >= 8 and gaps == [3], 'run %s: FreeStoreService TICKs every 3 frames (%s)' % (tag, ticks[:5]),
          fails)
    scn = [(field(r, 'scn'), field(r, 'ev')) for r in recs if kind(r) == 'SCN']
    check(scn == [(n, e) for n in names for e in ('start', 'end')], 'run %s: SCN start/end in order: %s' % (
        tag, [s[0] for s in scn if s[1] == 'start']), fails)
    first = next((int(field(r, 'f')) for r in recs if kind(r) == 'SCN'), None)
    check(first is not None and 450 <= first <= 453, 'run %s: first scenario at F=%s (start_frame 450)' % (tag, first),
          fails)
    return kinds


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=build_s7s8_mod.ted_owners.DEFAULT_GAME)
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
        print('S7S8 SELFTEST FAILED')
        return 1
    cfg = build_s7s8_mod.make_config(a.game, build_s7s8_mod.DEFAULT_LAND_MAP, build_s7s8_mod.DEFAULT_SPACE_MAP,
                                     build_s7s8_mod.DEFAULT_FACTION)
    cfg['text'] = True
    data, src = build_s7s8_mod.freestore_text(a.game, cfg, a.source_zip)
    script = os.path.join(OUT, 'BusyTacticalFreeStore.lua')
    with open(script, 'wb') as f:
        f.write(data)
    print('  hooked script from %s (%d bytes), land points_ok=%s, space points_ok=%s' % (
        src, len(data), cfg['land']['points_ok'], cfg['space']['points_ok']))
    check(all(b < 128 for b in data), 'hooked script is ASCII', fails)
    spath = script.replace('\\', '/')

    # run A: land
    rc, recs, state, text = run_lua(spath, 'REBEL', 'Land', 11500, 'land')
    kinds = common_checks('A (land)', rc, recs, state, text, LAND, fails)
    want = {'LOAD', 'ARM', 'TICK', 'HB', 'VICTIMS', 'HUMAN', 'SCN', 'SPAWN', 'SETUP', 'ACT', 'BLK', 'ORDER', 'MARK',
            'COUNT', 'P', 'APIERR', 'DONE'}
    check(want <= set(kinds), 'run A: record kinds %s (missing %s)' % (sorted(set(kinds)), sorted(want - set(kinds))),
          fails)
    hum = next((r for r in recs if kind(r) == 'HUMAN'), '')
    check(field(hum, 'saboteur') == '1' and field(hum, 'units') == '3', 'run A: Saboteur spawned for the Empire player '
                                                                         '(%s)' % hum[:90], fails)
    face = [field(r, 'order') for r in recs if kind(r) == 'ORDER' and field(r, 'tag') == 'spmat']
    apierr = [field(r, 'api') for r in recs if kind(r) == 'APIERR']
    check(face[:2] == ['face_immediate', 'turn_to_face'] and any('face_immediate' in (x or '') for x in apierr),
          'run A: planted Face_Immediate failure -> APIERR, then Turn_To_Face (%s)' % face[:2], fails)
    crow = [r for r in recs if kind(r) == 'SPAWN' and field(r, 'type') == 'Moldy_Crow']
    check(len(crow) == 1 and field(crow[0], 'ok') == '0' and field(crow[0], 'why') == 'no_type',
          'run A: missing type Moldy_Crow -> SPAWN ok=0 why=no_type', fails)
    at = [(field(r, 'blk'), field(r, 'ret')) for r in recs if kind(r) == 'ACT' and field(r, 'tag') == 'atat']
    check(len(at) == 8 and all(x[1] == 'nil' for x in at) and
          [x[0] for x in at if x[0].endswith('_troopers')] == ['deploy1_troopers', 'deploy2_troopers', 'deploy3_troopers'],
          'run A: AT-AT DEPLOY(true) nil -> DEPLOY_TROOPERS fallback, DEPLOY(false) alone (%d calls)' % len(at), fails)
    sp = {field(r, 'blk'): field(r, 'ret') for r in recs if kind(r) == 'ACT' and field(r, 'scn') == 'SPMAT'}
    check(sp == {b: 'block' for b in ('deploy1', 'undeploy1', 'deploy2', 'undeploy2', 'deploy3')},
          'run A: SPMAT DEPLOY calls return blocks (%s)' % sp, fails)
    md = {field(r, 'blk'): field(r, 'ret') for r in recs if kind(r) == 'ACT' and field(r, 'scn') == 'MDU'}
    check(md.get('deploy_again') == 'nil' and md.get('deploy') == 'block', 'run A: MDU DEPLOY again -> nil (%s)' % md,
          fails)
    bm = next((field(r, 'sb_dead') for r in recs if kind(r) == 'MARK' and field(r, 'mark') == 'before_move'), None)
    blk = next((r for r in recs if kind(r) == 'BLK' and field(r, 'blk') == 'sb_dead'), '')
    mv = next((int(field(r, 'f')) for r in recs if kind(r) == 'ORDER' and field(r, 'order') == 'move_away'), None)
    check(bm == 'pending' and blk and mv is not None and 0 <= int(field(blk, 'f')) - mv <= 3,
          'run A: STICKY block of the dead target pending until the move order (mark %s, BLK F=%s, move F=%s)' % (
              bm, field(blk, 'f'), mv), fails)
    pend = [field(r, 'pending_blocks') for r in recs if kind(r) == 'SCN' and field(r, 'ev') == 'end' and
            field(r, 'scn') == 'MDU']
    check(pend == [''], 'run A: no MDU block left pending at the scenario end (%s)' % pend, fails)
    check('service_rate=20' in state and 'done=true' in state and 'on=true' in state,
          'run A after DONE: %s (land keeps the marker poll on)' % state, fails)
    m = re.search(r'fss=(\d+) ous=(\d+)', state)
    check(m is not None and int(m.group(1)) >= 1 and int(m.group(2)) >= 1,
          'run A: shipped FreeStoreService / On_Unit_Service run again after DONE (%s)' % (m.group(0) if m else '-'),
          fails)
    check('msgs=' in state and int(state.split('msgs=')[1].split()[0]) >= 3, 'run A: Game_Message lines sent (%s)' %
          state.split(' ')[-1], fails)

    # run B: space
    rc2, recs2, state2, text2 = run_lua(spath, 'REBEL', 'Space', 8400, 'space')
    k2 = common_checks('B (space)', rc2, recs2, state2, text2, SPACE, fails)
    marks = [field(r, 'mark') for r in recs2 if kind(r) == 'MARK']
    check(['drop1', 'drop2', 'moved', 'carrier_dead', 'guard_point', 'intruder', 'intruder_gone'] == marks,
          'run B: marks %s' % marks, fails)
    cnt = next((r for r in recs2 if kind(r) == 'COUNT' and field(r, 'what') == 'escorts_launched'), '')
    check(field(cnt, 'squadrons') == '2', 'run B: escorts launched counted (%s)' % cnt[:100], fails)
    check('HUMAN' not in k2 and 'done=true' in state2 and 'on=false' in state2, 'run B: no human track; after DONE '
                                                                                 '%s' % state2, fails)

    # run C: another faction
    rc3, recs3, state3, text3 = run_lua(spath, 'EMPIRE', 'Land', 1500, 'empire')
    k3 = [kind(r) for r in recs3]
    m3 = re.search(r'fss=(\d+) ous=(\d+)', state3)
    check(rc3 == 0 and 'RESUME_ERROR' not in text3 and k3 == ['LOAD', 'SKIP'] and m3 and int(m3.group(1)) > 0,
          'run C (EMPIRE): records %s, shipped bodies ran (%s)' % (k3, m3.group(0) if m3 else '-'), fails)

    # runs D / E: the s78g lists (README "s78g run")
    saved = {k: os.environ.get(k) for k in ('ALAMO_S78_LAND', 'ALAMO_S78_SPACE')}
    os.environ['ALAMO_S78_LAND'] = ','.join(LAND_G)
    os.environ['ALAMO_S78_SPACE'] = ','.join(SPACE_G)
    try:
        data_g, _src = build_s7s8_mod.freestore_text(a.game, cfg, a.source_zip)
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    script_g = os.path.join(OUT, 'BusyTacticalFreeStore_s78g.lua')
    with open(script_g, 'wb') as f:
        f.write(data_g)
    spath_g = script_g.replace('\\', '/')
    rc4, recs4, state4, text4 = run_lua(spath_g, 'REBEL', 'Land', 13500, 'land_s78g')
    common_checks('D (land s78g)', rc4, recs4, state4, text4, LAND_G, fails)
    m4 = [field(r, 'mark') for r in recs4 if kind(r) == 'MARK' and field(r, 'scn') == 'SPMAT_EDGE2']
    check(m4.count('aim1') == 1 and 'win9' in m4 and len([x for x in m4 if x.startswith('win')]) == 9,
          'run D: SPMAT_EDGE2 9 windows (%d marks)' % len(m4), fails)
    d4 = [field(r, 'ret') for r in recs4 if kind(r) == 'ACT' and field(r, 'scn') == 'SPMAT_EDGE2']
    check(len(d4) == 9 and all(x == 'block' for x in d4), 'run D: one DEPLOY block per EDGE2 window (%s)' % d4[:3],
          fails)
    c4 = [field(r, 'fac') for r in recs4 if kind(r) == 'MARK' and field(r, 'scn') == 'SPMAT_CTRL']
    check(c4 == ['Hostile', 'Empire'], 'run D: SPMAT_CTRL windows Hostile, Empire (%s)' % c4, fails)
    dt = [r for r in recs4 if kind(r) == 'SETUP' and field(r, 'scn') == 'IDLE47W' and 'Set_Single_Ability_Autofire=1'
          in r]
    check(len(dt) == 6, 'run D: IDLE47W JET_PACK autofire off on the six Dark_Trooper_PhaseII (%d)' % len(dt), fails)
    r2 = [field(r, 'tag') for r in recs4 if kind(r) == 'SPAWN' and field(r, 'scn') == 'REINF2']
    check(r2 == ['victim'] + ['mz8_%d' % k for k in range(1, 7)], 'run D: REINF2 spawns %s' % r2, fails)
    rc5, recs5, state5, text5 = run_lua(spath_g, 'REBEL', 'Space', 9600, 'space_s78g')
    common_checks('E (space s78g)', rc5, recs5, state5, text5, SPACE_G, fails)
    m5 = [field(r, 'mark') for r in recs5 if kind(r) == 'MARK' and field(r, 'scn') == 'GUARD_HAND']
    check(m5 == ['find', 'g_move', 'guard_go', 'shift_go', 'guard_go2', 'shift_go2', 'g_stop'],
          'run E: GUARD_HAND marks %s' % m5, fails)
    legs = [field(r, 'order') for r in recs5 if kind(r) == 'ORDER' and field(r, 'scn') == 'GUARD_HAND']
    gh = [r for r in recs5 if kind(r) == 'COUNT' and field(r, 'what') == 'gh']
    check(legs[:2] == ['leg1', 'leg2'] and 'stop' in legs and len(gh) >= 140, 'run E: G legs %s, %d gh records' % (
        legs, len(gh)), fails)
    held = [r for r in recs5 if kind(r) == 'COUNT' and field(r, 'what') == 'escorts_held']
    em = [field(r, 'mark') for r in recs5 if kind(r) == 'MARK' and field(r, 'scn') == 'ESCORT']
    gp = next((r for r in recs5 if kind(r) == 'SPAWN' and field(r, 'tag') == 'intruder'), '')
    check(held and field(held[0], 'new') == '2' and field(gp, 'how') == 'death_point' and 'intruder_gone' in em,
          'run E: escorts held (%s), intruder at the death point (%s)' % (held[:1], field(gp, 'how')), fails)

    # run F: the s78g land rerun list
    saved = {k: os.environ.get(k) for k in ('ALAMO_S78_LAND', 'ALAMO_S78_SPACE')}
    os.environ['ALAMO_S78_LAND'] = ','.join(LAND_R)
    os.environ['ALAMO_S78_SPACE'] = 'GUARD_HAND'
    try:
        data_r, _src = build_s7s8_mod.freestore_text(a.game, cfg, a.source_zip)
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    script_r = os.path.join(OUT, 'BusyTacticalFreeStore_s78g_rerun.lua')
    with open(script_r, 'wb') as f:
        f.write(data_r)
    rc6, recs6, state6, text6 = run_lua(script_r.replace('\\', '/'), 'REBEL', 'Land', 15000, 'land_s78g_rerun')
    common_checks('F (land s78g rerun)', rc6, recs6, state6, text6, LAND_R, fails)
    m6 = [field(r, 'mark') for r in recs6 if kind(r) == 'MARK' and field(r, 'scn') == 'SPMAT_EDGE3']
    o6 = [field(r, 'order') for r in recs6 if kind(r) == 'ORDER' and field(r, 'scn') == 'SPMAT_EDGE3']
    check(len([x for x in m6 if x.startswith('win')]) == 10 and o6.count('attack_aim') == 10 and
          'attack_ev' not in o6, 'run F: SPMAT_EDGE3 10 windows, aim orders only (%s)' % sorted(set(o6)), fails)
    sx = [field(r, 'tag') for r in recs6 if kind(r) == 'SPAWN' and field(r, 'scn') == 'IDLE47X']
    mx = [field(r, 'mark') for r in recs6 if kind(r) == 'MARK' and field(r, 'scn') == 'IDLE47X']
    check(sx.count('i12') == 2 and sx.count('iv12') == 45 and 'D_on15' in mx and 'V_off15' in mx,
          'run F: IDLE47X 12 attackers in P and D, 15 cycles per phase (i12 %d, iv12 %d)' % (
              sx.count('i12'), sx.count('iv12')), fails)

    print('2. Lua records -> s7s8_parse.py (no memory data)')
    log = os.path.join(OUT, 'reader_lua_only.log')
    with open(log, 'w') as f:
        f.write('S78READER 1 selftest\nGAME pid=0 base=0x0 identity=selftest\n')
        for n, typ, rr in ((1, 1, recs), (2, 2, recs2)):
            f.write('MODE n=%d ptr=0x%d type=%d game_type=3 stack=1 frame=0 gframe=0\n' % (n, n, typ))
            for i, r in enumerate(rr):
                f.write('LUA n=%d frame=99999 addr=0x%x rec=%s\n' % (n, 0x1000 + i, r))
            f.write('MODE_END n=%d last_frame=12000\n' % n)
        f.write('EXIT reason=game_exited modes=2\n')
    res = s7s8_parse.cmd_parse(argparse.Namespace(log=log, out=os.path.join(OUT, 'report_lua_only.txt'),
                                                  json=os.path.join(OUT, 'report_lua_only.json')))
    got = [[r['name'] for r in run['scenarios']] for run in res['runs']]
    check(got == [LAND + ['HUMAN'], SPACE], 'parser sees the scenarios (%s)' % got, fails)
    ans = res['answers']
    check(ans.get('5_deploy_again') == 'NIL' and ans.get('5_target_dies') == 'FINISHES_ON_NEW_ORDER' and
          (ans.get('3_atat') or '').endswith('(DEPLOY ret=nil)'), 'Lua-only answers: again %s, target dies %s, '
                                                                  'AT-AT %s' % (ans.get('5_deploy_again'),
                                                                                ans.get('5_target_dies'),
                                                                                ans.get('3_atat')), fails)
    print('  report: %s' % os.path.join(OUT, 'report_lua_only.txt'))
    log_g = os.path.join(OUT, 'reader_lua_only_s78g.log')
    with open(log_g, 'w') as f:
        f.write('S78READER 1 selftest\nGAME pid=0 base=0x0 identity=selftest\n')
        for n, typ, rr in ((1, 1, recs4), (2, 2, recs5), (3, 1, recs6)):
            f.write('MODE n=%d ptr=0x%d type=%d game_type=3 stack=1 frame=0 gframe=0\n' % (n, n, typ))
            for i, r in enumerate(reversed(rr)):  # address order is not time order
                f.write('LUA n=%d frame=99999 addr=0x%x rec=%s\n' % (n, 0x1000 + i, r))
            f.write('MODE_END n=%d last_frame=14000\n' % n)
        f.write('EXIT reason=game_exited modes=2\n')
    try:
        resg = s7s8_parse.cmd_parse(argparse.Namespace(log=log_g, out=os.path.join(OUT, 'report_lua_only_s78g.txt'),
                                                       json=None))
        gotg = [[r['name'] for r in run['scenarios']] for run in resg['runs']]
        ag = resg['answers']
        check(gotg == [LAND_G + ['HUMAN'], SPACE_G, LAND_R + ['HUMAN']] and 't02_guard_shift_1' in ag and
              '1_edge2' in ag and '1_edge3' in ag and 't03_slot47_clip' in ag and 't03_slot47_clip_x' in ag, 's78g records (reversed) -> parser: %s; answers %s' % (gotg, sorted(ag)), fails)
    except Exception as e:  # noqa: BLE001
        check(False, 's78g records -> parser: %r' % e, fails)

    print('3. reader and parser selftests')
    for script_name in ('s7s8_reader.py', 's7s8_parse.py'):
        r = subprocess.run([sys.executable, '-I', os.path.join(ROOT, script_name), '--selftest'], capture_output=True,
                           text=True)
        last = (r.stdout.strip().splitlines() or ['-'])[-1]
        check(r.returncode == 0 and last.endswith('OK'), '%s --selftest: %s' % (script_name, last), fails)
    print('S7S8 SELFTEST %s' % ('OK' if not fails else 'FAILED (%d)' % len(fails)))
    return 0 if not fails else 1


if __name__ == '__main__':
    sys.exit(main())
