#!/usr/bin/env python3
"""selftest_r1.py - offline check of the R1 hardpoint probe kit (no game is started, nothing is installed).

  selftest_r1.py [--game DIR] [--source-zip extracted\\foc_data_text.zip] [--keep]

1. Builds the mod into a temp folder: from the install when --game has one (read only), else from the text zip
   (--no-text). The built free store must compile under a stock Lua 5.0.2 (tools\\probe_mod\\out\\tools\\lua50.exe,
   built by ..\\common\\build_lua50.cmd on first use): the syntax check the game's Lua 5.0 would do.
2. Runs R1_CFG + r1_hook.lua under a stubbed engine with a hardpoint model (shield absorbs first, a named hardpoint
   takes the rest, unknown names hit the hull, Get_Hull = min(hull, hardpoint fraction), hull capped at hardpoint
   fraction + 0.2, owner death kills every live hardpoint, breakoff props / death clone are objects) in two lookup
   variants: "type" (case-insensitive HardPoint type name, the expected retail rule) and "coll" (Collision_Mesh only,
   which must switch the later A stages to the alt names). Also a no-human run (main-menu battle) that must SKIP.
3. Passes the records (also shuffled, as the heap scan returns them in address order) through r1_parse.py and checks
   the verdicts of both variants.
4. The model sheet (r1_model_sheet.py) on the Star_Destroyer, when the install is there: the decal meshes are found
   and the engine hardpoint's Engine_Particles subtree is empty.
Ends with R1 SELFTEST OK, or lists the failures (exit 1).
"""
import argparse
import os
import random
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
import build_r1_mod as B  # noqa: E402
import r1_parse  # noqa: E402

STUB = r'''
NOW = 0
MESSAGES = {}
function GetCurrentTime() return NOW end
function Game_Message(s) table.insert(MESSAGES, s) end
function Get_Game_Mode() return GAME_MODE end
OBJS = {}
NEXT_ID = 100
function Find_Object_Type(n)
	if MISSING_TYPES[n] then return nil end
	return { name = n, Get_Name = function() return n end }
end
function PLAYER(name, human)
	local p = { name = name }
	p.Get_Faction_Name = function() return string.upper(name) end
	p.Is_Human = function() return human end
	return p
end
PLAYERS = {}
function Find_Player(n) return PLAYERS[string.upper(n)] end
function Create_Position(x, y, z) return { x = x, y = y, z = z } end
function ADD(tname)
	NEXT_ID = NEXT_ID + 1
	local o = { tname = tname, id = NEXT_ID, valid = true }
	o.Is_Valid = function() return o.valid end
	table.insert(OBJS, o)
	return o
end
function Find_All_Objects_Of_Type(ty)
	local t = {}
	for _, o in ipairs(OBJS) do
		if o.valid and o.tname == ty.name then table.insert(t, o) end
	end
	return t
end
-- hardpoint model of one Star_Destroyer (HP_LIST from the generated config)
function SHIP(pos)
	local o = ADD(R1_CFG.unit)
	o.x, o.y = pos.x, pos.y
	o.hull, o.shield, o.maxhull, o.maxshield = 7500, 2000, 7500, 2000
	o.hps = {}
	local tot = 0
	for i, h in ipairs(R1_CFG.hardpoints) do
		o.hps[i] = { name = h.name, coll = HP_COLL[i], health = h.health, max = h.health, alive = true, breakoff = h.breakoff }
		tot = tot + h.health
	end
	o.tot = tot
	o.frac = function()
		local s = 0
		for _, h in ipairs(o.hps) do if h.alive then s = s + h.health end end
		return s / o.tot
	end
	o.kill_hp = function(h)
		h.alive = false
		h.health = 0
		if h.breakoff ~= "" then ADD(h.breakoff) end
		ADD(R1_CFG.explosion)
		if string.find(h.name, "Shield_Generator") then o.shield = 0 o.noshield = true end
	end
	o.die = function()
		for _, h in ipairs(o.hps) do if h.alive then o.kill_hp(h) end end
		o.valid = false
		ADD(R1_CFG.death_clone)
	end
	o.Get_ID = function() return o.id end
	o.Get_Position = function() return { Get_XYZ = function() return o.x, o.y, 0 end } end
	o.Get_Hull = function() local h = o.hull / o.maxhull local f = o.frac() if f < h then return f end return h end
	o.Get_Shield = function() return o.shield / o.maxshield end
	o.Take_Damage = function(amount, name)
		local a = amount
		local s = o.shield
		if s > a then s = a end
		o.shield = o.shield - s
		a = a - s
		local hit = nil
		if name then
			for _, h in ipairs(o.hps) do
				if (LOOKUP == "type" and string.upper(h.name) == string.upper(name)) or
				   (LOOKUP == "coll" and string.upper(h.coll) == string.upper(name)) then hit = h break end
			end
		end
		if hit then
			if hit.alive then
				hit.health = hit.health - a
				if hit.health <= 0 then o.kill_hp(hit) end
			end
		else
			o.hull = o.hull - a
		end
		local cap = (o.frac() + 0.2) * o.maxhull
		if o.hull > cap then o.hull = cap end
		if o.hull <= 0 then o.die() end
	end
	return o
end
function Spawn_Unit(ty, pos, player)
	if ty.name ~= R1_CFG.unit then return nil end
	return { SHIP(pos) }
end
function Create_Generic_Object(ty, pos, player) return nil end
'''

DRIVER = r'''
-- original free-store functions the hook wraps
function Base_Definitions() ServiceRate = 20 end
function FreeStoreService() FSS_CALLS = (FSS_CALLS or 0) + 1 end
function On_Unit_Service(o) end
dofile(HOOKFILE)
Base_Definitions()
for i = 1, STEPS do
	NOW = i * 0.1
	FreeStoreService()
end
local f = io.open(LOGFILE, "w")
for i = 1, table.getn(R1_LOG) do f:write(R1_LOG[i] .. "\n") end
f:write("MESSAGES " .. table.concat(MESSAGES, ",") .. "\n")
f:write("RATE " .. tostring(ServiceRate) .. "\n")
f:close()
'''

HP_COLL = ['HP_F-L_COLL', 'HP_F-R_COLL', 'HP_B-L_COLL', 'HP_B-R_COLL', 'HP_M-L_COLL', 'HP_M-R_COLL', 'HP_M-C_COLL',
           'HP_E_COLL', 'HP_trac_bone', 'SPAWN_00']

EXPECT = {
    'type': {'FL': 'HP_KILLED', 'ENG': 'HP_KILLED', 'SHLD': 'HP_KILLED', 'TRAC': 'HP_KILLED',
             'DEATH': 'ALL_LIVE_BREAK_OFF', 'LOWER': 'HP_KILLED', 'COLL': 'HULL_HIT'},
    'coll': {'FL': 'HULL_HIT', 'ENG': 'UNSEEN_BY_LUA', 'SHLD': 'HP_KILLED', 'TRAC': 'UNSEEN_BY_LUA',
             'DEATH': 'ALL_LIVE_BREAK_OFF', 'LOWER': 'HULL_HIT', 'COLL': 'HP_KILLED'},
}


def lua_str(s):
    return '"%s"' % s.replace('\\', '\\\\').replace('"', '\\"')


def run_variant(lua, work, cfg, lookup, human=True, mode='Space'):
    hook = os.path.join(work, 'hook_%s.lua' % lookup)
    with open(hook, 'w', newline='\n') as f:
        f.write(B.lua_config(cfg) + '\n' + open(os.path.join(HERE, 'r1_hook.lua'), encoding='ascii').read())
    log = os.path.join(work, 'records_%s%s.log' % (lookup, '' if human else '_nohuman'))
    script = os.path.join(work, 'run_%s.lua' % lookup)
    pre = ['R1_CFG = nil', 'LOOKUP = %s' % lua_str(lookup), 'GAME_MODE = %s' % lua_str(mode),
           'HOOKFILE = %s' % lua_str(hook.replace('\\', '/')), 'LOGFILE = %s' % lua_str(log.replace('\\', '/')),
           'STEPS = %d' % int((cfg['first_kill'] + len(cfg['stages']) * cfg['period'] + cfg['tail'] + 300) / 3),
           'MISSING_TYPES = {}', 'HP_COLL = { %s }' % ', '.join(lua_str(c) for c in HP_COLL)]
    # The stub needs R1_CFG for SHIP(); the hook file defines it, so load the stub functions first and the hook after
    # (dofile in DRIVER). PlayerObject / players are set before the hook runs.
    pre.append('PLAYERS = {}')
    body = '\n'.join(pre) + '\n' + STUB + '\n' + \
        'PLAYERS["EMPIRE"] = PLAYER("Empire", %s)\nPLAYERS["REBEL"] = PLAYER("Rebel", false)\n' % (
            'true' if human else 'false') + 'PlayerObject = PLAYERS["REBEL"]\n' + DRIVER
    with open(script, 'w', newline='\n') as f:
        f.write(body)
    r = subprocess.run([lua, script], capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        raise RuntimeError('lua run %s failed: %s %s' % (lookup, r.stdout[-800:], r.stderr[-800:]))
    return log


def check_build(fails, game, source_zip, work, lua):
    use_zip = not os.path.isdir(os.path.join(game, 'corruption'))
    mod = os.path.join(work, 'mod')
    cfg = B.build(game, mod, source_zip if use_zip else None, no_text=use_zip)
    fs = os.path.join(mod, 'Data', 'Scripts', 'FreeStore', 'BusyTacticalFreeStore.lua')
    if not os.path.isfile(fs):
        fails.append('free store not built')
        return cfg
    if not use_zip and not os.path.isfile(os.path.join(mod, 'Data', 'Text', 'MasterTextFile_English.dat')):
        fails.append('text file not built')
    if not os.path.isfile(os.path.join(mod, B.MARKER)):
        fails.append('marker missing')
    chk = os.path.join(work, 'compile.lua')
    with open(chk, 'w') as f:
        f.write('local f, e = loadfile(%s)\nif not f then print("COMPILE_FAIL " .. e) os.exit(1) end\nprint("COMPILE_OK")\n'
                % lua_str(fs.replace('\\', '/')))
    r = subprocess.run([lua, chk], capture_output=True, text=True)
    if 'COMPILE_OK' not in r.stdout:
        fails.append('built free store does not compile: ' + r.stdout + r.stderr)
    data = open(fs, 'rb').read()
    if b'R1_CFG = {' not in data or b'r1_hook.lua' not in data or b'\r\n' not in data:
        fails.append('free store tail / CRLF missing')
    keys = {t for t, _ in B.TEXTS}
    for s in B.STAGES:
        for suf in ('_PRE', '_DONE'):
            if 'TEXT_R1P_' + s['key'] + suf not in keys:
                fails.append('no text for stage %s%s' % (s['key'], suf))
    print('build: %s, site %s' % ('zip' if use_zip else 'install', cfg['site_note']))
    return cfg


def check_variant(fails, lua, work, cfg, lookup):
    log = run_variant(lua, work, cfg, lookup)
    lines = open(log).read().splitlines()
    msgs = [x for x in lines if x.startswith('MESSAGES ')]
    rate = [x for x in lines if x.startswith('RATE ')]
    if not rate or rate[0] != 'RATE 20':
        fails.append('%s: ServiceRate not restored (%s)' % (lookup, rate))
    if not msgs or 'TEXT_R1P_DONE' not in msgs[0]:
        fails.append('%s: no DONE message' % lookup)
    for variant, path in (('ordered', log), ('shuffled', None)):
        if path is None:
            recs = [x for x in lines if x.startswith('R1P|')]
            random.Random(7).shuffle(recs)
            path = os.path.join(work, 'shuffled_%s.log' % lookup)
            with open(path, 'w') as f:
                f.write('\n'.join(recs) + '\n')
        text, _recs = r1_parse.report(path, os.path.join(work, 'report_%s_%s.txt' % (lookup, variant)))
        summary, _ = r1_parse.analyse(r1_parse.load(path))
        got = {s['key']: s['verdict'] for s in summary['stages']}
        for k, v in EXPECT[lookup].items():
            if got.get(k) != v:
                fails.append('%s/%s: stage %s verdict %s, expected %s' % (lookup, variant, k, got.get(k), v))
        mode = summary.get('mode') or {}
        if mode.get('mode') != lookup:
            fails.append('%s/%s: name mode %s' % (lookup, variant, mode.get('mode')))
        death = next((s for s in summary['stages'] if s['key'] == 'DEATH'), {})
        if death.get('death_clone') != 1:
            fails.append('%s/%s: death clone %s' % (lookup, variant, death.get('death_clone')))
        if summary['apierr'] or summary['errors'] or not summary['done']:
            fails.append('%s/%s: apierr %s errors %s done %s' % (lookup, variant, summary['apierr'], summary['errors'],
                                                                  bool(summary['done'])))
    print('variant %s: %s' % (lookup, ', '.join('%s=%s' % kv for kv in sorted(got.items()))))


def check_nohuman(fails, lua, work, cfg):
    log = run_variant(lua, work, cfg, 'type', human=False)
    recs = [x for x in open(log).read().splitlines() if x.startswith('R1P|')]
    if any('|ARM|' in r or '|SPAWN|' in r for r in recs) or not any('|SKIP|' in r and 'no_human' in r for r in recs):
        fails.append('no-human run did not SKIP: %s' % recs[:4])


def check_sheet(fails, game):
    if not os.path.isdir(os.path.join(game, 'corruption')):
        print('model sheet: skipped (no install)')
        return
    import r1_model_sheet as S
    s = S.sheet('Star_Destroyer', S.XmlSource(game), S.ModelFiles(game))
    hp = {h['name']: h for h in s['hardpoints']}
    if not s.get('model_source'):
        fails.append('model sheet: EV_StarDestroyer.ALO not read')
        return
    for n in ('HP_Star_Destroyer_Weapon_FL', 'HP_Star_Destroyer_Shield_Generator', 'HP_Star_Destroyer_Tractor_Beam'):
        if not (hp.get(n, {}).get('decal') or {}).get('found'):
            fails.append('model sheet: decal of %s not found' % n)
    eng = hp.get('HP_Star_Destroyer_Engines', {})
    if eng.get('hidden_on_death') != {'meshes': [], 'proxies': []}:
        fails.append('model sheet: engine subtree not empty: %s' % eng.get('hidden_on_death'))
    if [h['name'] for h in s['hardpoints']] != [n for n, _h, _b in B.HARDPOINTS]:
        fails.append('model sheet: HardPoints list differs from build_r1_mod.HARDPOINTS')
    print('model sheet: %s, %d hardpoints, engine subtree empty' % (s['model_source'], len(s['hardpoints'])))


def parse_selftest():
    return main([])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=B.DEFAULT_GAME)
    ap.add_argument('--source-zip', default=os.path.join(ROOT, 'extracted', 'foc_data_text.zip'))
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args(argv)
    lua = lua50.find_lua()
    if not lua:
        print('lua50.exe missing and could not be built (see %s)' % lua50.BUILD_LOG)
        print('R1 SELFTEST FAILED')
        return 1
    work = tempfile.mkdtemp(prefix='r1_selftest_')
    fails = []
    try:
        cfg = check_build(fails, a.game, a.source_zip, work, lua)
        for lookup in ('type', 'coll'):
            check_variant(fails, lua, work, cfg, lookup)
        check_nohuman(fails, lua, work, cfg)
        check_sheet(fails, a.game)
    except Exception as e:  # noqa: BLE001
        fails.append('exception: %r' % e)
    finally:
        if a.keep:
            print('kept', work)
        else:
            shutil.rmtree(work, ignore_errors=True)
    for f in fails:
        print('FAIL', f)
    print('R1 SELFTEST OK' if not fails else 'R1 SELFTEST FAILED (%d)' % len(fails))
    return 0 if not fails else 1


if __name__ == '__main__':
    sys.exit(main())
