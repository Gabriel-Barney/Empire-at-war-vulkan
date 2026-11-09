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
import json
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
	o.Move_To = function(pos) o.x, o.y = pos.x, pos.y MOVED = (MOVED or 0) + 1 end
	o.Get_ID = function() return o.id end
	o.Get_Position = function() return { Get_XYZ = function() return o.x, o.y, 0 end } end
	o.Get_Hull = function() local h = o.hull / o.maxhull local f = o.frac() if f < h then return f end return h end
	o.Get_Shield = function() return o.shield / o.maxshield end
	o.Take_Damage = function(amount, name)
		if BOGUS_STOP and name == "HP_NO_SUCH" then
			WRITE_LOG()   -- retail stops the Lua script inside this call: nothing after it runs
			os.exit(0)
		end
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
function WRITE_LOG()
	local f = io.open(LOGFILE, "w")
	for i = 1, table.getn(R1_LOG) do f:write(R1_LOG[i] .. "\n") end
	f:write("MESSAGES " .. table.concat(MESSAGES, ",") .. "\n")
	f:write("RATE " .. tostring(ServiceRate) .. "\n")
	f:close()
end
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
WRITE_LOG()
'''

HP_COLL = ['HP_F-L_COLL', 'HP_F-R_COLL', 'HP_B-L_COLL', 'HP_B-R_COLL', 'HP_M-L_COLL', 'HP_M-R_COLL', 'HP_M-C_COLL',
           'HP_E_COLL', 'HP_trac_bone', 'SPAWN_00']

EXPECT = {
    # (profile, lookup variant) -> stage verdicts. '@' = whatever the stub gives, only checked to not be NOT_RUN.
    ('short', 'type'): {'FL': 'HP_KILLED', 'ML': 'HP_KILLED', 'MR': 'HP_KILLED', 'ENG': 'HP_KILLED',
                        'BOGUS': 'NO_MATCH_UNSEEN_BY_LUA'},   # stub: shield already 0, hull masked by the fraction,
    ('short', 'coll'): {'FL': 'HULL_HIT', 'ML': '@', 'MR': '@', 'ENG': '@', 'BOGUS': 'NO_MATCH_HIT_SHIP'},
    ('short', 'stop'): {'FL': 'HP_KILLED', 'ML': 'HP_KILLED', 'MR': 'HP_KILLED', 'ENG': 'HP_KILLED',
                        'BOGUS': 'SCRIPT_STOPPED_AT_CALL'},
    ('tail', 'type'): {'ENG': 'HP_KILLED', 'BOGUS': 'NO_MATCH_HIT_SHIP'},
    ('tail', 'stop'): {'ENG': 'HP_KILLED', 'BOGUS': 'SCRIPT_STOPPED_AT_CALL'},
    ('full', 'type'): {'FL': 'HP_KILLED', 'ENG': 'HP_KILLED', 'SHLD': 'HP_KILLED', 'TRAC': 'HP_KILLED',
                       'DEATH': 'ALL_LIVE_BREAK_OFF', 'LOWER': 'HP_KILLED', 'COLL': 'HULL_HIT'},
    ('full', 'coll'): {'FL': 'HULL_HIT', 'ENG': 'UNSEEN_BY_LUA', 'SHLD': 'HP_KILLED', 'TRAC': 'UNSEEN_BY_LUA',
                       'DEATH': 'ALL_LIVE_BREAK_OFF', 'LOWER': 'HULL_HIT', 'COLL': 'HP_KILLED'},
}
# report.json "answers" per (profile, variant); NOT_RUN for what the profile skips (never a NO_MATCH_* answer)
ANSWERS = {
    ('short', 'type'): {'lua_type_name': 'KILLS_THE_HARDPOINT', 'lua_lower_case': 'NOT_RUN',
                        'lua_collision_mesh': 'NOT_RUN', 'owner_death': 'NOT_RUN', 'kill_ml': 'HP_KILLED',
                        'kill_mr': 'HP_KILLED', 'kill_eng': 'HP_KILLED', 'kill_shld': 'NOT_RUN',
                        'kill_trac': 'NOT_RUN', 'lua_bogus_name': 'NO_MATCH_UNSEEN_BY_LUA'},
    ('tail', 'stop'): {'lua_bogus_name': 'SCRIPT_STOPPED_AT_CALL', 'kill_eng': 'HP_KILLED', 'kill_ml': 'NOT_RUN'},
    ('short', 'stop'): {'lua_bogus_name': 'SCRIPT_STOPPED_AT_CALL', 'kill_eng': 'HP_KILLED'},
    ('full', 'type'): {'lua_type_name': 'KILLS_THE_HARDPOINT', 'lua_lower_case': 'CASE_INSENSITIVE',
                       'lua_collision_mesh': 'NO_MATCH_HULL_HIT', 'owner_death': 'ALL_LIVE_BREAK_OFF',
                       'kill_eng': 'HP_KILLED', 'kill_shld': 'HP_KILLED', 'kill_trac': 'HP_KILLED',
                       'kill_ml': 'NOT_RUN', 'kill_mr': 'NOT_RUN', 'lua_bogus_name': 'NOT_RUN'},
}


def lua_str(s):
    return '"%s"' % s.replace('\\', '\\\\').replace('"', '\\"')


def run_variant(lua, work, cfg, lookup, human=True, mode='Space', stop=False):
    tag = cfg['profile'] + '_' + lookup + ('_stop' if stop else '')
    hook = os.path.join(work, 'hook_%s.lua' % tag)
    with open(hook, 'w', newline='\n') as f:
        f.write(B.lua_config(cfg) + '\n' + open(os.path.join(HERE, 'r1_hook.lua'), encoding='ascii').read())
    log = os.path.join(work, 'records_%s%s.log' % (tag, '' if human else '_nohuman'))
    script = os.path.join(work, 'run_%s.lua' % tag)
    pre = ['R1_CFG = nil', 'LOOKUP = %s' % lua_str(lookup), 'BOGUS_STOP = %s' % ('true' if stop else 'false'), 'GAME_MODE = %s' % lua_str(mode),
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
        raise RuntimeError('lua run %s failed: %s %s' % (tag, r.stdout[-800:], r.stderr[-800:]))
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
    for prof in B.PROFILES:
        for st in B.stages_for(prof):
            for suf in ('_PRE', '_DONE'):
                if 'TEXT_R1P_' + st['key'] + suf not in keys:
                    fails.append('no text for stage %s%s' % (st['key'], suf))
    if [st['key'] for st in cfg['stages']] != B.PROFILES[B.DEFAULT_PROFILE]['stages']:
        fails.append('default profile stages: %s' % [st['key'] for st in cfg['stages']])
    last = cfg['stages'][-1]
    if last['key'] != 'BOGUS' or last['arg'] != 'HP_NO_SUCH' or not last['final']:
        fails.append('the bogus name is not the last stage: %s' % last)
    # the --lua-camera build must compile too
    cam_mod = os.path.join(work, 'mod_cam')
    B.build(game, cam_mod, source_zip if use_zip else None, no_text=True, lua_camera=True)
    fs2 = os.path.join(cam_mod, 'Data', 'Scripts', 'FreeStore', 'BusyTacticalFreeStore.lua')
    chk2 = os.path.join(work, 'compile_cam.lua')
    with open(chk2, 'w') as f:
        f.write('local f, e = loadfile(%s)\nif not f then print("COMPILE_FAIL " .. e) os.exit(1) end\n'
                'print("COMPILE_OK")\n' % lua_str(fs2.replace('\\', '/')))
    r2 = subprocess.run([lua, chk2], capture_output=True, text=True)
    if 'COMPILE_OK' not in r2.stdout or b'lua_camera = true' not in open(fs2, 'rb').read():
        fails.append('lua-camera build: ' + r2.stdout + r2.stderr)
    print('build: %s, site %s' % ('zip' if use_zip else 'install', cfg['site_note']))
    return cfg


def check_variant(fails, lua, work, cfg, lookup, stop=False):
    prof = cfg['profile']
    vname = 'stop' if stop else lookup
    log = run_variant(lua, work, cfg, lookup, stop=stop)
    lines = open(log).read().splitlines()
    msgs = [x for x in lines if x.startswith('MESSAGES ')]
    rate = [x for x in lines if x.startswith('RATE ')]
    tag = '%s/%s' % (prof, vname)
    if not stop and (not rate or rate[0] != 'RATE 20'):
        fails.append('%s: ServiceRate not restored (%s)' % (tag, rate))
    if not stop and prof == 'full' and (not msgs or 'TEXT_R1P_DONE' not in msgs[0]):
        fails.append('%s: no DONE message' % tag)
    if prof == 'short' and (not msgs or 'TEXT_R1P_BOGUS_PRE' not in msgs[0]):
        fails.append('%s: no BOGUS_PRE message' % tag)
    if prof in ('short', 'tail') and not stop and 'TEXT_R1P_BOGUS_DONE' not in msgs[0]:
        fails.append('%s: no BOGUS_DONE message' % tag)
    if stop and 'TEXT_R1P_BOGUS_DONE' in msgs[0]:
        fails.append('%s: BOGUS_DONE shown although the script stopped' % tag)
    for variant, path in (('ordered', log), ('shuffled', None)):
        if path is None:
            recs = [x for x in lines if x.startswith('R1P|')]
            random.Random(7).shuffle(recs)
            path = os.path.join(work, 'shuffled_%s_%s.log' % (prof, vname))
            with open(path, 'w') as f:
                f.write('\n'.join(recs) + '\n')
        jpath = os.path.join(work, 'report_%s_%s_%s.json' % (prof, vname, variant))
        r1_parse.report(path, os.path.join(work, 'report_%s_%s_%s.txt' % (prof, vname, variant)), jpath)
        summary = json.load(open(jpath))       # what report.json carries
        got = {s['key']: s['verdict'] for s in summary['stages']}
        for k, v in EXPECT[(prof, vname)].items():
            if v == '@':
                if got.get(k) in (None, 'NOT_RUN'):
                    fails.append('%s/%s: stage %s did not run (%s)' % (tag, variant, k, got.get(k)))
            elif got.get(k) != v:
                fails.append('%s/%s: stage %s verdict %s, expected %s' % (tag, variant, k, got.get(k), v))
        for k, v in ANSWERS.get((prof, vname), {}).items():
            if summary['answers'].get(k) != v:
                fails.append('%s/%s: report.json answers[%s] = %s, expected %s' % (tag, variant, k,
                                                                                  summary['answers'].get(k), v))
        for k, v in summary['answers'].items():
            if 'SCRIPT_STOPPED' in str(v) and k != 'lua_bogus_name':
                fails.append('%s/%s: %s reports %s (a step that never ran must be NOT_RUN)' % (tag, variant, k, v))
        if True:
            mode = summary.get('mode') or {}
            if mode.get('mode') != lookup:
                fails.append('%s/%s: name mode %s' % (tag, variant, mode.get('mode')))
        death = next((s for s in summary['stages'] if s['key'] == 'DEATH'), {})
        if prof == 'full' and death.get('death_clone') != 1:
            fails.append('%s/%s: death clone %s' % (tag, variant, death.get('death_clone')))
        if summary['apierr'] or summary['errors']:
            fails.append('%s/%s: apierr %s errors %s' % (tag, variant, summary['apierr'], summary['errors']))
        if not summary['done'] and not stop:
            fails.append('%s/%s: no DONE record' % (tag, variant))
    print('variant %s: %s' % (tag, ', '.join('%s=%s' % kv for kv in sorted(got.items()))))


def check_cli_build(fails, game, source_zip, work):
    """r1_run.py's own build path must carry --profile and --lua-camera into the built free store (run 2 of r1b t07 built
    the default profile although --profile tail was given)."""
    import argparse as ap
    import r1_run
    use_zip = not os.path.isdir(os.path.join(game, 'corruption'))
    for prof, cam in (('tail', True), ('short', False), ('full', False)):
        ns = ap.Namespace(game=game, profile=prof, lua_camera=cam)
        dest = os.path.join(work, 'cli_%s' % prof)
        r1_run.build_mod(ns, dest, source_zip if use_zip else None)
        data = open(os.path.join(dest, 'Data', 'Scripts', 'FreeStore', 'BusyTacticalFreeStore.lua'), 'rb').read()
        want = [b'profile = "%s"' % prof.encode(), b'lua_camera = %s' % (b'true' if cam else b'false')]
        if prof == 'tail':
            want += [b'turn_dy = 3000.0', b'first_kill = 1800']
        for w in want:
            if w not in data:
                fails.append('cli build %s: built free store lacks %s' % (prof, w))
    print('cli build: profile and lua_camera reach the built free store')


def check_old_run(fails):
    """The 2026-11-08 full run (23:54, script stopped before stage 7): the new parser must give NOT_RUN for COLL and carry
    kill_eng / kill_shld / kill_trac in report.json. Skipped when that run folder is not on this machine."""
    cands = [os.path.join(ROOT, 'tools', 'probe_mod', 'r1_hardpoint', 'out', '20261108-235419', 'records.log'),
             os.path.join(ROOT, '..', '..', '..', 'tools', 'probe_mod', 'r1_hardpoint', 'out', '20261108-235419',
                          'records.log')]
    path = next((c for c in cands if os.path.isfile(c)), None)
    if not path:
        print('old run 20261108-235419: skipped (records.log not found)')
        return
    summ, _ = r1_parse.analyse(r1_parse.load(path))
    want = {'lua_type_name': 'KILLS_THE_HARDPOINT', 'lua_lower_case': 'CASE_INSENSITIVE',
            'lua_collision_mesh': 'SCRIPT_STOPPED_AFTER_CALL', 'owner_death': 'ALL_LIVE_BREAK_OFF', 'kill_eng': 'HP_KILLED',
            'kill_shld': 'HP_KILLED', 'kill_trac': 'HP_KILLED', 'kill_ml': 'NOT_RUN', 'kill_mr': 'NOT_RUN',
            'lua_bogus_name': 'NOT_RUN'}
    for k, v in want.items():
        if summ['answers'].get(k) != v:
            fails.append('old run: answers[%s] = %s, expected %s' % (k, summ['answers'].get(k), v))
    print('old run 20261108-235419: %s' % ', '.join('%s=%s' % kv for kv in sorted(summ['answers'].items())))


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
        check_variant(fails, lua, work, cfg, 'type', stop=True)   # retail stops the script at the bogus name
        tail = B.config(cfg['site'], cfg['site_note'], cfg['minimap'], 'tail')
        check_variant(fails, lua, work, tail, 'type')
        check_variant(fails, lua, work, tail, 'type', stop=True)
        full = B.config(cfg['site'], cfg['site_note'], cfg['minimap'], 'full')
        for lookup in ('type', 'coll'):
            check_variant(fails, lua, work, full, lookup)
        check_cli_build(fails, a.game, a.source_zip, work)
        check_old_run(fails)
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
