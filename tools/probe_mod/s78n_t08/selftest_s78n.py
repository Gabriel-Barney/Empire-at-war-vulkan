#!/usr/bin/env python3
"""selftest_s78n.py - offline check of the s78n t08 probe kit (no game is started, nothing is installed).

  selftest_s78n.py [--game DIR] [--source-zip extracted\\foc_data_text.zip] [--lua <lua50.exe>] [--keep]

1. Builds the mod into a temp folder: from the install when --game has one (read only), else from the text zip
   (--no-text, fallback sites and extents).
2. The built BusyTacticalFreeStore.lua is compiled by a stock Lua 5.0.2 (tools\\probe_mod\\out\\tools\\lua50.exe,
   built by ..\\common\\build_lua50.cmd on first use): the syntax check the game's Lua 5.0 would do. The probe must
   not use the math library or '#'.
3. The probe (SN_CFG + s78n_probe.lua, without the shipped free store) runs under a stubbed engine:
   space_port  the port model: the beam starts when d <= R + E (E by the target's facing), the user does not walk,
               the order is kept; verdict (A) must be PASS, every trial PASS;
   space_alt   centre distance only (d <= R) and the user walks to the target; verdict (A) must be FAIL;
   land_port   the port model: Empire and Underworld allied, the aura on every allied unit (own included) with no
               radius, applied at the source's arrival with the current health raised, Lua damage passes
               invulnerability; verdict (B) must be PASS;
   land_alt    not allied at the start (the probe's Make_Ally fallback), invulnerability blocks the Lua damage
               (INVULN_BLOCKS fallback), the aura only on the owner's units within 300 and only the maximum raised
               for units already there; verdict (B) must be FAIL with B0 PASS;
   menu        Empire not human (the main-menu background battle): only SKIP, never ARM.
   Each run's records go through s78n_analyse.py.
Ends with S78N SELFTEST OK, or lists the failures (exit 1).
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
import build_s78n_mod as B  # noqa: E402
import s78n_analyse  # noqa: E402

STUB = r'''
OUT = io.open(LOGFILE, "w")
NOW = 0
FRAME = 0
function GetCurrentTime() return NOW end
function Game_Message(s) MSGS = (MSGS or "") .. s .. ";" end
function Point_Camera_At(p) if type(p) ~= "table" or p.x == nil then error("camera needs a position") end end
OBJS = {}
NEXT_ID = 1000
function PLAYER(name, human)
	local p = { name = name, allies = {} }
	p.allies[string.upper(name)] = true
	p.Get_Faction_Name = function() return name end
	p.Is_Human = function() return human end
	p.Is_Ally = function(q) return p.allies[string.upper(q.name)] == true end
	p.Make_Ally = function(q) p.allies[string.upper(q.name)] = true end
	return p
end
function Find_Player(n) return PLAYERS[string.upper(n)] end
function Create_Position(x, y, z) return { x = x, y = y, z = z } end
function DIST(a, b) local dx, dy = a.x - b.x, a.y - b.y return math.sqrt(dx * dx + dy * dy) end
function TYPE(n)
	local t = { name = n }
	t.Get_Name = function() return string.upper(n) end
	return t
end
function Find_Object_Type(n) return TYPE(n) end
function NEWOBJ(tname, x, y, player)
	NEXT_ID = NEXT_ID + 1
	local o = { tname = tname, x = x, y = y, owner = player, id = NEXT_ID, valid = true, fx = 1, fy = 0,
		active = false, eff = false, autofire = true, pending = nil, invuln = false, hmax = HEALTH, hcur = HEALTH,
		boosted = false }
	o.Is_Valid = function() return o.valid end
	o.Get_Object_ID = function() return o.id end
	o.Get_Type = function() return TYPE(o.tname) end
	o.Get_Owner = function() return o.owner end
	o.Get_Position = function() return { Get_XYZ = function() return o.x, o.y, 0 end } end
	o.Despawn = function() o.valid = false end
	o.Is_Ability_Ready = function(ab) return not o.active end
	o.Is_Ability_Active = function(ab) return o.active end
	o.Is_Under_Effects_Of_Ability = function(ab) return o.eff end
	o.Is_Ability_Autofire = function(ab) return o.autofire end
	o.Set_Single_Ability_Autofire = function(ab, v) o.autofire = v end
	o.Activate_Ability = function(ab, arg)
		if type(arg) ~= "table" or arg.Is_Valid == nil then error("Activate_Ability without a game object stops a retail script") end
		if ab ~= "TRACTOR_BEAM" or o.active then return nil end
		o.pending = arg
		return {}
	end
	o.Turn_To_Face = function(t)
		local d = DIST(o, t)
		o.fx, o.fy = (t.x - o.x) / d, (t.y - o.y) / d
	end
	o.Teleport = function(p) o.x, o.y = p.x, p.y end
	o.Take_Damage = function(v)
		if o.invuln and INVULN_BLOCKS then return end
		o.hcur = o.hcur - v
	end
	o.Get_Hull = function() return o.hcur / o.hmax end
	for _, m in ipairs({ "Prevent_AI_Usage", "Prevent_All_Fire" }) do
		o[m] = function(v) if type(v) ~= "boolean" then error(m .. " needs a bool") end end
	end
	o.Make_Invulnerable = function(v) if type(v) ~= "boolean" then error("needs a bool") end o.invuln = v end
	table.insert(OBJS, o)
	return o
end
function Spawn_Unit(ty, pos, player)
	if WATER and pos.x < 0 then return {} end
	return { NEWOBJ(ty.name, pos.x, pos.y, player) }
end
function Find_All_Objects_Of_Type(n)
	local l = {}
	for _, o in ipairs(OBJS) do
		if o.valid and type(n) == "string" and string.upper(o.tname) == string.upper(n) then table.insert(l, o) end
	end
	return l
end
function Get_Game_Mode() return MODE end
-- tractor beam: every 3rd frame (the targeting service) a pending order starts when in range, else (WALK) the user
-- moves 2 units a frame toward the target; the beam ends when the target is gone
function BEAM(f)
	for _, u in ipairs(OBJS) do
		if u.valid and u.tname == "Star_Destroyer" then
			local x = u.pending
			if u.active and (x == nil or not x.valid) then u.active = false u.pending = nil end
			if x and x.valid and not u.active then
				local d = DIST(u, x)
				local ux, uy = (u.x - x.x) / d, (u.y - x.y) / d
				local dot = x.fx * ux + x.fy * uy
				if dot < 0 then dot = -dot end
				local e = EY
				if dot > 0.7071 then e = EX end
				if CENTRE_ONLY then e = 0 end
				if math.mod(f, 3) == 0 and d <= R + e then
					u.active = true
					x.eff = true
				elseif WALK and d > R + e then
					u.x = u.x + (x.x - u.x) / d * 2
					u.y = u.y + (x.y - u.y) / d * 2
				end
			end
		end
	end
end
-- Combat_Bonus aura of a valid Emperor: Health_Bonus on the AT-STs it reaches; gone when he is
function REACHES(h, o)
	if not ALLY_AURA and o.owner ~= h.owner then return false end
	if not h.owner.Is_Ally(o.owner) then return false end
	if RADIUS and DIST(h, o) > RADIUS then return false end
	return true
end
function AURA(f)
	local h = nil
	for _, o in ipairs(OBJS) do
		if o.valid and o.tname == "Emperor_Palpatine" then h = o end
	end
	for _, o in ipairs(OBJS) do
		if o.valid and o.tname == "AT_ST_Walker" then
			local want = h ~= nil and REACHES(h, o)
			if want and not o.boosted then
				o.boosted = true
				local old = o.hmax
				o.hmax = HEALTH * (1 + BONUS)
				if not MAX_ONLY then o.hcur = o.hcur + o.hmax - old end
			elseif not want and o.boosted then
				o.boosted = false
				o.hmax = HEALTH
				if o.hcur > o.hmax then o.hcur = o.hmax end
			end
		end
	end
end
-- shipped free store stand-ins
function Base_Definitions() ServiceRate = 20 end
function FreeStoreService() end
function On_Unit_Service(o) end
'''

SPACE_PORT = r'''
MODE = "Space"
PLAYERS = { EMPIRE = PLAYER("Empire", true), REBEL = PLAYER("Rebel", false) }
PlayerObject = PLAYERS.REBEL
function WORLD(f) BEAM(f) end
'''
SPACE_ALT = 'CENTRE_ONLY = true\nWALK = true\n' + SPACE_PORT

LAND_PORT = r'''
MODE = "Land"
PLAYERS = { EMPIRE = PLAYER("Empire", true), REBEL = PLAYER("Rebel", false), UNDERWORLD = PLAYER("Underworld", false) }
PLAYERS.EMPIRE.Make_Ally(PLAYERS.UNDERWORLD)
PLAYERS.UNDERWORLD.Make_Ally(PLAYERS.EMPIRE)
PlayerObject = PLAYERS.REBEL
ALLY_AURA = true
function WORLD(f) AURA(f) end
'''
LAND_ALT = r'''
MODE = "Land"
PLAYERS = { EMPIRE = PLAYER("Empire", true), REBEL = PLAYER("Rebel", false), UNDERWORLD = PLAYER("Underworld", false) }
PlayerObject = PLAYERS.REBEL
ALLY_AURA = false
RADIUS = 300
MAX_ONLY = true
INVULN_BLOCKS = true
function WORLD(f) AURA(f) end
'''
MENU = r'''
MODE = "Land"
PLAYERS = { EMPIRE = PLAYER("Empire", false), REBEL = PLAYER("Rebel", false), UNDERWORLD = PLAYER("Underworld", false) }
PlayerObject = PLAYERS.REBEL
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
for i = 1, table.getn(SN_LOG) do OUT:write(SN_LOG[i] .. "\n") end
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
    work = tempfile.mkdtemp(prefix='s78n_selftest_')
    have_game = os.path.isfile(os.path.join(a.game, 'corruption', 'StarWarsG.exe'))
    print('source:', 'install ' + a.game + ' (read only)' if have_game else 'text zip ' + a.source_zip)
    out = os.path.join(work, 'mod', B.MOD_NAME)
    cfg = None
    try:
        cfg = B.build(a.game, out, None if have_game else a.source_zip, no_text=not have_game)
        print('built %s; %s' % (', '.join(cfg['files']), cfg.get('site_note')))
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
        probe = open(os.path.join(HERE, 's78n_probe.lua'), encoding='ascii').read()
        code = '\n'.join(line.split('--')[0] for line in probe.splitlines())
        for bad in ('math.', '_MessagePopup', '#'):
            if bad in code:
                fails.append('s78n_probe.lua uses %r (not available / not shown in retail free-store Lua)' % bad)
        tb, cb = cfg['tb'], cfg['cb']
        consts = ('R = %r\nEX = %r\nEY = %r\nHEALTH = %r\nBONUS = %r\n' % (
            float(tb['R']), float(tb['ex']), float(tb['ey']), float(cb['health']), float(cb['bonus'])))
        cfg_path = os.path.join(work, 'config.json')
        import json
        with open(cfg_path, 'w') as f:
            json.dump({k: v for k, v in cfg.items() if k != 'files'}, f, default=str)
        results = {}
        worlds = (('space_port', SPACE_PORT, 9000), ('space_alt', SPACE_ALT, 9000), ('land_port', LAND_PORT, 1800),
                  ('land_alt', LAND_ALT, 1800), ('menu', MENU, 900))
        for name, world, frames in worlds:
            log = os.path.join(work, name + '.log')
            chunks = ['LOGFILE = "%s"' % lua_str(log), 'FRAMES = %d' % frames, consts, STUB, world, B.lua_config(cfg),
                      probe, DRIVE]
            rc, msg = run_lua(lua, work, 'run_' + name, chunks)
            if rc != 0:
                fails.append('stub run %s failed: %s' % (name, msg[:600]))
                continue
            text, res = s78n_analyse.report(log, os.path.join(work, name + '_report.txt'),
                                            os.path.join(work, name + '_report.json'), cfg_path)
            results[name] = (text, res)
            print('=' * 30, name, '=' * 30)
            print(text)

        def verdict(res, key):
            for part in ('tb', 'cb'):
                r = res.get(part)
                if r and key in r.get('verdicts', {}):
                    return r['verdicts'][key]
            return None

        for name in ('space_port', 'land_port'):
            if name in results:
                text, res = results[name]
                log = os.path.join(work, name + '.log')
                nerr = sum(1 for line in open(log) if line.startswith(('S8N|ERR|', 'S8N|APIERR|')))
                if nerr:
                    fails.append('%s run recorded %d APIERR / ERR records' % (name, nerr))
                if not any(s['done'] for s in res['scripts']):
                    fails.append('%s run did not reach DONE' % name)
        if 'space_port' in results:
            res = results['space_port'][1]
            if verdict(res, 'A') != 'PASS':
                fails.append('space_port: verdict (A) %s, expected PASS' % verdict(res, 'A'))
            bad = [t['name'] for t in (res.get('tb') or {}).get('trials', []) if not t['port_agrees']]
            if bad or len((res.get('tb') or {}).get('trials', [])) != len(tb['trials']):
                fails.append('space_port: trials not all PASS / missing: %s' % bad)
        if 'space_alt' in results:
            res = results['space_alt'][1]
            if verdict(res, 'A') != 'FAIL' or verdict(res, 'A1') != 'FAIL':
                fails.append('space_alt: verdict (A) %s / A1 %s, expected FAIL / FAIL' % (verdict(res, 'A'),
                                                                                         verdict(res, 'A1')))
        if 'land_port' in results:
            res = results['land_port'][1]
            for k in ('B0', 'B1', 'B2', 'B3', 'B'):
                if verdict(res, k) != 'PASS':
                    fails.append('land_port: %s %s, expected PASS' % (k, verdict(res, k)))
        if 'land_alt' in results:
            text, res = results['land_alt']
            want = {'B0': 'PASS', 'B1': 'FAIL', 'B2': 'FAIL', 'B3': 'FAIL', 'B': 'FAIL'}
            for k, w in want.items():
                if verdict(res, k) != w:
                    fails.append('land_alt: %s %s, expected %s' % (k, verdict(res, k), w))
            if 'Make_Ally called' not in text or 'invulnerability blocked' not in text:
                fails.append('land_alt: the Make_Ally / INVULN_BLOCKS fallbacks were not reported')
        if 'menu' in results:
            res = results['menu'][1]
            if any(s['armed'] for s in res['scripts']) or not any(s['skip'] for s in res['scripts']):
                fails.append('menu run: expected SKIP and no ARM')
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print('work folder kept:', work)
    if fails:
        print('S78N SELFTEST FAILED:')
        for x in fails:
            print('  ' + x)
        return 1
    print('S78N SELFTEST OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
