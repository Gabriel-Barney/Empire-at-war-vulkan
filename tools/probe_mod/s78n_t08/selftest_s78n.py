#!/usr/bin/env python3
"""selftest_s78n.py - offline check of the s78n t08 probe kit (no game is started, nothing is installed).

  selftest_s78n.py [--game DIR] [--source-zip extracted\\foc_data_text.zip] [--lua <lua50.exe>] [--keep]

1. Builds the mod twice into a temp folder (space + land, and --space-only): from the install when --game has one
   (read only), else from the text zip (--no-text, fallback sites, extents and facing bones).
2. The built BusyTacticalFreeStore.lua files are compiled by a stock Lua 5.0.2 (tools\\probe_mod\\out\\tools\\
   lua50.exe, built by ..\\common\\build_lua50.cmd on first use): the syntax check the game's Lua 5.0 would do. The
   probe must not use the math library or '#'.
3. The probe (SN_CFG + s78n_probe.lua, without the shipped free store) runs under a stubbed engine. Space worlds
   (space-only config): Spawn_Unit puts the ships 300 / 200 off the asked point (as retail did; Teleport fixes it),
   Face_Immediate takes effect 40 frames late (the settle wait must catch it), Get_Bone_Position gives the two
   facing bones (an unknown bone name is an error), Suspend_Locomotor(true) stops a user walking:
   space_new     sec. 5.4.5 (E along 109.95 / across 23.48), user locked: verdict (A) PASS, every trial MATCH, A1 PASS;
   space_old     the old port rule (along 23.48 / across 111.86): verdict (A) FAIL;
   space_lock    sec. 5.4.5, but a locked user never fires and an unlocked one walks in: ULOCK reported on the first
                 control, verdict (A) PASS from the walk brackets;
   space_nobone  sec. 5.4.5 with the facing read off (--no-bone-read): A1 ASSUMED, verdict (A) PASS;
   land_space_only  a land battle with the space-only mod: SKIP only, never ARM.
   Land worlds (space + land config):
   land_port   the port model: Empire and Underworld allied, the aura on every allied unit (own included) with no
               radius, applied at the source's arrival with the current health raised, Lua damage passes
               invulnerability; verdict (B) must be PASS;
   land_alt    not allied at the start (the probe's Make_Ally fallback), invulnerability blocks the Lua damage
               (INVULN_BLOCKS fallback), the aura only on the owner's units within 300 and only the maximum raised
               for units already there; verdict (B) must be FAIL with B0 PASS;
   menu        Empire not human (the main-menu background battle): only SKIP, never ARM.
   Each run's records go through s78n_analyse.py.
4. When out\\20261109-200249\\records.log is there (the first retail run), its report is rebuilt (old "tb" analysis).
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
	-- Face_Immediate(position): the new facing shows TURN_FRAMES later (0: at once)
	o.Face_Immediate = function(p)
		if type(p) ~= "table" or p.x == nil then error("Face_Immediate -- could not extract a position") end
		local d = DIST(o, p)
		o.tfx, o.tfy = (p.x - o.x) / d, (p.y - o.y) / d
		o.turn_at = FRAME + (TURN_FRAMES or 0)
	end
	o.Suspend_Locomotor = function(v)
		if type(v) ~= "boolean" then error("Suspend_Locomotor -- Expected bool") end
		o.locked = v
	end
	-- bones in model space (nose = model -Y, left = +X), scale 0.7: world = pos - by * f + bx * (-fy, fx)
	o.Get_Bone_Position = function(name)
		local b = BONES[name]
		if b == nil then error("unknown bone: a failed engine read stops a retail script") end
		local wx = o.x - b[2] * o.fx - b[1] * o.fy
		local wy = o.y - b[2] * o.fy + b[1] * o.fx
		return { Get_XYZ = function() return wx, wy, 0 end }
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
BONES = { missile_target00 = { 0, -79.76 * 0.7 }, missile_target01 = { 0, 128.41 * 0.7 } }
function Spawn_Unit(ty, pos, player)
	if WATER and pos.x < 0 then return {} end
	local ox, oy = 0, 0
	if SPAWN_OFF then ox, oy = 300, -200 end
	return { NEWOBJ(ty.name, pos.x + ox, pos.y + oy, player) }
end
function TURNS(f)
	for _, o in ipairs(OBJS) do
		if o.turn_at and f >= o.turn_at then
			o.fx, o.fy = o.tfx, o.tfy
			o.turn_at = nil
		end
	end
end
function Find_All_Objects_Of_Type(n)
	local l = {}
	for _, o in ipairs(OBJS) do
		if o.valid and type(n) == "string" and string.upper(o.tname) == string.upper(n) then table.insert(l, o) end
	end
	return l
end
function Get_Game_Mode() return MODE end
-- tractor beam: every 3rd frame (the targeting service) a pending order starts when in range (E_ALONG when the
-- target's facing is along the line, |f.u| > 0.7071, else E_ACROSS), else (WALK, user not locked) the user moves 2
-- units a frame toward the target; LOCK_BLOCKS: a locked user never starts it; the beam ends when the target is gone
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
				local e = E_ACROSS
				if dot > 0.7071 then e = E_ALONG end
				if CENTRE_ONLY then e = 0 end
				local blocked = LOCK_BLOCKS and u.locked
				if math.mod(f, 3) == 0 and d <= R + e and not blocked then
					u.active = true
					x.eff = true
				elseif WALK and not u.locked and d > R + e then
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

SPACE = r'''
MODE = "Space"
SPAWN_OFF = true
TURN_FRAMES = 40
PLAYERS = { EMPIRE = PLAYER("Empire", true), REBEL = PLAYER("Rebel", false) }
PlayerObject = PLAYERS.REBEL
function WORLD(f) TURNS(f) BEAM(f) end
'''
SPACE_NEW = 'E_ALONG = EA\nE_ACROSS = EC\n' + SPACE
SPACE_OLD = 'E_ALONG = OA\nE_ACROSS = OC\n' + SPACE
SPACE_LOCK = 'E_ALONG = EA\nE_ACROSS = EC\nLOCK_BLOCKS = true\nWALK = true\n' + SPACE
LAND_SPACE_ONLY = r'''
MODE = "Land"
PLAYERS = { EMPIRE = PLAYER("Empire", true), REBEL = PLAYER("Rebel", false), UNDERWORLD = PLAYER("Underworld", false) }
PlayerObject = PLAYERS.REBEL
function WORLD(f) end
'''

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
    cfgs = {}
    import copy
    import json
    for mode, space_only in (('both', False), ('space', True)):
        out = os.path.join(work, 'mod_' + mode, B.MOD_NAME)
        try:
            cfg = B.build(a.game, out, None if have_game else a.source_zip, no_text=not have_game,
                          space_only=space_only)
            cfgs[mode] = cfg
            print('built %s (%s); %s' % (', '.join(cfg['files']), mode, cfg.get('site_note')))
            print('\n'.join(B.describe(cfg)))
        except SystemExit as e:
            fails.append('build %s: %s' % (mode, e))
            continue
        for rel in cfg['files']:
            if rel.lower().endswith('.lua'):
                rc, msg = run_lua(lua, work, 'compile', ['assert(loadfile("%s"))' % lua_str(os.path.join(out, rel))])
                if rc != 0:
                    fails.append('%s (%s): Lua 5.0 compile failed: %s' % (rel, mode, msg))
                else:
                    print('compiled %s (%s) with stock Lua 5.0.2' % (rel, mode))
        if have_game and not any(r.lower().endswith('.dat') for r in cfg['files']):
            fails.append('the install build (%s) has no MasterTextFile_English.dat' % mode)
    if 'space' in cfgs:
        tv = cfgs['space']['tv']
        if not cfgs['space'].get('space_only'):
            fails.append('the --space-only build has no space_only flag')
        if not tv['bone_front'] or not tv['bone_rear']:
            fails.append('no facing bones in the space-only build (%s)' % cfgs['space']['extent'].get('note'))
        want = {'e_along': 109.95, 'e_across': 23.48}
        for key, w in want.items():
            if abs(tv[key] - w) > 0.01:
                fails.append('%s %.3f, expected %.2f (sec. 5.4.5, Nebulon-B)' % (key, tv[key], w))
        ds = [t['d'] for t in tv['trials']]
        if ds != [950.0, 1095.0, 1105.0, 1115.0, 1125.0, 950.0, 1010.0, 1018.0, 1029.0, 1037.0]:
            fails.append('unexpected trial distances %s' % ds)
    if len(cfgs) == 2:
        probe = open(os.path.join(HERE, 's78n_probe.lua'), encoding='ascii').read()
        code = '\n'.join(line.split('--')[0] for line in probe.splitlines())
        for bad in ('math.', '_MessagePopup', '#'):
            if bad in code:
                fails.append('s78n_probe.lua uses %r (not available / not shown in retail free-store Lua)' % bad)
        tv, cb = cfgs['space']['tv'], cfgs['both']['cb']
        consts = ('R = %r\nEA = %r\nEC = %r\nOA = %r\nOC = %r\nHEALTH = %r\nBONUS = %r\n' % (
            float(tv['R']), float(tv['e_along']), float(tv['e_across']), float(tv['old_along']),
            float(tv['old_across']), float(cb['health']), float(cb['bonus'])))
        nobone = copy.deepcopy(cfgs['space'])
        nobone['tv']['bone_front'] = nobone['tv']['bone_rear'] = ''
        cfgs['nobone'] = nobone
        cfg_paths = {}
        for mode, cfg in cfgs.items():
            cfg_paths[mode] = os.path.join(work, 'config_%s.json' % mode)
            with open(cfg_paths[mode], 'w') as f:
                json.dump({k: v for k, v in cfg.items() if k != 'files'}, f, default=str)
        results = {}
        worlds = (('space_new', SPACE_NEW, 9000, 'space'), ('space_old', SPACE_OLD, 9000, 'space'),
                  ('space_lock', SPACE_LOCK, 9000, 'space'), ('space_nobone', SPACE_NEW, 12000, 'nobone'),
                  ('land_space_only', LAND_SPACE_ONLY, 900, 'space'), ('land_port', LAND_PORT, 1800, 'both'),
                  ('land_alt', LAND_ALT, 1800, 'both'), ('menu', MENU, 900, 'both'))
        for name, world, frames, mode in worlds:
            log = os.path.join(work, name + '.log')
            chunks = ['LOGFILE = "%s"' % lua_str(log), 'FRAMES = %d' % frames, consts, STUB, world,
                      B.lua_config(cfgs[mode]), probe, DRIVE]
            rc, msg = run_lua(lua, work, 'run_' + name, chunks)
            if rc != 0:
                fails.append('stub run %s failed: %s' % (name, msg[:600]))
                continue
            text, res = s78n_analyse.report(log, os.path.join(work, name + '_report.txt'),
                                            os.path.join(work, name + '_report.json'), cfg_paths[mode])
            results[name] = (text, res)
            print('=' * 30, name, '=' * 30)
            print(text)

        def verdict(res, key):
            for part in ('tv', 'tb', 'cb'):
                r = res.get(part)
                if r and key in r.get('verdicts', {}):
                    return r['verdicts'][key]
            return None

        for name in ('space_new', 'space_lock', 'space_nobone', 'land_port'):
            if name in results:
                text, res = results[name]
                log = os.path.join(work, name + '.log')
                nerr = sum(1 for line in open(log) if line.startswith(('S8N|ERR|', 'S8N|APIERR|')))
                if nerr:
                    fails.append('%s run recorded %d APIERR / ERR records' % (name, nerr))
                if not any(s['done'] for s in res['scripts']):
                    fails.append('%s run did not reach DONE' % name)
        for name, want in (('space_new', {'A': 'PASS', 'A0': 'PASS', 'A1': 'PASS', 'A2': 'PASS', 'A3': 'PASS'}),
                           ('space_old', {'A': 'FAIL', 'A0': 'PASS', 'A2': 'FAIL', 'A3': 'FAIL'}),
                           ('space_lock', {'A': 'PASS', 'A0': 'PASS', 'A2': 'PASS'}),
                           ('space_nobone', {'A': 'PASS', 'A1': 'ASSUMED'})):
            if name not in results:
                continue
            text, res = results[name]
            for k, w in want.items():
                if verdict(res, k) != w:
                    fails.append('%s: %s %s, expected %s' % (name, k, verdict(res, k), w))
            trials = (res.get('tv') or {}).get('trials', [])
            if len(trials) != len(tv['trials']):
                fails.append('%s: %d trials analysed, expected %d' % (name, len(trials), len(tv['trials'])))
            if 'VERDICT (A) tractor beam range, TACTICAL_COMBAT 5.4.5 (R + E: along 1109.95 / across 1023.48)' \
                    not in text or 'old port 1023.48 / 1111.86' not in text or 'NO DATA (space-only run)' not in text:
                fails.append('%s: the (A) / (B) verdict lines are not as expected' % name)
        if 'space_new' in results:
            trials = (results['space_new'][1].get('tv') or {}).get('trials', [])
            bad = [t['name'] for t in trials if not t.get('match')]
            if bad:
                fails.append('space_new: trials not MATCH: %s' % bad)
            stepped = [t['name'] for t in trials if t.get('obs') == 'OUT' and t.get('d_eng') is None]
            if stepped:
                fails.append('space_new: OUT trials without a stepped-in bracket: %s' % stepped)
            if any(t.get('facing_src') != 'bones' for t in trials):
                fails.append('space_new: facing not read from the bones in every trial')
        if 'space_lock' in results:
            text = results['space_lock'][0]
            if 'ULOCK: the locked user did not start the beam on ctrl_along' not in text:
                fails.append('space_lock: the ULOCK fallback was not reported')
        if 'land_space_only' in results:
            res = results['land_space_only'][1]
            if any(s['armed'] for s in res['scripts']) or not any(s['skip'] for s in res['scripts']):
                fails.append('land_space_only run: expected SKIP and no ARM')
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
    old_run = os.path.join(HERE, 'out', '20261109-200249')
    if os.path.isfile(os.path.join(old_run, 'records.log')):
        text, res = s78n_analyse.report(os.path.join(old_run, 'records.log'), None, None,
                                        os.path.join(old_run, 'config.json'))
        if 'VERDICT (A) tractor beam range includes the target footprint: INCONCLUSIVE' not in text or \
                'VERDICT (B)' not in text:
            fails.append('the report of the first retail run (20261109-200249) changed')
        else:
            print('first retail run 20261109-200249: report rebuilt (old "tb" analysis)')
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
