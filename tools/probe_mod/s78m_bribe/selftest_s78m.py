#!/usr/bin/env python3
"""selftest_s78m.py - offline check of the s78m t06 bribe probe kit (no game is started, nothing is installed).

  selftest_s78m.py [--game DIR] [--source-zip extracted\\foc_data_text.zip] [--lua <lua50.exe>] [--keep]

1. Builds the mod into a temp folder: from the install when --game has one (read only), else from the text zip
   (--no-text, fallback site).
2. The built BusyTacticalFreeStore.lua is compiled by a stock Lua 5.0.2 (tools\\probe_mod\\out\\tools\\lua50.exe,
   built by ..\\common\\build_lua50.cmd on first use): the syntax check the game's Lua 5.0 would do.
3. The probe (S8_CFG + s78m_bribe.lua, without the shipped free store) runs under a stubbed engine three times:
   port   the port model (cost 25 paid at the fire, container + every member on one frame, recharge 1500, a
          container target refused, range 100 on the target, a refused bribe uses up the order, no recharge);
          every verdict must be MATCH;
   alt    a deliberately different engine (cost 18.75, owner changes 20 frames after the fire and spread over two
          polls, recharge 1200, a container accepted, a refused order kept until the money is back); the cost,
          timing, recharge, container and refused verdicts must be DIFFERS;
   menu   Underworld not human (the main-menu background battle): only SKIP, never ARM.
   Each run's records go through s78m_parse.py.
Ends with S78M SELFTEST OK, or lists the failures (exit 1).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
import lua50  # noqa: E402
import build_s78m_mod as B  # noqa: E402
import s78m_parse  # noqa: E402

STUB = r'''
OUT = io.open(LOGFILE, "w")
NOW = 0
FRAME = 0
function GetCurrentTime() return NOW end
function Game_Message(s) MSGS = (MSGS or "") .. s .. ";" end
function Point_Camera_At(o) CAMERA = o end
OBJS = {}
NEXT_ID = 1000
function PLAYER(name, human, credits)
	local p = { name = name, credits = credits }
	p.Get_Faction_Name = function() return name end
	p.Is_Human = function() return human end
	p.Get_Credits = function() return p.credits end
	p.Give_Money = function(v) p.credits = p.credits + v end
	return p
end
function Find_Player(n) return PLAYERS[string.upper(n)] end
function Create_Position(x, y, z) return { x = x, y = y, z = z } end
function DIST(a, b) local dx, dy = a.x - b.x, a.y - b.y return math.sqrt(dx * dx + dy * dy) end
function TYPE(n)
	local t = { name = n }
	t.Get_Name = function() return string.upper(n) end
	t.Get_Bribe_Cost = function(u) return BRIBE_COST end
	return t
end
function Find_Object_Type(n) return TYPE(n) end
function NEWOBJ(tname, x, y, player, parent)
	NEXT_ID = NEXT_ID + 1
	local o = { tname = tname, x = x, y = y, owner = player, id = NEXT_ID, valid = true, parent = parent,
		ready = true, ready_f = 0, autofire = true, pending = nil, members = {} }
	o.Is_Valid = function() return o.valid end
	if not NOID then
		o.Get_Object_ID = function() return o.id end   -- objects have Get_Object_ID; Get_ID is a PlayerWrapper method
	end
	o.Get_Type = function() return TYPE(o.tname) end
	o.Get_Owner = function() return o.owner end
	o.Get_Position = function()
		if o.nopos then error("Get_Position: broken world") end
		return { Get_XYZ = function() return o.x, o.y, 0 end }
	end
	o.Get_Parent_Object = function()
		if NOID and o.parent then return PROXY(o.parent) end
		return o.parent
	end
	o.Despawn = function() o.valid = false end
	o.Is_Ability_Ready = function(ab) return o.ready end
	o.Is_Ability_Active = function(ab) return false end
	o.Is_Ability_Autofire = function(ab) return o.autofire end
	o.Set_Single_Ability_Autofire = function(ab, v) o.autofire = v end
	o.Activate_Ability = function(ab, arg)
		if arg == nil then error("no-argument Activate_Ability stops a retail script") end
		return ACTIVATE(o, ab, arg)
	end
	for _, m in ipairs({ "Prevent_AI_Usage", "Prevent_All_Fire", "Make_Invulnerable" }) do
		o[m] = function(v) if type(v) ~= "boolean" then error(m .. " needs a bool") end end
	end
	table.insert(OBJS, o)
	return o
end
-- a fresh wrapper each call (no ==, no id): the probe must match parents by position
function PROXY(c) return setmetatable({}, { __index = c }) end
function Spawn_Unit(ty, pos, player)
	if ty.name == "Imperial_Stormtrooper_Squad" then
		SQUADS = (SQUADS or 0) + 1
		local r = {}
		for t = 1, 2 do
			local c = NEWOBJ("Stormtrooper_Team", pos.x, pos.y + (t - 1) * 40, player, nil)
			for i = 1, 9 do
				local m = NEWOBJ("Squad_Stormtrooper", pos.x + i * 3, pos.y + (t - 1) * 40 + i, player, c)
				-- broken world, squads 1-2: no trooper position can be read, so the probe finds no team
				m.nopos = BROKEN and SQUADS <= 2
				table.insert(c.members, m)
			end
			if NOID then table.insert(r, PROXY(c)) else table.insert(r, c) end
		end
		return r
	end
	return { NEWOBJ(ty.name, pos.x, pos.y, player, nil) }
end
function Find_All_Objects_Of_Type(n)
	local l = {}
	for _, o in ipairs(OBJS) do
		if o.valid and ((type(n) == "table" and o.owner == n) or (type(n) == "string" and string.upper(o.tname) == string.upper(n))) then
			table.insert(l, o)
		end
	end
	return l
end
function Get_Game_Mode() return MODE end
function TEAM_OF(o) if o.parent then return o.parent end return o end
function CONVERT(c, player, all)
	c.owner = player
	for _, m in ipairs(c.members) do if m.valid and (all or m == PENDING_FIRST) then m.owner = player end end
end
-- shipped free store stand-ins
function Base_Definitions() ServiceRate = 20 end
function FreeStoreService() end
function On_Unit_Service(o) end
'''

# The port model (TACTICAL_COMBAT sec. 5.4.1 as implemented in engine/tactical/abilities).
PORT_WORLD = r'''
MODE = "Land"
PLAYERS = { UNDERWORLD = PLAYER("Underworld", true, 1000), EMPIRE = PLAYER("Empire", false, 1000) }
PlayerObject = PLAYERS.EMPIRE
BRIBE_COST = 25.0
function ACTIVATE(o, ab, arg)
	if ab ~= "TACTICAL_BRIBE" or not o.ready or not arg.valid then return nil end
	if arg.tname == "Stormtrooper_Team" then return nil end      -- not a combat unit: refused
	if arg.owner == o.owner then return nil end
	o.pending = arg
	return {}
end
function WORLD(f)
	if math.mod(f, 600) == 7 then PLAYERS.UNDERWORLD.credits = PLAYERS.UNDERWORLD.credits + 5 end   -- income
	for _, u in ipairs(OBJS) do
		if u.valid and u.tname == "Tyber_Zann" then
			if not u.ready and f >= u.ready_f then u.ready = true end
			local p = u.pending
			if p and math.mod(f, 3) == 1 then
				if not p.valid then u.pending = nil
				elseif DIST(u, p) <= 100 then
					u.pending = nil
					local pl = u.owner
					if pl.credits >= BRIBE_COST then
						pl.credits = pl.credits - BRIBE_COST
						CONVERT(TEAM_OF(p), pl, true)
						u.ready = false
						u.ready_f = f + 1500
					end                                              -- else refused: the order is used up
				else
					local d = DIST(u, p)
					u.x = u.x + (p.x - u.x) / d * 3
					u.y = u.y + (p.y - u.y) / d * 3
				end
			end
		end
	end
end
'''

# A deliberately different engine: the parser must flag it.
ALT_WORLD = r'''
MODE = "Land"
PLAYERS = { UNDERWORLD = PLAYER("Underworld", true, 1000), EMPIRE = PLAYER("Empire", false, 1000) }
PlayerObject = PLAYERS.EMPIRE
BRIBE_COST = 18.75
LATER = {}
function ACTIVATE(o, ab, arg)
	if ab ~= "TACTICAL_BRIBE" or not o.ready or not arg.valid then return nil end
	if arg.owner == o.owner then return nil end
	o.pending = arg
	return {}
end
function WORLD(f)
	for _, u in ipairs(OBJS) do
		if u.valid and u.tname == "Tyber_Zann" then
			if not u.ready and f >= u.ready_f then u.ready = true end
			local p = u.pending
			if p and math.mod(f, 3) == 1 then
				if DIST(u, p) <= 100 then
					local pl = u.owner
					if pl.credits >= BRIBE_COST then                 -- else the order is kept
						u.pending = nil
						pl.credits = pl.credits - BRIBE_COST
						u.ready = false
						u.ready_f = f + 1200
						table.insert(LATER, { f = f + 20, c = TEAM_OF(p), p = p, pl = pl, all = false })
						table.insert(LATER, { f = f + 26, c = TEAM_OF(p), p = p, pl = pl, all = true })
					end
				else
					local d = DIST(u, p)
					u.x = u.x + (p.x - u.x) / d * 3
					u.y = u.y + (p.y - u.y) / d * 3
				end
			end
		end
	end
	for _, l in ipairs(LATER) do
		if l.f == f then
			PENDING_FIRST = l.p
			if l.all then CONVERT(l.c, l.pl, true) else l.c.owner = l.pl l.p.owner = l.pl end
		end
	end
end
'''

MENU_WORLD = r'''
MODE = "Land"
PLAYERS = { UNDERWORLD = PLAYER("Underworld", false, 1000), EMPIRE = PLAYER("Empire", false, 1000) }
PlayerObject = PLAYERS.EMPIRE
BRIBE_COST = 25.0
function ACTIVATE(o, ab, arg) return nil end
function WORLD(f) end
'''

DRIVE = r'''
Base_Definitions()
for f = 1, FRAMES do
	FRAME = f
	NOW = f / 30
	WORLD(f)
	if math.mod(f, 3) == 0 then FreeStoreService() end
	if math.mod(f, 60) == 2 then On_Unit_Service(nil) end
end
for i = 1, table.getn(S8_LOG) do OUT:write(S8_LOG[i] .. "\n") end
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
    work = tempfile.mkdtemp(prefix='s78m_selftest_')
    have_game = os.path.isfile(os.path.join(a.game, 'corruption', 'StarWarsG.exe'))
    print('source:', 'install ' + a.game + ' (read only)' if have_game else 'text zip ' + a.source_zip)
    out = os.path.join(work, 'mod', B.MOD_NAME)
    cfg = None
    try:
        cfg = B.build(a.game, out, None if have_game else a.source_zip, no_text=not have_game)
        print('built %s; site %s' % (', '.join(cfg['files']), cfg['site_note']))
    except SystemExit as e:
        fails.append('build: %s' % e)
    if cfg:
        for rel in cfg['files']:
            if rel.lower().endswith('.lua'):
                rc, msg = run_lua(lua, work, 'compile', ['assert(loadfile("%s"))' % lua_str(os.path.join(out, rel))])
                if rc != 0:
                    fails.append('%s: Lua 5.0 compile failed: %s' % (rel, msg))
                else:
                    print('compiled %s with stock Lua 5.0.2' % rel)
        if have_game and not any(r.lower().endswith('.dat') for r in cfg['files']):
            fails.append('the install build has no MasterTextFile_English.dat')
        probe = open(os.path.join(HERE, 's78m_bribe.lua'), encoding='ascii').read()
        for bad in ('math.', '_MessagePopup', '#'):
            code = '\n'.join(l.split('--')[0] for l in probe.splitlines())
            if bad in code:
                fails.append('s78m_bribe.lua uses %r (not available / not shown in retail free-store Lua)' % bad)
        results = {}
        worlds = (('port', PORT_WORLD, 7800), ('alt', ALT_WORLD, 7800), ('menu', MENU_WORLD, 900),
                  ('noid', 'NOID = true\n' + PORT_WORLD, 7800), ('broken', 'BROKEN = true\n' + PORT_WORLD, 7800))
        # broken world, trials 3-4: a Lua error inside the trial every tick (retail 20261108-214221: nil 'fact')
        breaker = ('S8_Orig_Poll = S8B_Poll\n'
                   'function S8B_Poll(t, F, full)\n'
                   '\tif t.k >= 3 then local q = {} return F - q.fact end\n'
                   '\treturn S8_Orig_Poll(t, F, full)\nend\n')
        for name, world, frames in worlds:
            log = os.path.join(work, name + '.log')
            chunks = ['LOGFILE = "%s"' % lua_str(log), 'FRAMES = %d' % frames, STUB, world, B.lua_config(cfg), probe,
                      breaker if name == 'broken' else '', DRIVE]
            rc, msg = run_lua(lua, work, 'run_' + name, chunks)
            if rc != 0:
                fails.append('stub run %s failed: %s' % (name, msg[:600]))
                continue
            text, res = s78m_parse.report(log, os.path.join(work, name + '_report.txt'),
                                          os.path.join(work, name + '_report.json'))
            results[name] = (text, res)
            print('=' * 30, name, '=' * 30)
            print(text)
        if 'port' in results:
            text, res = results['port']
            for q in ('Q1 ', 'Q2 ', 'Q3 ', 'Q4 ', 'Q5 ', 'Q6 ', 'Q7 '):
                line = next((v for v in res['verdicts'] if v.startswith(q)), None)
                if line is None or '-> MATCH' not in line:
                    fails.append('port model: %s not MATCH: %s' % (q.strip(), line))
            if 'APIERR' in text or '  ERR ' in text:
                fails.append('port model run recorded APIERR / ERR')
            if not any(s['done'] is not None for s in res['scripts']):
                fails.append('port model run did not reach DONE')
            if 'Q8 by hand: no bribe' not in text:
                fails.append('port model: the manual window should report no bribe')
        if 'noid' in results:
            # no Get_Object_ID and a fresh parent wrapper per call (retail run 20261108-214221 read every id as "?")
            text, res = results['noid']
            for q in ('Q1 ', 'Q2 ', 'Q3 ', 'Q4 ', 'Q5 ', 'Q6 ', 'Q7 '):
                line = next((v for v in res['verdicts'] if v.startswith(q)), None)
                if line is None or '-> MATCH' not in line:
                    fails.append('noid model: %s not MATCH: %s' % (q.strip(), line))
            if 'APIERR' in text or '  ERR ' in text:
                fails.append('noid model run recorded APIERR / ERR')
        if 'broken' in results:
            # every trial fails to find its team: each one ends on its own, the probe reaches DONE, ERRs stay few
            text, res = results['broken']
            if not any(s['done'] is not None for s in res['scripts']):
                fails.append('broken model run did not reach DONE')
            nerr = sum(1 for l in open(os.path.join(work, 'broken.log')) if '|ERR|' in l or l.startswith('S8M|ERR'))
            ntend = sum(1 for l in open(os.path.join(work, 'broken.log')) if 'TEND' in l)
            naborted = sum(1 for l in open(os.path.join(work, 'broken.log')) if 'TEND' in l and 'aborted=errors' in l)
            if nerr > 12:
                fails.append('broken model: %d ERR records (an error must not repeat every tick)' % nerr)
            if ntend < 4:
                fails.append('broken model: %d TEND records, expected one per trial (4)' % ntend)
            if naborted != 2:
                fails.append('broken model: %d trials ended on errors, expected 2 (trials 3 and 4)' % naborted)
            print('broken model: %d ERR, %d TEND (%d aborted on errors) records' % (nerr, ntend, naborted))
        if 'alt' in results:
            text, res = results['alt']
            for q in ('Q1 ', 'Q3 ', 'Q4 ', 'Q5 ', 'Q7 '):
                line = next((v for v in res['verdicts'] if v.startswith(q)), None)
                if line is None or '-> DIFFERS' not in line:
                    fails.append('alt model: %s not DIFFERS: %s' % (q.strip(), line))
            line = next((v for v in res['verdicts'] if v.startswith('Q2 ')), '')
            if '-> MATCH' not in line:
                fails.append('alt model: the whole team converts in the end, Q2 should be MATCH: ' + line)
        if 'menu' in results:
            text, res = results['menu']
            if any(s['armed'] for s in res['scripts']) or not any(s['skip'] for s in res['scripts']):
                fails.append('menu run: expected SKIP and no ARM')
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print('work folder kept:', work)
    if fails:
        print('S78M SELFTEST FAILED:')
        for x in fails:
            print('  ' + x)
        return 1
    print('S78M SELFTEST OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
