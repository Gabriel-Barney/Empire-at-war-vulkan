#!/usr/bin/env python3
"""selftest_cu8g.py - offline check of the cu8g probe kit (no game is started, nothing is installed).

  selftest_cu8g.py [--game DIR] [--source-zip extracted\\foc_data_text.zip] [--lua <lua50.exe>] [--keep]

1. Builds every session into a temp folder: from the install when --game has one (read only), else from the text
   zip (--no-text; maps-hit needs the install's .ted and is skipped then).
2. Every built or patched .xml must parse (xml.etree); a patched shipped file is also compared with its original
   (same element count, only the intended values differ).
3. Every built .lua is compiled by a stock Lua 5.0.2 (tools\\probe_mod\\out\\tools\\lua50.exe, built by
   ..\\common\\build_lua50.cmd on first use): the syntax check the game's Lua 5.0 would do.
4. The probe hooks (C8_CFG + cu8g_common.lua + hook, without the shipped free store) and the galactic story script
   run under a stubbed engine that follows the port model (stick: spawn 0.0 / -1 = XML 5 s / rescan every 36
   frames; lure: pulses every 15 frames for 180; cancel: Home One blocks the tractor beam, removal restores it at
   once; maps: a few map objects). The records go through cu8g_parse.py; the report must give the port-model
   verdicts (t07 CONFIRMED, t09a CONFIRMED) and the maps / cancel sections.
Ends with CU8G SELFTEST OK, or lists the failures (exit 1).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
import lua50  # noqa: E402
import build_cu8g_mod as B  # noqa: E402
import cu8g_parse  # noqa: E402

STUB = r'''
OUT = io.open(LOGFILE, "w")
NOW = 0
FRAME = 0
function GetCurrentTime() return NOW end
function Game_Message(s) end
function Sleep(s) coroutine.yield(s) end
OBJS = {}
NEXT_ID = 100
TYPES = {}
function Find_Object_Type(n) if TYPES[n] == false then return nil end return { name = n, Get_Name = function() return n end } end
function PLAYER(name, human)
	local p = { name = name }
	p.Get_Faction_Name = function() return string.upper(name) end
	p.Is_Human = function() return human end
	p.Get_ID = function() return name end
	return p
end
PLAYERS = {}
function Find_Player(n) return PLAYERS[string.upper(n)] end
function Create_Position(x, y, z) return { x = x, y = y, z = z, Get_XYZ = function() return x, y, z end } end
function DIST(a, b) local dx, dy = a.x - b.x, a.y - b.y return math.sqrt(dx * dx + dy * dy) end
function NEWOBJ(tname, pos, player)
	NEXT_ID = NEXT_ID + 1
	local o = { tname = tname, x = pos.x, y = pos.y, owner = player, id = NEXT_ID, valid = true, hull = 1.0, shield = 1.0,
		hp = 1300, rate = 0, target = nil, ability = { active = false, ready = true, until_f = 0, ready_f = 0 } }
	o.Is_Valid = function() return o.valid end
	o.Get_ID = function() return o.id end
	o.Get_Type = function() return { Get_Name = function() return o.tname end } end
	o.Get_Owner = function() return o.owner end
	o.Get_Position = function() return { Get_XYZ = function() return o.x, o.y, 0 end } end
	o.Get_Hull = function() return o.hull end
	o.Get_Shield = function() return o.shield end
	o.Get_Rate_Of_Damage_Taken = function() return o.rate end
	o.Get_Time_Till_Dead = function() if o.rate <= 0 then return 1e18 end return o.hp / o.rate end
	o.Get_Attack_Target = function() return o.target end
	o.Attack_Target = function(t) o.target = t o.ordered = true end
	o.Get_Distance = function(b) return DIST(o, b) end
	o.Despawn = function() o.valid = false end
	o.Set_Targeting_Stickiness_Time_Threshold = function(v) o.sticky = v end
	o.Activate_Ability = function(ab, arg) return ABILITY_ACTIVATE(o, ab, arg) end
	o.Is_Ability_Ready = function(ab) return o.ability.ready and not o.blocked end
	o.Is_Ability_Active = function(ab) return o.ability.active end
	o.Is_Under_Effects_Of_Ability = function(ab) return o.tractored == true end
	for _, m in ipairs({ "Prevent_AI_Usage", "Prevent_All_Fire", "Make_Invulnerable", "Suspend_Locomotor" }) do
		o[m] = function() end
	end
	table.insert(OBJS, o)
	return o
end
function Spawn_Unit(ty, pos, player) return { NEWOBJ(ty.name, pos, player) } end
function Create_Generic_Object(ty, pos, player) return NEWOBJ(ty.name, pos, player) end
function Find_All_Objects_Of_Type(p)
	local l = {}
	for _, o in ipairs(OBJS) do if o.valid and o.owner == p then table.insert(l, o) end end
	return l
end
function ABILITY_ACTIVATE(o, ab, arg) return nil end
function Get_Game_Mode() return MODE end
-- shipped free store stand-ins
function Base_Definitions() ServiceRate = 20 end
function FreeStoreService() end
function On_Unit_Service(o) end
'''

DRIVE = r'''
Base_Definitions()
function DUMP() for i = 1, table.getn(C8_LOG) do OUT:write(C8_LOG[i] .. "\n") end end
for f = 1, FRAMES do
	FRAME = f
	NOW = f / 30
	WORLD(f)
	if math.mod(f, 3) == 0 then FreeStoreService() end
	if math.mod(f, 60) == 3 then On_Unit_Service(nil) end
end
DUMP()
OUT:close()
'''

# port model of the stickiness rule: override starts 0.0, negative -> type 5 s; rescan every 36 frames
STICK_WORLD = r'''
MODE = "Space"
PLAYERS = { REBEL = PLAYER("Rebel", true), EMPIRE = PLAYER("Empire", false) }
PlayerObject = PLAYERS.EMPIRE
function ROLE(o, t) return o.tname == t end
function WORLD(f)
	for _, F in ipairs(OBJS) do
		if F.valid and F.tname == "Nebulon_B_Frigate" and not F.ordered then
			-- the frigate: acquire the nearest enemy; rescan every 36 frames unless the current target is sticky
			local cur = F.target
			if cur and not cur.valid then cur = nil F.target = nil end
			F.next_scan = F.next_scan or f
			if f >= F.next_scan then
				F.next_scan = f + 36
				local thr = F.sticky or 0.0
				if thr < 0 then thr = 5.0 end
				local sticky = cur and cur.rate > 0 and (cur.hp / cur.rate) <= thr
				if not sticky then
					local best, bs = nil, nil
					for _, e in ipairs(OBJS) do
						if e.valid and e.owner ~= F.owner and DIST(F, e) <= 1000 then
							local s = 3.0
							if e.tname == "TIE_Bomber" then s = 2.0 end
							if e.hull <= 0.33 then s = 0 end
							if bs == nil or s < bs then best, bs = e, s end
						end
					end
					F.target = best
				end
			end
		end
	end
	for _, V in ipairs(OBJS) do
		if V.valid and V.tname == "Tartan_Patrol_Cruiser" then
			local n = 0
			for _, H in ipairs(OBJS) do if H.valid and H.target == V then n = n + 1 end end
			V.rate = n * 110
			V.hp = V.hp - V.rate / 30
			if V.hp > 500 then V.shield = (V.hp - 500) / 800 V.hull = 1.0 else V.shield = 0 V.hull = V.hp / 500 end
			if V.hp <= 0 then V.valid = false end
		end
	end
end
'''

LURE_WORLD = r'''
MODE = "Land"
PLAYERS = { EMPIRE = PLAYER("Empire", true), REBEL = PLAYER("Rebel", false) }
PlayerObject = PLAYERS.REBEL
function ABILITY_ACTIVATE(o, ab, arg)
	if ab == "LURE" and o.ability.ready then
		o.ability.active = true o.ability.ready = false o.ability.until_f = FRAME + 180 o.next_pulse = FRAME
	end
end
function WORLD(f)
	for _, L in ipairs(OBJS) do
		if L.valid and L.ability.active then
			if f >= L.ability.until_f then L.ability.active = false
			elseif f >= L.next_pulse then
				L.next_pulse = f + 15
				for _, v in ipairs(OBJS) do
					if v.valid and v.tname == "AT_ST_Walker" and v.target ~= L and DIST(v, L) <= 350 then v.target = L end
				end
			end
		end
	end
end
'''

CANCEL_WORLD = r'''
MODE = "Space"
PLAYERS = { EMPIRE = PLAYER("Empire", true), REBEL = PLAYER("Rebel", false) }
PlayerObject = PLAYERS.REBEL
function ABILITY_ACTIVATE(o, ab, arg)
	if ab == "TRACTOR_BEAM" and o.ability.ready and not o.blocked and arg then
		o.ability.active = true o.ability.ready = false o.ability.until_f = FRAME + 150 o.ability.ready_f = FRAME + 600
		o.victim = arg
	end
end
function WORLD(f)
	local user = nil
	for _, h in ipairs(OBJS) do if h.valid and h.tname == "Home_One" then user = h end end
	for _, T in ipairs(OBJS) do
		if T.valid and T.tname == "Accuser_Star_Destroyer" then
			T.blocked = user ~= nil and DIST(user, T) < 2000
			if T.ability.active and (f >= T.ability.until_f or T.blocked) then T.ability.active = false end
			if not T.ability.ready and f >= T.ability.ready_f then T.ability.ready = true end
			if T.victim and T.victim.valid then
				T.victim.tractored = T.ability.active
				if T.ability.active and not T.blocked then T.victim.x = T.victim.x - 20 / 30 end
			end
		end
	end
end
'''

MAPS_WORLD = r'''
MODE = "Land"
PLAYERS = { EMPIRE = PLAYER("Empire", false), REBEL = PLAYER("Rebel", true), NEUTRAL = PLAYER("Neutral", false) }
PlayerObject = PLAYERS.EMPIRE
NEWOBJ("Alderaan_Prop_Rock", Create_Position(300, -165, 0), PLAYERS.NEUTRAL)
NEWOBJ("Alderaan_Prop_Tree", Create_Position(3540, 3430, 0), PLAYERS.NEUTRAL)
NEWOBJ("Imperial_Stormtrooper", Create_Position(1000, 2900, 0), PLAYERS.EMPIRE)
function WORLD(f) end
'''

STORY_DRIVE = r'''
MODE = "Galactic"
PLAYERS = { EMPIRE = PLAYER("Empire", false), REBEL = PLAYER("Rebel", true) }
OWNERS = { Abregado_Rae = PLAYERS.REBEL, Jabiim = PLAYERS.EMPIRE, Thyferra = PLAYERS.EMPIRE }
function FindPlanet(n) return { Get_Owner = function() return OWNERS[n] end } end
OnEnter = "OnEnter"
function Create_Thread(name) CO = coroutine.create(_G[name]) end
Definitions()
StoryModeEvents.C8G_Begin("OnEnter")
for s = 1, 40 do
	NOW = s * 2
	if s == 10 then OWNERS.Jabiim = PLAYERS.REBEL end
	if s == 20 then NOW = 120 end
	coroutine.resume(CO)
end
for i = 1, table.getn(C8G_LOG) do OUT:write(C8G_LOG[i] .. "\n") end
OUT:close()
'''


def run_lua(lua, workdir, name, chunks):
    path = os.path.join(workdir, name + '.lua')
    with open(path, 'w', newline='\n') as f:
        f.write('\n'.join(chunks))
    r = subprocess.run([lua, path], capture_output=True, text=True, cwd=workdir)
    return r.returncode, (r.stdout + r.stderr).strip()


def lua_str(p):
    return p.replace('\\', '/')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=B.DEFAULT_GAME)
    ap.add_argument('--source-zip', default=os.path.join(ROOT, 'extracted', 'foc_data_text.zip'))
    ap.add_argument('--lua')
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    fails = []
    lua = a.lua or lua50.find_lua()
    if not lua:
        print('lua50.exe missing and could not be built (see %s)' % lua50.BUILD_LOG)
        return 1
    work = tempfile.mkdtemp(prefix='cu8g_selftest_')
    have_game = os.path.isfile(os.path.join(a.game, 'corruption', 'StarWarsG.exe'))
    print('source:', 'install ' + a.game if have_game else 'text zip ' + a.source_zip)
    cfgs = {}
    src = B.Source(a.game if have_game else None, None if have_game else a.source_zip)
    for s in B.SESSIONS:
        if s == 'maps-hit' and not have_game:
            print('skip maps-hit (needs the install)')
            continue
        out = os.path.join(work, s, B.MOD_NAME)
        try:
            cfgs[s] = B.build(s, a.game, out, None if have_game else a.source_zip, no_text=not have_game)
        except SystemExit as e:
            fails.append('build %s: %s' % (s, e))
            continue
        print('built %-9s %s' % (s, ', '.join(cfgs[s]['files'])))
        # 2. XML
        for rel in cfgs[s]['files']:
            p = os.path.join(out, rel)
            if not rel.lower().endswith('.xml'):
                continue
            try:
                new = ET.parse(p).getroot()
            except ET.ParseError as e:
                fails.append('%s %s: not well-formed: %s' % (s, rel, e))
                continue
            orig_name = {'Planets.xml': B.PLANETS, 'UniqueUnits.xml': B.UNIQUEUNITS,
                         'Units_Land_Underworld_Canderous_Tank.xml': B.CANDEROUS,
                         'CampaignFiles.xml': B.CAMPAIGNFILES}.get(os.path.basename(rel))
            if orig_name:
                data, _ = src.read(orig_name)
                old = ET.fromstring(data)
                n_old, n_new = len(list(old.iter())), len(list(new.iter()))
                base = os.path.basename(rel)
                if base == 'UniqueUnits.xml':   # the un-commented Cancel_Special_Ability block adds its elements
                    ok = n_new > n_old and len(new.findall(".//Cancel_Special_Ability[@Name='Akbar_Cancel_Special_Ability']")) >= 1
                else:
                    ok = n_new == n_old + (1 if base == 'CampaignFiles.xml' else 0)
                if not ok:
                    fails.append('%s %s: %d elements, shipped %d' % (s, rel, n_new, n_old))
        # 3. Lua syntax (stock 5.0.2)
        for rel in cfgs[s]['files']:
            if rel.lower().endswith('.lua'):
                rc, msg = run_lua(lua, work, 'compile', ['assert(loadfile("%s"))' % lua_str(os.path.join(out, rel))])
                if rc != 0:
                    fails.append('%s %s: Lua 5.0 compile failed: %s' % (s, rel, msg))
        if s == 'maps-hit':
            m = open(os.path.join(out, 'Data', 'Art', 'Maps', B.HIT_MAP), 'rb').read()
            orig, _ = src.read('DATA\\ART\\MAPS\\' + B.HIT_SOURCE_MAP)
            diff = [i for i in range(len(m)) if m[i] != orig[i]] if len(m) == len(orig) else None
            if diff is None or len(diff) != 1:
                fails.append('maps-hit map: expected one changed byte, got %s' % (diff if diff is None else len(diff)))
            else:
                print('maps-hit map: one byte changed at %d (faction mini 05: 3 -> %d)' % (diff[0], m[diff[0]]))
    # 4. hooks under the stub engine
    kit = lambda n: open(os.path.join(HERE, n), encoding='ascii').read()
    recs_path = os.path.join(work, 'records.log')
    open(recs_path, 'w').close()
    runs = [('stick', 'stick', ['cu8g_stick.lua'], STICK_WORLD, 9000),
            ('lure', 'abil', ['cu8g_lure.lua'], LURE_WORLD, 2400),
            ('cancel', 'abil', ['cu8g_cancel.lua'], CANCEL_WORLD, 6000),
            ('maps', 'maps-miss', ['cu8g_maps_tact.lua'], MAPS_WORLD, 900)]
    for name, session, hooks, world, frames in runs:
        if session not in cfgs:
            continue
        cfg = cfgs[session]
        log = os.path.join(work, name + '.log')
        chunks = ['LOGFILE = "%s"' % lua_str(log), 'FRAMES = %d' % frames, STUB, B.lua_config(cfg),
                  kit('cu8g_common.lua')] + [kit(h) for h in hooks] + [world, DRIVE]
        rc, msg = run_lua(lua, work, 'run_' + name, chunks)
        if rc != 0:
            fails.append('stub run %s failed: %s' % (name, msg[:500]))
            continue
        with open(log) as f, open(recs_path, 'a') as g:
            g.write(f.read())
    log = os.path.join(work, 'story.log')
    story = kit('cu8g_maps_story.lua').replace('@@SESSION@@', 'maps-miss')
    rc, msg = run_lua(lua, work, 'run_story', ['LOGFILE = "%s"' % lua_str(log), STUB, 'function require(n) end', story, STORY_DRIVE])
    if rc != 0:
        fails.append('story stub run failed: ' + msg[:500])
    else:
        with open(log) as f, open(recs_path, 'a') as g:
            g.write(f.read())
    text, recs = cu8g_parse.report(recs_path, os.path.join(work, 'report.txt'))
    print(text)
    for want in ('VERDICT t07: CONFIRMED', 'VERDICT t09a: CONFIRMED', 'VERDICT t09b', 'VERDICT t08',
                 'tactical battle sid', 'CHG sid'):
        if want not in text:
            fails.append('report lacks %r' % want)
    if 'APIERR' in text or ' ERR ' in text:
        fails.append('stub runs recorded APIERR / ERR (see the report)')
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print('work folder kept:', work)
    if fails:
        print('CU8G SELFTEST FAILED:')
        for x in fails:
            print('  ' + x)
        return 1
    print('CU8G SELFTEST OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
