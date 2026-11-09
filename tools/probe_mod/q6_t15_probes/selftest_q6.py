#!/usr/bin/env python3
"""selftest_q6.py - offline check of the Q6 t15 probe kit: builds the vanilla mod from extracted/foc_data_text.zip
(--source-zip, no text file), runs every probe Lua under a stock Lua 5.0.2 interpreter with the game's own script
library (Data\\Scripts\\Library from the zip) and stubbed engines, and builds report.txt from the records they keep.

  selftest_q6.py [--lua <lua50.exe>]   (default tools\\probe_mod\\out\\tools\\lua50.exe, built by
                                        tools\\probe_mod\\common\\build_lua50.cmd)

Two engine models, each must give its own answers in the report:
  port  plot threads resumed once at load; first-pump draw bound = ServiceRate; ELIM B ends a few s after the take-back
        (the port's latch), A runs to the end, C ends at once; Perpetual ELAPSED re-inits at the next service
  alt   no load-time resume (Begin0 fires before main and is dropped); bound = 1; ELIM B runs to the end
The TR land hook runs on a stub free store (TR's real LandTacticalFreeStore needs TR's whole library); the real built
TR file and every built vanilla file are also compiled (loadfile) to catch syntax errors. Ends with Q6 SELFTEST OK.
"""
import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
import lua50  # noqa: E402
import build_q6_mod  # noqa: E402
import q6_parse  # noqa: E402

DEFAULT_LUA = lua50.LUA50

PRELUDE = r'''
Q6_OUT = io.open(Q6_LOG, "w")
function W(s) Q6_OUT:write(s .. "\n") end
Script = "Q6Test"
NOW = 0
function GetCurrentTime() return NOW end
TV = {}
local function tv()
	local co = CUR or "main"
	if not TV[co] then TV[co] = {} end
	return TV[co]
end
ThreadValue = setmetatable({ Set = function(k, v) tv()[k] = v end, Reset = function() end },
	{ __call = function(_, k) return tv()[k] end })
GetEvent = setmetatable({ Reset = function() end, Params = function() return nil end },
	{ __call = function() return nil end })
function _ScriptMessage(s) end
function _OuputDebug(s) end
function _MessagePopup(s) end
function _CustomScriptMessage(s) end
function Game_Message(s) W("GAME_MESSAGE " .. tostring(s)) end
function DumpCallStack() return debug.traceback() end
function ScriptExit() W("SCRIPT_EXIT") error("ScriptExit") end
function GetThreadID() if CUR == nil or CUR == MAIN_CO then return 0 end return 1 end
THREADS = {}
function Create_Thread(name, param)
	W("CREATE_THREAD " .. name)
	table.insert(THREADS, coroutine.create(function() _G[name](param) end))
end
function RESUME(co)
	CUR = co
	local ok, err = coroutine.resume(co)
	CUR = nil
	if not ok and tostring(err) ~= "ScriptExit" and not string.find(tostring(err), "ScriptExit") then
		W("LUA_ERROR " .. tostring(err))
	end
	return ok
end
function DUMP(t) for i = 1, table.getn(t) do W("REC " .. t[i]) end end
-- generic object: unknown methods are no-ops returning false
function OBJ(fields)
	local o = fields or {}
	setmetatable(o, { __index = function(t, k)
		if type(k) == "string" and string.find(k, "^Get_") then return function() return 0 end end
		if type(k) == "string" and string.find(k, "^[A-Z]") then return function() return false end end
		return nil
	end })
	o.Is_Valid = function() return not o.dead end
	o.Get_Type = function() return { Get_Name = function() return o.tname or "?" end } end
	o.Get_ID = function() return o.id end
	o.Get_Owner = function() return o.owner end
	o.Get_Hull = function() return o.hull or 1 end
	o.Get_Parent_Object = function() return o.parent end
	o.Is_Category = function(c) return c == "Structure" and o.structure == true end
	o.Get_Position = function()
		return { Get_XYZ = function() return o.x or 0, o.y or 0, 0 end }
	end
	o.Get_Distance = function(b)
		local dx, dy = (o.x or 0) - (b.x or 0), (o.y or 0) - (b.y or 0)
		return math.sqrt(dx * dx + dy * dy)
	end
	return o
end
function PLAYER(name, human)
	local p = OBJ({ tname = name })
	p.Get_Faction_Name = function() return string.upper(name) end
	p.Is_Human = function() return human end
	p.Is_Enemy = function(q) return q ~= p end
	p.Get_ID = function() return name end
	p.Get_Space_Station = function() return nil end
	return p
end
'''

STORY_DRIVER = r'''
EV = { { n = "Q6S_Begin0", T = 0 }, { n = "Q6S_Begin3", T = 3 }, { n = "Q6S_Once2", T = 2 },
	{ n = "Q6S_Cycle2", T = 2, p = true }, { n = "Q6S_Cycle1", T = 1, p = true }, { n = "Q6S_Cycle07", T = 0.7, p = true },
	{ n = "Q6S_Cycle40", T = 40, p = true } }
for _, e in ipairs(EV) do e.acc = 0; e.init = false; e.active = true end
Base_Definitions()
MAIN_CO = coroutine.create(main)
function SERVICE()
	if type(ServiceRate) ~= "number" then return end
	if LastService == nil or NOW - LastService > ServiceRate then
		local first = LastService == nil
		if coroutine.status(MAIN_CO) == "suspended" then RESUME(MAIN_CO) end
		for _, co in ipairs(THREADS) do if coroutine.status(co) == "suspended" then RESUME(co) end end
		if first then LastService = -(DRAW * ServiceRate) else LastService = NOW end
	end
end
if MODEL == "port" then RESUME(MAIN_CO) end
for f = 1, 4200 do
	NOW = f * 0.033333335
	for _, e in ipairs(EV) do
		if e.active then
			if not e.init then
				e.init = true
				e.last = NOW
			else
				e.acc = e.acc + (NOW - e.last)
				e.last = NOW
			end
			if e.acc >= e.T then
				Story_Event_Trigger(e.n)
				if e.p then e.acc = 0; e.init = false else e.active = false end
			end
		end
	end
	SERVICE()
end
W("STATE " .. tostring(CurrentState))
DUMP(Q6S_LOG)
Q6_OUT:close()
'''

ELIM_STUBS = r'''
PL = { Empire = PLAYER("Empire", true), Rebel = PLAYER("Rebel", false), Neutral = PLAYER("Neutral", false),
	Underworld = PLAYER("Underworld", false), Pirates = PLAYER("Pirates", false) }
function Find_Player(n) return PL[n] end
PLANETS = {}
for _, n in ipairs({ "Tatooine", "Byss", "Abregado_Rae", "Jabiim" }) do
	PLANETS[n] = OBJ({ tname = n, owner = PL.Neutral })
	local p = PLANETS[n]
	p.Change_Owner = function(q) p.owner = q end
end
PLANETS.Jabiim.owner = PL.Rebel
function FindPlanet(n) return PLANETS[n] end
function Find_All_Objects_Of_Type(x) return {} end
'''

ELIM_DRIVER = r'''
Base_Definitions()
MAIN_CO = coroutine.create(main)
RESUME(MAIN_CO)
local fired = false
for f = 1, 5400 do
	NOW = f / 30
	if END_T and NOW >= END_T then break end
	if not fired and NOW >= 1 then fired = true; Story_Event_Trigger("Q6E_Begin") end
	if LastService == nil or NOW - LastService > ServiceRate then
		if coroutine.status(MAIN_CO) == "suspended" then RESUME(MAIN_CO) end
		for _, co in ipairs(THREADS) do if coroutine.status(co) == "suspended" then RESUME(co) end end
		LastService = NOW
	end
end
DUMP(Q6E_LOG)
Q6_OUT:close()
'''

TACT_STUBS = r'''
PL = { Rebel = PLAYER("Rebel", false), Empire = PLAYER("Empire", true), Underworld = PLAYER("Underworld", false),
	Pirates = PLAYER("Pirates", false), Hostile = PLAYER("Hostile", false), Neutral = PLAYER("Neutral", false),
	Hutts = PLAYER("Hutts", false), Sarlacc = PLAYER("Sarlacc", false) }
function Find_Player(n) return PL[n] end
function Get_Game_Mode() return MODE end
ALL = {}
NEXT_ID = 100
function NEWOBJ(tname, owner, x, y, structure)
	NEXT_ID = NEXT_ID + 1
	local o = OBJ({ tname = tname, owner = owner, x = x, y = y, id = NEXT_ID, structure = structure })
	table.insert(ALL, o)
	return o
end
for i = 1, 4 do NEWOBJ("Rebel_Unit", PL.Rebel, 5000 + i * 10, 5000) end
for i = 1, 4 do NEWOBJ("Empire_Unit", PL.Empire, 1000 + i * 10, 1000) end
function Find_All_Objects_Of_Type(x)
	local out = {}
	for _, o in ipairs(ALL) do if o.owner == x and not o.dead then table.insert(out, o) end end
	return out
end
function Find_Object_Type(t) return { name = t } end
function Create_Position(x, y, z) return { Get_XYZ = function() return x, y, z end, x = x, y = y } end
function Spawn_Unit(t, pos, player)
	local x, y = pos.Get_XYZ()
	local o = NEWOBJ(t.name, player, x, y)
	if t.name == "Imperial_Stormtrooper_Squad" then
		for i = 1, 3 do local m = NEWOBJ("Imperial_Stormtrooper", player, x, y); m.parent = o end
	end
	return { o }
end
function Create_Generic_Object(t, pos, player)
	if t.name == "Proximity_Mine" then return nil end
	local x, y = pos.Get_XYZ()
	return NEWOBJ(t.name, player, x, y, true)
end
FindTarget = { Reachable_Target = function() return nil end }
function EvaluatePerception() return 0 end
function FindDeadlyEnemy() return nil end
'''

TACT_DRIVER = r'''
Base_Definitions()
PlayerObject = PL.Rebel
MAIN_CO = coroutine.create(main)
for f = 1, 3900, 3 do
	NOW = f / 30
	if LastService == nil or NOW - LastService > ServiceRate then
		RESUME(MAIN_CO)
		LastService = NOW
	end
	On_Unit_Service(nil)
end
DUMP(Q6T_LOG)
Q6_OUT:close()
'''

OBJ_DRIVER = r'''
Base_Definitions()
MAIN_CO = coroutine.create(main)
local owner = PLAYER("Rebel", false)
Object = OBJ({ tname = OBJ_TYPE, owner = owner, id = OBJ_ID_V })
function Get_Game_Mode() return MODE end
function FindDeadlyEnemy() return nil end
function Find_Object_Type(t) return { name = t } end
function Find_All_Objects_Of_Type(x) return {} end
function Find_Nearest() return nil end
function EvaluatePerception() return 0 end
for f = F0 + 1, F0 + 1200 do
	NOW = f * 0.033333335
	if type(ServiceRate) == "number" and (LastService == nil or NOW - LastService > ServiceRate) then
		local first = LastService == nil
		if coroutine.status(MAIN_CO) == "suspended" then RESUME(MAIN_CO) end
		if HI == "rate" then
			if first then LastService = -(DRAW * ServiceRate) else LastService = NOW end
		else
			if first then LastService = -(DRAW * 1.0) else LastService = NOW end
		end
	end
end
DUMP(Q6O_LOG)
Q6_OUT:close()
'''

TR_STUB_FREESTORE = r'''
function Base_Definitions()
	ServiceRate = 20
	UnitServiceRate = 2
end
function main()
	while 1 do
		FreeStoreService()
		coroutine.yield(true)
	end
end
function FreeStoreService() end
function On_Unit_Service(object) end
'''

TR_DRIVER = r'''
PL = { Empire = PLAYER("Empire", false), Rebel = PLAYER("Rebel", true) }
function Find_Player(n) return PL[n] end
function Get_Game_Mode() return "Land" end
ALL = {}
local id = 0
local function new(t, owner, x, y, s, die)
	id = id + 1
	local o = OBJ({ tname = t, owner = owner, x = x, y = y, id = id, structure = s, die = die })
	table.insert(ALL, o)
	return o
end
CC = new("Rebel_Command_Center", PL.Rebel, 0, 0, true, 20000)
new("R_Ground_Barracks", PL.Rebel, 100, 0, true, 3000)
new("R_Ground_Light_Vehicle_Factory", PL.Rebel, 200, 0, true, 6000)
new("Rebel_Anti_Infantry_Turret", PL.Rebel, 300, 0, true, 9000)
for i = 1, 5 do new("Rebel_Infantry_Squad", PL.Rebel, 50, 50, false, 1500) end
for i = 1, 8 do new("Imperial_Unit", PL.Empire, 1000 + i * 100, 0, false, nil) end
function Find_All_Objects_Of_Type(x)
	local out = {}
	for _, o in ipairs(ALL) do if o.owner == x and not o.dead then table.insert(out, o) end end
	return out
end
Base_Definitions()
PlayerObject = PL.Empire
MAIN_CO = coroutine.create(main)
for f = 1, 24000 do
	NOW = f / 30
	for _, o in ipairs(ALL) do
		if o.die and f >= o.die then o.dead = true end
	end
	CC.hull = 0.2 + 0.5 * math.mod(f, 3000) / 3000
	if LastService == nil or NOW - LastService > ServiceRate then RESUME(MAIN_CO); LastService = NOW end
	if math.mod(f, 60) == 0 then On_Unit_Service(nil) end
end
DUMP(Q6L_LOG)
Q6_OUT:close()
'''


def esc(p):
    return p.replace('\\', '\\\\')


class Runner:
    def __init__(self, lua, out_dir, lib):
        self.lua, self.out_dir, self.lib = lua, out_dir, lib
        self.n = 0
        self.errors = []

    def run(self, tag, header, body_parts):
        self.n += 1
        log = os.path.join(self.out_dir, '%s.txt' % tag)
        script = os.path.join(self.out_dir, '%s.lua' % tag)
        with open(script, 'w', encoding='latin-1', newline='\n') as f:
            f.write('Q6_LOG = "%s"\n%s\n' % (esc(log), header))
            f.write(PRELUDE)
            for part in body_parts:
                f.write(part.replace('\r\n', '\n') + '\n')
        env = dict(os.environ, LUA_PATH=os.path.join(self.lib, '?.lua'))
        r = subprocess.run([self.lua, script], capture_output=True, text=True, env=env)
        text = open(log, encoding='latin-1').read() if os.path.exists(log) else ''
        if r.returncode != 0 or 'LUA_ERROR' in text:
            self.errors.append('%s: rc=%d %s %s' % (tag, r.returncode, (r.stdout + r.stderr)[-400:],
                                                    ' '.join(ln for ln in text.splitlines() if 'LUA_ERROR' in ln)[:400]))
        return text

    def records(self, text):
        return [ln[4:] for ln in text.splitlines() if ln.startswith('REC ')]


def library(dst):
    z = zipfile.ZipFile(os.path.join(ROOT, 'extracted', 'foc_data_text.zip'))
    n = 0
    for info in z.infolist():
        p = info.filename.replace('\\', '/').upper()
        if '/SCRIPTS/LIBRARY/' in p and p.endswith('.LUA'):
            with open(os.path.join(dst, os.path.basename(info.filename)), 'wb') as f:
                f.write(z.read(info))
            n += 1
    return n


def read(path):
    return open(path, encoding='latin-1').read()


def compile_check(runner, files):
    bad = []
    for p in files:
        tag = 'compile_%d' % runner.n
        text = runner.run(tag, '', ['local f, e = loadfile("%s")\nif f then W("COMPILED") else W("LUA_ERROR " .. tostring(e)) end'
                                    '\nQ6_OUT:close()' % esc(p)])
        if 'COMPILED' not in text:
            bad.append(p)
    return bad


def run_model(runner, mod, model, rng):
    recs = []
    sdir = os.path.join(mod, 'Data', 'Scripts')
    timing = read(os.path.join(sdir, 'Story', 'Q6_Story_Timing.lua'))
    t = runner.run('%s_story' % model, 'MODEL = "%s"\nDRAW = 0.37' % model, [timing, STORY_DRIVER])
    recs += runner.records(t)
    story_text = t
    # D / E: the RE reading (end about 1.5 s after the event; the plot starts at t = 1.17) in 'port', no end in 'alt'.
    ends = {'A': None, 'B': 55.0 if model == 'port' else None, 'C': 0.5,
            'D': 23.0 if model == 'port' else None, 'E': 43.0 if model == 'port' else None}
    for m, end in ends.items():
        elim = read(os.path.join(sdir, 'Story', 'Q6_Story_Elim_%s.lua' % m))
        hdr = 'END_T = %s' % ('nil' if end is None else repr(end))
        recs += runner.records(runner.run('%s_elim_%s' % (model, m), hdr, [ELIM_STUBS, elim, ELIM_DRIVER]))
    fs = read(os.path.join(sdir, 'FreeStore', 'BusyTacticalFreeStore.lua'))
    spawns = []
    for mode in ('Land', 'Space'):
        t = runner.run('%s_tact_%s' % (model, mode), 'MODE = "%s"' % mode, [TACT_STUBS, fs, TACT_DRIVER])
        rr = runner.records(t)
        recs += rr
        for line in rr:
            k, kv = q6_parse.parse_line(line)
            if k == 'SPAWN' and kv.get('id', '-') != '-':
                spawns.append((mode, kv))
    for i, (mode, kv) in enumerate(spawns):
        script = read(os.path.join(sdir, 'GameObject', '%s.lua' % kv['script']))
        hdr = ('MODE = "%s"\nOBJ_TYPE = "%s"\nOBJ_ID_V = "%s"\nF0 = %s\nDRAW = %.6f\nHI = "%s"' % (
            mode, kv['type'], kv['id'], kv['f'], rng.random(), 'rate' if model == 'port' else 'one'))
        recs += runner.records(runner.run('%s_obj_%d' % (model, i), hdr, [script, OBJ_DRIVER]))
    return recs, story_text, len(spawns)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--lua', default=DEFAULT_LUA)
    a = ap.parse_args()
    if not os.path.isfile(a.lua):
        print('lua50.exe not found (%s): run tools\\probe_mod\\common\\build_lua50.cmd' % a.lua)
        return 2
    out_dir = os.path.join(HERE, 'out', 'selftest')
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    lib = os.path.join(out_dir, 'library')
    os.makedirs(lib)
    ok = library(lib) > 0
    mod = os.path.join(out_dir, 'mod', build_q6_mod.MOD_NAME)
    build_q6_mod.build_vanilla(None, mod, os.path.join(ROOT, 'extracted', 'foc_data_text.zip'), no_text=True)
    runner = Runner(a.lua, out_dir, lib)
    built = []
    for d, _, files in os.walk(mod):
        built += [os.path.join(d, f) for f in files if f.lower().endswith('.lua')]
    tr_built = None
    if os.path.isdir(os.path.join(build_q6_mod.DEFAULT_TR, 'Data')):
        trm = os.path.join(out_dir, 'mod', build_q6_mod.TR_MOD_NAME)
        build_q6_mod.build_tr(build_q6_mod.DEFAULT_TR, trm)
        tr_built = os.path.join(trm, 'Data', 'Scripts', 'FreeStore', 'LandTacticalFreeStore.lua')
        built.append(tr_built)
    bad_compile = compile_check(runner, built)
    checks = {'library': ok, 'compile %d built Lua files' % len(built): not bad_compile}
    # TR land hook on the stub free store
    hook = open(os.path.join(HERE, 'q6_tr_land_hook.lua'), encoding='ascii').read().replace(
        '@@FACTIONS@@', '"Empire", "Rebel"')
    tr_recs = runner.records(runner.run('tr_land', '', [TR_STUB_FREESTORE, hook, TR_DRIVER]))
    rng = random.Random(20261104)
    expect = {
        'port': {1: ['ANSWER A (own nothing all game): RAN_TO_END', 'ANSWER B (owned once, then nothing): STOPPED',
                     'ANSWER D (AI gains a planet while the human owns nothing): STOPPED',
                     'ANSWER E (fleet, then no planet): STOPPED', 'after the AIGIVE event'],
                 2: ['ANSWER bound: hi = ServiceRate', 'ANSWER vanilla objects run their Lua_Script: YES'],
                 3: ['ANSWER: Cycle2 period'],
                 4: ['ANSWER: MAIN (f=', 'Begin0 state entered'],
                 5: ['the CC survived'],
                 6: ['spawned for you: Darth_Vader', 'spawned for you: Eclipse_Super_Star_Destroyer']},
        'alt': {1: ['ANSWER B (owned once, then nothing): RAN_TO_END',
                    'ANSWER D (AI gains a planet while the human owns nothing): RAN_TO_END',
                    'ANSWER E (fleet, then no planet): RAN_TO_END'],
                2: ['ANSWER bound: hi = 1'],
                4: ['BEFORE main\'s first resume', 'Begin0 state NEVER entered']},
    }
    for model in ('port', 'alt'):
        recs, story_text, nsp = run_model(runner, mod, model, rng)
        recs += tr_recs
        rl = os.path.join(out_dir, 'records_%s.log' % model)
        with open(rl, 'w') as f:
            f.write('\n'.join(recs) + '\n')
        text, parsed = q6_parse.report(rl, os.path.join(out_dir, 'report_%s.txt' % model))
        secs = {}
        cur = None
        for ln in text.splitlines():
            m = re.match(r'== (\d)\.', ln)
            if m:
                cur = int(m.group(1))
            if cur:
                secs.setdefault(cur, []).append(ln)
        for sec, needles in expect[model].items():
            body = '\n'.join(secs.get(sec, []))
            for nd in needles:
                checks['%s sec %d has "%s"' % (model, sec, nd)] = nd in body
        checks['%s spawns (land 3x7 + space 4x7)' % model] = nsp == 49
        checks['%s story thread' % model] = story_text.count('CREATE_THREAD Q6S_Watch') == 1
        checks['%s every probe text exists' % model] = all(
            t in dict(build_q6_mod.TEXTS) for t in re.findall(r'GAME_MESSAGE (TEXT_Q6_\w+)', story_text))
    bad = [k for k, v in checks.items() if not v]
    for k, v in checks.items():
        print('%-70s %s' % (k, 'ok' if v else 'FAILED'))
    for e in runner.errors:
        print('LUA RUN PROBLEM:', e)
    for p in bad_compile:
        print('COMPILE FAILED:', p)
    good = not bad and not runner.errors
    print('reports: %s' % os.path.join(out_dir, 'report_port.txt'))
    print('Q6 SELFTEST OK' if good else 'Q6 SELFTEST FAILED')
    return 0 if good else 1


if __name__ == '__main__':
    sys.exit(main())
