#!/usr/bin/env python3
r"""selftest_a2f.py - offline check of the a2f t07 budget probe (no game needed).

  selftest_a2f.py [--lua <lua50.exe>] [--out DIR]

  --lua  a stock Lua 5.0.2 interpreter (tools\probe_mod\a1_tactical\selftest\build_lua50.cmd builds one at
         tools\probe_mod\a1_tactical\out\tools\lua50.exe, which is the default). Without it the Lua checks are skipped.

Checks (against extracted/foc_data_text.zip, the shipped FoC text data):
  spec      every shipped equation the probe evaluates exists; none of the galactic equation files contains the '#'
            operator (no RNG draw); the probe types, thread functions and plan scripts exist; the patched equation file is
            well-formed XML with the 2 + 3 x 12 probe equations
  build     the mod builds from the zip alone (no install): all files present, every generated Lua loads (stock Lua 5.0.2)
  story     the story script runs under Lua 5.0.2 with the game's PG library and a model engine (budget not initialised
            until 0.5 s, then pools = credits x template weights): dense samples from the start, fiscal windows, full
            samples, DONE at 300 s; the report decodes pre-Init, the first real pools, the ALL row = credits, and picks
            the right template
  hooks     the free-store wrapper samples and still calls the shipped function; each of the six real plan scripts loads
            with its hook and its wrapped thread records PSTART (Budget answers) and still runs
  watcher   a2f_watch.py follows a synthetic player -> AI -> context -> budget chain, logs the init byte change and a reset
            flag 0 -> 1 -> 0
  cleanup   the cleanup removes only a marked mod folder

Retail emulation (a2i t02, after run 20261106-040725 lost the story channel): the model engine knows only the equations
of the shipped galactic files plus the probe's own. An evaluation of an unknown equation, or of a probe equation whose
body breaks a retail rule (retail_eval_fails: ActiveGoals without exactly one Parameter_Category), sets the model's
script-failed flag and returns no value, as retail's script error helper does (docs/re/LUA_SCRIPT_OUTPUT.md section 1,
AI_GALACTIC_3 section 8); pcall does not see it. The drivers then stop servicing that script, as retail did in that run.
math is removed from every probe environment (no shipped script uses it; the free store has none), debug from the
story and free-store ones.
  robust    a fault injected into one record (an error inside the Underworld's SM text) costs only that record: one ERR
            record, the other records of that sample and of the other players are still written
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
import a2f_cleanup  # noqa: E402
import a2f_report  # noqa: E402
import a2f_watch  # noqa: E402
import a2fb_spec  # noqa: E402
import build_a2f_mod  # noqa: E402

ZIP = os.path.join(ROOT, 'extracted', 'foc_data_text.zip')
RESULTS = []


def check(name, ok, detail=''):
    RESULTS.append((name, bool(ok)))
    print('%-58s %s%s' % (name, 'ok' if ok else 'FAILED', ('  ' + detail) if detail and not ok else ''))
    return ok


def zread(z, names, path):
    key = path.replace('\\', '/').upper()
    return z.read(names[key]).decode('latin-1')


def spec_checks(z):
    names = {i.filename.upper(): i for i in z.infolist()}
    eqdir = 'DATA/XML/AI/PERCEPTUALEQUATIONS/'
    eqtext = {n: z.read(i).decode('latin-1') for n, i in names.items() if n.startswith(eqdir)}
    all_eq = '\n'.join(eqtext.values())
    missing = []
    for fkey, lst in a2fb_spec.SHIPPED.items():
        for n, _k in lst:
            if not re.search(r'<%s>' % re.escape(n), all_eq, re.I):
                missing.append(n)
    for n, _k, _h in a2fb_spec.MAJOR_SHIPPED:
        if not re.search(r'<%s>' % re.escape(n), all_eq, re.I):
            missing.append(n)
    check('spec: shipped equations exist', not missing, str(missing))
    galactic = [t for n, t in eqtext.items() if 'LAND' not in n and 'SPACE' not in n]
    check('spec: galactic equation files have no # operator',
          not any('#' in re.sub(r'<!--.*?-->', '', t, flags=re.S) for t in galactic))
    xml = [zread(z, names, build_a2f_mod.SPECIAL_STRUCTURES[0].replace('\\', '/')),
           zread(z, names, build_a2f_mod.SPECIAL_STRUCTURES[1].replace('\\', '/'))]
    types = a2fb_spec.parse_type_data(xml, a2fb_spec.probe_type_names())
    check('spec: probe types have cost and build time', len(types) == 6, str(types))
    ok = True
    for _f, _f3, _p, t1, t2 in a2fb_spec.FACTIONS:
        ok = ok and types[t1]['time'] != types[t2]['time']
    check('spec: the two probe types of a faction differ in build time', ok)
    for name, thread in build_a2f_mod.PLANS:
        t = zread(z, names, 'DATA/SCRIPTS/AI/%s.LUA' % name.upper())
        check('spec: plan %s defines %s' % (name, thread),
              re.search(r'function\s+%s\s*\(' % thread, t) is not None)
    check('spec: a2h plans hooked (HVF, AdvancedVF, LiftBlockade, FlushMajorItem, ConquerPirate)',
          {'BuildBaseComponentHeavyVehicleFactory', 'BuildBaseComponentAdvancedVehicleFactory', 'LiftBlockadePlan',
           'FlushMajorItemBudget', 'ConquerPiratePlan'} <= {n for n, _t in build_a2f_mod.PLANS})
    pe = a2fb_spec.patch_equation_file(zread(z, names, a2fb_spec.PROBE_EQ_FILE.replace('\\', '/')))
    try:
        root = ET.fromstring(pe)
        n = [e.tag for e in root if e.tag.startswith('A2FB_')]
        want = a2fb_spec.probe_equation_count()
        check('spec: patched equation file is XML with %d probe equations' % want, len(n) == want == 2 + 36 + 8 + 11,
              str(len(n)))
        check('spec: shipped equations are kept', root.find('Roaming_Empire_Fleet_Target') is not None)
        bad = [e.tag for e in root if e.tag.startswith('A2FB_') and '#' in (e.text or '')]
        check('spec: probe equations have no #', not bad)
        bad = sorted(k for k, b in probe_equation_bodies().items() if retail_eval_fails(b))
        check('spec: no probe equation fails in retail (ActiveGoals needs one Parameter_Category)', not bad, str(bad))
    except ET.ParseError as e:
        check('spec: patched equation file is XML', False, str(e))
    a2j_spec_checks(z, names, all_eq)
    return types


def eq_body(all_eq, name):
    m = re.search(r'<%s>(.*?)</%s>' % (re.escape(name), re.escape(name)), all_eq, re.S | re.I)
    return re.sub(r'\s+', ' ', m.group(1)).strip() if m else None


def eq_closure(all_eq, name, seen=None):
    """{equation names} reachable from name through Function_<X>.Evaluate (the shipped galactic files)."""
    seen = set() if seen is None else seen
    if name in seen:
        return seen
    seen.add(name)
    body = eq_body(all_eq, name) or ''
    for sub in re.findall(r'Function_(\w+)\.Evaluate', body):
        eq_closure(all_eq, sub, seen)
    return seen


def a2j_spec_checks(z, names, all_eq):
    """a2j t02: the D4 pairs / equations and the D3 GameScoring events (what makes each read safe in retail)."""
    galactic = '\n'.join(re.sub(r'<!--.*?-->', '', z.read(i).decode('latin-1'), flags=re.S) for n, i in names.items()
                         if n.startswith('DATA/XML/AI/PERCEPTUALEQUATIONS/') and 'LAND' not in n and 'SPACE' not in n)
    missing = [n for n in a2fb_spec.PERC_SHIPPED_NAMES if eq_body(galactic, n) is None]
    check('a2j spec: D4 shipped equations exist in the galactic files', not missing, str(missing))
    closure = eq_closure(galactic, 'Should_Conquer_Pirate_Planet')
    check('a2j spec: every D4 shipped equation is Should_Conquer_Pirate_Planet or one of its sub-functions',
          set(a2fb_spec.PERC_SHIPPED_NAMES) <= closure, str(set(a2fb_spec.PERC_SHIPPED_NAMES) - closure))
    bodies = ' | '.join(eq_body(galactic, n) or '' for n in sorted(closure))
    bad = [n for n, _k, b in a2fb_spec.PERC_MOD if re.search(r'%s(?![\w.{])' % re.escape(b), bodies) is None]
    check('a2j spec: every D4 token equation is, verbatim, a token of Should_Conquer_Pirate_Planet or its sub-functions',
          not bad, str(bad))
    human = sorted(n for n in closure if 'Variable_Human' in (eq_body(galactic, n) or ''))
    need = {n for n, _k, h in a2fb_spec.PERC_SHIPPED if h}
    ok = all((n in need) == bool(eq_closure(galactic, n) & set(human)) for n in a2fb_spec.PERC_SHIPPED_NAMES)
    check('a2j spec: D4 shipped equations that reach Variable_Human are marked needs-human', ok, str(human))
    for _f, _p, _pl, _s, plot in a2fb_spec.PERC_PAIRS:
        t = zread(z, names, 'DATA/XML/' + plot.upper())
        check('a2j spec: %s exists with no Lua_Script of its own' % plot,
              '<Story_Mode_Plots>' in t and not re.search(r'<Lua_Script>\s*\S', t))
    gs = zread(z, names, build_a2f_mod.SCORING.replace('\\', '/'))
    check('a2j spec: GameScoring.lua defines the three galactic production events', all(
        re.search(r'function\s+%s\s*\(' % e, gs) for e in ('Galactic_Production_Begin_Event',
                                                           'Galactic_Production_Canceled_Event',
                                                           'Galactic_Production_End_Event')))
    check('a2j spec: shipped GameScoring makes the reads the Q hook makes (Get_Type().Get_Name, Get_Owner, Get_Build_Cost, '
          'Get_Type == nil)', all(s in gs for s in ('planet.Get_Type()', 'planet_type.Get_Name()', 'planet.Get_Owner()',
                                                 'object.Get_Build_Cost()', 'object.Get_Type == nil')))


def retail_eval_fails(body):
    """True when retail can never evaluate this equation body: the evaluation fails, the script error helper sets the
    calling script's failed flag and that script stops (run 20261106-040725: A2FB_ActiveGoals = Variable_Self.ActiveGoals
    with no Parameter_Category stopped the story script and the Empire's free store after their first sample).
    Rule: ActiveGoals reads exactly one Parameter_Category, with no default (A2H_GALACTIC_AI_RE.md section 1)."""
    for m in re.finditer(r'\.ActiveGoals\b(\s*\{[^}]*\})?', body):
        if (m.group(1) or '').count('Parameter_Category') != 1:
            return True
    return False


def probe_equation_bodies():
    """{name: body} of the probe equations the mod adds (a2fb_spec.equations_xml)."""
    return {m.group(1): m.group(2).strip()
            for m in re.finditer(r'<(A2FB_\w+)>(.*?)</\1>', a2fb_spec.equations_xml(), re.S)}


MODEL_EQ = ''


def model_equations(z):
    """Lua tables of the model engine: A2_KNOWN_EQ (shipped galactic equations + probe equations) and A2_FAIL_EQ (probe
    equations that fail in retail)."""
    known = set()
    for info in z.infolist():
        n = info.filename.upper()
        if n.startswith('DATA/XML/AI/PERCEPTUALEQUATIONS/') and n.endswith('.XML') and 'LAND' not in n and 'SPACE' not in n:
            text = re.sub(r'<!--.*?-->', '', z.read(info).decode('latin-1'), flags=re.S)
            known |= set(re.findall(r'^\s*<([A-Za-z_]\w*)>', text, re.M))
    bodies = probe_equation_bodies()
    known |= set(bodies)
    fails = sorted(n for n, b in bodies.items() if retail_eval_fails(b))
    return ('A2_KNOWN_EQ = { %s }\nA2_FAIL_EQ = { %s }\nA2_NEEDS_HUMAN = { %s }\n' %
            (', '.join('["%s"] = true' % n for n in sorted(known)), ', '.join('["%s"] = true' % n for n in fails),
             ', '.join('["%s"] = true' % n for n, _k, h in a2fb_spec.PERC_SHIPPED if h)))


def prelude(outfile, types, no_debug=True):
    """Head of every test script: output file, init time, probe types, the model equation tables, the stubs and the
    retail library set (math removed everywhere; debug removed unless no_debug is False)."""
    return ('A2_OUTFILE = "%s"\nA2_INIT_T = 0.5\n' % outfile.replace('\\', '\\\\') + types_lua(types) + MODEL_EQ + STUBS +
            'math = nil\n' + ('debug = nil\n' if no_debug else ''))


def lua_run(lua, script, env=None, cwd=None):
    r = subprocess.run([lua, script], capture_output=True, text=True, env=env, cwd=cwd)
    return r.returncode, r.stdout + r.stderr


def extract(z, dst):
    lib = os.path.join(dst, 'library')
    os.makedirs(lib, exist_ok=True)
    for info in z.infolist():
        p = info.filename.replace('\\', '/').upper()
        if ('/SCRIPTS/LIBRARY/' in p or '/SCRIPTS/FREESTORE/' in p) and p.endswith('.LUA'):
            with open(os.path.join(lib, os.path.basename(info.filename)), 'wb') as f:
                f.write(z.read(info))
    return lib


STUBS = r'''
Script = "A2fb_Test"
A2_T = 0
A2_OUT = io.open(A2_OUTFILE, "w")
function GetCurrentTime() return A2_T end
A2_TV = {}
local function tv()
	local co = A2_CUR or "main"
	if not A2_TV[co] then A2_TV[co] = {} end
	return A2_TV[co]
end
ThreadValue = setmetatable({ Set = function(k, v) tv()[k] = v end, Reset = function() end },
	{ __call = function(_, k) return tv()[k] end })
GetEvent = setmetatable({ Reset = function() end, Params = function() return nil end },
	{ __call = function() return nil end })
function _ScriptMessage(s) end
function _OuputDebug(s) end
function _MessagePopup(s) end
function _CustomScriptMessage(f, t) A2_OUT:write("CANARYCALL " .. f .. "\n") end
function Game_Message(s) A2_OUT:write("GAME_MESSAGE " .. s .. "\n") end
local A2_DBG = debug
function DumpCallStack() return A2_DBG.traceback() end
function ScriptExit() error("ScriptExit") end

-- model engine: the budget is not initialised until A2_INIT_T seconds (pools 0, BudgetFractionToBuild 1e18), then
-- pools = credits x base-template weights with a category rate of 10 x weight.
A2_CREDITS = 13000
A2_W = { OFF = 0.4, DEF = 0, INFO = 0.1, INFRA = 0.3, MAJOR = 0.2 }
A2_SHIPPED = {
	BasicEmpireOffensiveBudgetAllocation = 8, BasicEmpireDefensiveBudgetAllocation = 3,
	BasicEmpireInformationBudgetAllocation = 2, BasicEmpireInfrastructureBudgetAllocation = 6,
	BasicEmpireMajorItemBudgetAllocation = 4,
	EmpireTechUpgradeOffensiveAllocation = 0.01, EmpireTechUpgradeDefensiveAllocation = 0,
	EmpireTechUpgradeInformationAllocation = 0, EmpireTechUpgradeInfrastructureAllocation = 0.2,
	EmpireTechUpgradeMajorItemAllocation = 0.9, TechUpgradeBudgetRequirement = 0.9,
	Empire_Tech_Trigger = 0, Empire_Tech_Untrigger = 1,
	BasicRebelOffensiveBudgetAllocation = 7, BasicRebelDefensiveBudgetAllocation = 1,
	BasicRebelInformationBudgetAllocation = 1, BasicRebelInfrastructureBudgetAllocation = 5,
	Underworld_Offensive_Budget_Allocation = 9, Underworld_Infrastructure_Budget_Allocation = 4,
	-- a2h t02 (MJ)
	Needs_More_Space_Force_Globally = 1, Need_To_Flush_MajorItem_Budget = 1, Can_Reclaim_Excess_MajorItem_Budget = 0,
	Has_Enough_Space_Force_Globally = 0, Friendly_Global_Space_Unit_Raw_Total = 52600,
	Enemy_Global_Space_Unit_Raw_Total = 61000, A2FB_HumanSpaceTotal = 70000, A2FB_SelfSpaceTotal = 52600,
	A2FB_ActiveGoals = 5, A2FB_ActiveGoalsMajor = 0, A2FB_HasDeathStar = 0, A2FB_Maintenance = 0.12,
	A2FB_PlanetsControlled = 0.3,
}
A2_PLAYERS = {}
local function mkplayer(n)
	return { name = n,
		Get_Faction_Name = function() return n end,
		Get_ID = function() return string.len(n) end,
		Get_Name = function() return n end,
		Is_Human = function() return n == "Rebel" and A2_HAS_HUMAN end,
		Get_Credits = function() return A2_CREDITS end,
		Get_Tech_Level = function() return 3 end,
		Get_Difficulty = function() return 1 end }
end
for _, nm in ipairs({ "Empire", "Rebel", "Underworld", "Pirates" }) do
	A2_PLAYERS[nm] = mkplayer(nm)
end
function Find_Player(n) return A2_PLAYERS[n] end
-- retail: a failed evaluation calls the script error helper, which only sets the script's failed flag (no Lua error,
-- so pcall sees nothing) and the binding returns no value; the script is not serviced again (the drivers stop).
A2_FAILED = false
function A2_Fail(name, why)
	if not A2_FAILED then
		A2_FAILED = name
		A2_OUT:write("SCRIPT_FAILED " .. name .. " (" .. why .. ") t=" .. GetCurrentTime() .. "\n")
	end
end
-- a2j t02: planets with an owner (FindPlanet) and the D4 target evaluations. A target evaluation before the AI Init
-- gives nil with a warning (no failure, as retail's "AI not yet initialized"); a Variable_Human equation with no human
-- player fails (sets the flag); parameter 3 must be a game object.
A2_OWNER = { Kashyyyk = "Empire", Felucia = "Underworld", Tatooine = "Empire" }
A2_HAS_HUMAN = true
function FindPlanet(n)
	if A2_OWNER[n] == nil then return nil end
	return { planet = n, Get_Owner = function() return A2_PLAYERS[A2_OWNER[n]] end,
		Get_Type = function() return { Get_Name = function() return string.upper(n) end } end }
end
A2_PERC_V = { Should_Conquer_Pirate_Planet = -0.306667, Is_Good_Invasion_Target = -0.306667, Opens_New_Front = 1,
	Is_AI_And_Should_Be_Ignored = 0, GenericPlanetValue = 0.586667, Is_Neglected_By_My_Opponent_Space = 0,
	System_Recon_Relevance = 1, Is_Connected_To_Me = 1, Is_Connected_To_Enemy = 1, A2FB_D4_Markup = 0,
	A2FB_D4_ConnIso = 1, A2FB_D4_TradeRoutes = 1, A2FB_D4_Chokepoint = 0, A2FB_D4_PriorityTarget = 0,
	A2FB_D4_HasGroundForce = 1, A2FB_D4_HasSpaceForce = 1, A2FB_D4_IsHuman = 0, A2FB_D4_ConnCorruption = 1,
	A2FB_D4_Corrupted = 0, A2FB_D4_AnyThreats = 1 }
function A2_Target(name, player, target)
	if type(target) ~= "table" or target.planet == nil then A2_Fail(name, "parameter 3 is not a game object") return end
	if A2_NEEDS_HUMAN[name] and not A2_HAS_HUMAN then A2_Fail(name, "Variable_Human with no human player") return end
	local t = GetCurrentTime()
	if t < A2_INIT_T then return nil end
	-- the Underworld's markup on Kashyyyk from 8 s (Distant_System_Path_Bonus in the port, A2I_T03 section 5)
	local mk = 0
	if player.name == "Underworld" and target.planet == "Kashyyyk" and t >= 8 then mk = 0.6 end
	if name == "A2FB_D4_Markup" then return mk end
	if name == "Should_Conquer_Pirate_Planet" or name == "Is_Good_Invasion_Target" then return A2_PERC_V[name] + mk end
	if A2_PERC_V[name] ~= nil then return A2_PERC_V[name] end
	return 0
end
GlobalValue = { Set = function() end, Get = function() return nil end }
function EvaluatePerception(name, player, target)
	if A2_FAIL_EQ[name] then A2_Fail(name, "evaluation fails in retail") return end
	if not A2_KNOWN_EQ[name] then A2_Fail(name, "unknown equation") return end
	if target ~= nil or A2_PERC_V[name] ~= nil then return A2_Target(name, player, target) end
	local t = GetCurrentTime()
	local pre = t < A2_INIT_T
	if name == "A2FB_CreditsUnnorm" then
		if pre then return 0 end
		return A2_CREDITS
	end
	if name == "A2FB_NetIncome" then return 667.5 end
	if A2_SHIPPED[name] ~= nil then return A2_SHIPPED[name] end
	local s, e, f3, k, cat = string.find(name, "^A2FB_(%u+)_BFB_(%d)_(%u+)$")
	if s == nil then return 0 end
	k = tonumber(k)
	local tp = A2_TYPES[f3][k]
	local avail, rate
	if cat == "ALL" then
		-- all categories: D = build time x income per second (+0x68) + the player's credits (+0x70)
		if pre then return tp[1] / A2_CREDITS end
		avail, rate = A2_CREDITS, 20
	else
		if pre then return 1e18 end
		avail, rate = A2_CREDITS * A2_W[cat], 10 * A2_W[cat]
		if avail <= 0 then return 1e18 end
	end
	return tp[1] / (tp[2] * rate + avail)
end
'''

STORY_DRIVER = r'''
Base_Definitions()
local main_co = coroutine.create(main)
local last = -100
local services = 0
for frame = 0, (A2_FRAMES or 55000) do
	A2_T = frame / 30
	if A2_FAILED then break end
	if (A2_T - last) > ServiceRate then
		last = A2_T
		services = services + 1
		A2_CUR = main_co
		local ok, err = coroutine.resume(main_co)
		if not ok then A2_OUT:write("MAIN_ERROR " .. tostring(err) .. "\n") break end
	end
end
A2_OUT:write("SERVICES " .. services .. "\n")
A2_OUT:write("STATE " .. tostring(CurrentState) .. "\n")
for i = 1, A2FB_Seq do A2_OUT:write(A2FB_Records[i] .. "\n") end
A2_OUT:close()
'''


def types_lua(types):
    fm = {'EMP': ('E_Ground_Barracks', 'E_Ground_Research_Facility'), 'REB': ('R_Ground_Barracks', 'R_Ground_Research_Facility'),
          'UWD': ('U_Ground_Barracks', 'U_Ground_Vehicle_Factory')}
    parts = []
    for f3, (t1, t2) in fm.items():
        parts.append('%s = { { %s, %s }, { %s, %s } }' % (f3, types[t1]['cost'], types[t1]['time'],
                                                          types[t2]['cost'], types[t2]['time']))
    return 'A2_TYPES = { %s }\n' % ', '.join(parts)


def story_test(lua, lib, out, types):
    ok_all = True
    env = dict(os.environ, LUA_PATH=os.path.join(lib, '?.lua'))
    outfile = os.path.join(out, 'story_out.txt')
    script = os.path.join(out, 'story_test.lua')
    with open(script, 'w', encoding='ascii') as f:
        f.write(prelude(outfile, types))
        f.write(build_a2f_mod.story_lua())
        f.write(STORY_DRIVER)
    rc, text = lua_run(lua, script, env)
    out_text = open(outfile).read() if os.path.exists(outfile) else ''
    check('story: lua exit 0, no MAIN_ERROR', rc == 0 and 'MAIN_ERROR' not in out_text, (text + out_text)[:600])
    fl = [ln for ln in out_text.splitlines() if ln.startswith('SCRIPT_FAILED')]
    check('story: no evaluation that fails in retail (script-failed flag never set)', not fl, str(fl))
    check('story: state machine ran A2FB_State', 'STATE A2FB_State' in out_text)
    check('story: canary call + game messages', 'CANARYCALL Mods\\A2fBudgetProbe\\A2FB_Canary_Story.txt' in out_text
          and 'GAME_MESSAGE TEXT_A2FB_LOADED' in out_text and 'GAME_MESSAGE TEXT_A2FB_DONE' in out_text
          and 'GAME_MESSAGE TEXT_A2FB_LONG_DONE' in out_text and 'GAME_MESSAGE TEXT_A2FB_MIN30' in out_text)
    records = os.path.join(out, 'records.txt')
    with open(records, 'w') as f:
        for ln in out_text.splitlines():
            if ln.startswith('A2FB1|'):
                f.write(ln + '\n')
    recs = a2f_report.parse_records(records)
    kinds = {}
    for r in recs:
        kinds[r['kind']] = kinds.get(r['kind'], 0) + 1
    check('story: records of every kind', all(k in kinds for k in ('LOAD', 'DEF', 'CANARY', 'ENTER', 'FIRST', 'SM', 'BF', 'SH', 'PL', 'DONE',
                                                                    'MJ', 'LONGDONE', 'MIN30')),
          str(kinds))
    check('story: record length <= 400 bytes incl. needle', all(len(r['text']) + 60 <= 400 for r in recs))
    sm = [r for r in recs if r['kind'] == 'SM' and r['f'].get('fac') == 'EMPIRE']
    frames = [int(float(r['f']['f'])) for r in sm]
    check('story: Empire sampled on every frame at the start', frames[:6] == [0, 1, 2, 3, 4, 5], str(frames[:8]))
    check('story: Rebel (human) has no SM records', not [r for r in recs if r['kind'] == 'SM' and r['f'].get('fac') == 'REBEL'])
    win = [f for f in frames if 1330 <= f <= 1430]
    check('story: dense window around the 45 s fiscal tick', len(win) >= 80, str(len(win)))
    check('story: record count within the cap', len(recs) < 20000, str(len(recs)))
    mj = [r for r in recs if r['kind'] == 'MJ']
    check('story: MJ with every Empire sample, no other faction', len(mj) == len(sm) and
          all(r['f'].get('fac') == 'EMPIRE' for r in mj), '%d MJ / %d Empire SM' % (len(mj), len(sm)))
    check('story: MJ carries the shipped and mod keys', bool(mj) and all(
        (' %s=' % k) in mj[0]['text'] for k in a2fb_spec.MAJOR_KEYS) and 'NTF=1' in mj[0]['text'] and
        'AGM=0' in mj[0]['text'], mj[0]['text'] if mj else '')
    late = sorted({int(float(r['f']['f'])) for r in recs if r['kind'] == 'SH' and r['f'].get('fac') == 'EMPIRE' and
                   float(r['f']['f']) > 9100})
    check('story: a full sample every 30 s after DONE up to the 25-minute mark', len(late) >= 40 and
          all(899 <= b - a <= 902 for a, b in zip(late, late[1:])), str(late[:5]))
    shutil.copy(os.path.join(out, 'A2FB_TYPES.json'), os.path.join(out, 'types_copy.json'))
    res, text = a2f_report.run(records, None, os.path.join(out, 'A2FB_TYPES.json'), out)
    emp = res['factions'].get('EMPIRE', {})
    check('report: Empire decoded, mod equations answer', emp.get('modeq_working'))
    check('report: pre-Init then real pools (frame 15)', emp.get('first_preinit_f') == 0 and emp.get('last_preinit_f') == 14
          and emp.get('first_postinit_f') == 15, str((emp.get('first_preinit_f'), emp.get('last_preinit_f'),
                                                       emp.get('first_postinit_f'))))
    post = [r for r in emp.get('samples', []) if r['pools'].get('INFRA', {}).get('state') == 'ok']
    p0 = post[0]['pools'] if post else {}
    pre0 = [r for r in emp.get('samples', []) if r['f'] == 0.0]
    pp = pre0[0]['pools'] if pre0 else {}
    check('report: pre-Init ALL row = credits (13000, rate 0), categories empty',
          bool(pp) and abs(pp['ALL']['avail'] - 13000) < 1 and abs(pp['ALL']['rate']) < 0.01 and
          pp['OFF']['state'] == 'empty' and pp['INFRA']['state'] == 'empty', str(pp)[:300])
    exact = bool(p0) and abs(p0['ALL']['avail'] - 13000) < 1 and abs(p0['INFRA']['avail'] - 3900) < 1 and \
        abs(p0['INFRA']['rate'] - 3.0) < 0.01 and abs(p0['OFF']['avail'] - 5200) < 1 and abs(p0['ALL']['rate'] - 20) < 0.01
    check('report: pools solved (ALL = credits 13000, INFRA 3900 @ 3/s, OFF 5200)', exact, str(p0)[:300])
    t = emp.get('template_at_first_post')
    check('report: template = base (L1 ~ 0), not tech', bool(t) and t['l1'].get('base', 9) < 1e-4 and t['l1'].get('tech', 0) > 0.1,
          str(t)[:300])
    check('report: Underworld decoded too', bool(res['factions'].get('UNDERWORLD', {}).get('samples')))
    txt = open(os.path.join(out, 'report.txt')).read()
    check('report: report.txt written with sections 1-5', all(s in txt for s in ('== 1.', '== 2.', '== 3.', '== 4.', '== 5.')))
    check('report: MajorItem section counts NTF and AGM, LONGDONE seen',
          'NTF  > 0 in %d of %d samples' % (len(mj), len(mj)) in txt and 'AGM  > 0 in 0 of' in txt and
          '25-minute mark (LONGDONE) reached' in txt, txt[txt.find('== 5.'):][:900])
    check('report: 30-minute mark (MIN30) reached (frame 54000 .. 54900: the 30 s service after 1800 s)',
          re.search(r'30-minute mark \(MIN30, a2j t04\): reached at frame 54\d\d\d', txt) is not None,
          txt[txt.find('30-minute'):][:200])
    mt = open(os.path.join(out, 'major.tsv')).read().splitlines()
    check('report: major.tsv has one row per MJ sample', len(mt) == len(mj) + 1 and 'AGM' in mt[0], str(len(mt)))
    return ok_all


# Fault injection (robust test): the Underworld's first small-sample entry gets a nil key, so building the text of its SM
# record raises a Lua error ("attempt to concatenate"). Lua 5.0 table.getn honours an explicit n field.
ROBUST_FAULT = r'''
A2FB_SPEC["UNDERWORLD"].small[1] = { nil, "A2FB_CreditsUnnorm" }
'''


def robust_test(lua, lib, out, types):
    env = dict(os.environ, LUA_PATH=os.path.join(lib, '?.lua'))
    outfile = os.path.join(out, 'robust_out.txt')
    script = os.path.join(out, 'robust_test.lua')
    with open(script, 'w', encoding='ascii') as f:
        f.write(prelude(outfile, types))
        f.write('A2_FRAMES = 300\n')
        f.write(build_a2f_mod.story_lua())
        f.write(ROBUST_FAULT)
        f.write(STORY_DRIVER)
    rc, text = lua_run(lua, script, env)
    o = open(outfile).read() if os.path.exists(outfile) else ''
    recs = [ln.split('|') for ln in o.splitlines() if ln.startswith('A2FB1|')]
    err = [r for r in recs if r[4] == 'ERR']
    check('robust: a failing record gives one ERR record (once per message)', rc == 0 and len(err) == 1 and
          err[0][5].startswith('SM ') and 'concatenate' in err[0][5], str([r[5] for r in err])[:400] + (text + o)[:300])

    def has(kind, fac):
        return any(r[4] == kind and ('fac=%s ' % fac) in r[5] for r in recs)
    check('robust: the rest of that sample (Underworld BF / SH / PL) is still recorded',
          has('BF', 'UNDERWORLD') and has('SH', 'UNDERWORLD') and has('PL', 'UNDERWORLD'))
    check('robust: the other players are still sampled (Empire SM / MJ, Pirates PL)',
          has('SM', 'EMPIRE') and has('MJ', 'EMPIRE') and has('PL', 'PIRATES'))
    uw = [r for r in recs if r[4] in ('BF', 'SH') and 'fac=UNDERWORLD ' in r[5]]
    check('robust: sampling goes on after the error (Underworld full samples at 0 .. 9 s)', len(uw) >= 18, str(len(uw)))


HOOK_FS = r'''
A2_OUT_CALLS = 0
function FreeStoreService() A2_OUT_CALLS = A2_OUT_CALLS + 1 end
'''

FS_DRIVER = r'''
PlayerObject = A2_PLAYERS["Empire"]
A2_T = 10
FreeStoreService()
A2_T = 18
if not A2_FAILED then FreeStoreService() end
A2_OUT:write("ORIG_CALLS " .. A2_OUT_CALLS .. "\n")
for i = 1, A2FB_Seq do A2_OUT:write(A2FB_Records[i] .. "\n") end
A2_OUT:close()
'''

PLAN_PRE = r'''
A2_T = 12.5
PlayerObject = A2_PLAYERS["Empire"]
Budget = { Get_Spendable_Resources = function() return 321 end, Get_Unallocated_Resources = function() return 123.5 end,
	Flush_Category = function(c) A2_FLUSHED = c end }
'''
PLAN_DRIVER = r'''
Sleep = function() end
AssembleForce = function() A2_RAN = true end
SynchronizedAssemble = AssembleForce
BaseForce = { Set_Plan_Result = function() end, Set_As_Goal_System_Removable = function() end,
	Produce_Force = function() return nil end, Are_All_Units_On_Free_Store = function() return true end,
	Move_To = function() return nil end, Get_Force_Count = function() return 1 end, Release_Forces = function() end }
StructureForce = BaseForce
TechForce = BaseForce
MainForce = BaseForce
Target = {}
Invade = function() return true end
FundBases = function() end
BlockOnCommand = function() end
Purge_Goals = function() end
-- the real ScriptExit (PGBase, wrapped by the hook) runs; outside a thread it does not yield
_ScriptExit = function() A2_EXIT = true end
GetThreadID = function() return -1 end
A2_TYPE = { Get_Name = function() return "E_GROUND_HEAVY_VEHICLE_FACTORY" end }
%s()
A2_T = 14
if %s_Production_Failed ~= nil then %s_Production_Failed(%s, A2_TYPE) end
A2_OUT:write("PLAN_RAN " .. tostring(A2_RAN) .. " EXIT " .. tostring(A2_EXIT) .. " FLUSHED " .. tostring(A2_FLUSHED) .. "\n")
for i = 1, A2FB_Seq do A2_OUT:write(A2FB_Records[i] .. "\n") end
A2_OUT:close()
'''


def hook_tests(lua, lib, out, types, z):
    env = dict(os.environ, LUA_PATH=os.path.join(lib, '?.lua'))
    # free store: wrapper around a stand-in for the shipped function
    outfile = os.path.join(out, 'fs_out.txt')
    script = os.path.join(out, 'fs_test.lua')
    body = build_a2f_mod.assemble(HOOK_FS, 'F', 'a2fb_freestore_hook.lua', True)
    with open(script, 'w', encoding='ascii') as f:
        f.write(prelude(outfile, types))
        f.write(body)
        f.write(FS_DRIVER)
    rc, text = lua_run(lua, script, env)
    o = open(outfile).read() if os.path.exists(outfile) else ''
    check('hooks: free store wrapper calls the shipped function twice', rc == 0 and 'ORIG_CALLS 2' in o, (text + o)[:400])
    check('hooks: free store samples PlayerObject (SM, BF, SH, PL)', all(('|%s|' % k) in o for k in ('SM', 'BF', 'SH', 'PL')))
    check('hooks: free store canary', 'CANARYCALL Mods\\A2fBudgetProbe\\A2FB_Canary_FreeStore.txt' in o)
    check('hooks: free store: no evaluation that fails in retail', 'SCRIPT_FAILED' not in o,
          str([ln for ln in o.splitlines() if ln.startswith('SCRIPT_FAILED')]))
    # the real shipped plan scripts with the hook
    names = {i.filename.upper(): i for i in z.infolist()}
    for plan, thread in build_a2f_mod.PLANS:
        orig = zread(z, names, 'DATA/SCRIPTS/AI/%s.LUA' % plan.upper())
        body = build_a2f_mod.assemble(orig, 'P', 'a2fb_plan_hook.lua', True, build_a2f_mod.plan_subs(plan, thread))
        outfile = os.path.join(out, 'plan_%s.txt' % plan)
        script = os.path.join(out, 'plan_%s.lua' % plan)
        with open(script, 'w', encoding='latin-1') as f:
            # plan scripts keep debug (PEXIT by= / line=; without it the hook writes by=nodebug)
            f.write(prelude(outfile, types, no_debug=False))
            f.write(body)
            f.write(PLAN_PRE)
            f.write('Definitions()\n')
            force = thread[:-len('_Thread')]
            f.write(PLAN_DRIVER % (thread, force, force, force))
        rc, text = lua_run(lua, script, env)
        o = open(outfile).read() if os.path.exists(outfile) else ''
        check('hooks: plan %s records PSTART and still runs' % plan,
              rc == 0 and 'PLAN_RAN' in o and
              ('plan=%s t=12.5 f=375 fac=EMPIRE budget=table spend=321 unalloc=123.5 cr=13000 n=1 prev=first' % plan) in o,
              (text + o)[:500])
        # a2h t02: end records. The test thread is not a coroutine, so ScriptExit returns and the thread returns too.
        body_thread = re.split(r'\nfunction\s', orig.replace('\r\n', '\n').split('function %s' % thread, 1)[1], maxsplit=1)[0]
        has_exit = re.search(r'\bScriptExit\s*\(', body_thread) is not None
        force = thread[:-len('_Thread')]
        has_fail = re.search(r'function\s+%s_Production_Failed\s*\(' % force, orig) is not None
        ok = ('|PRET|plan=%s t=12.5 f=375 fac=EMPIRE n=1 age=0 spend=321' % plan) in o
        if has_exit:
            m = re.search(r'\|PEXIT\|plan=%s t=12.5 f=375 fac=EMPIRE n=1 age=0 spend=321 unalloc=123.5 cr=13000 '
                          r'by=(\S+) line=(\d+)' % plan, o)
            ok = ok and m is not None and m.group(1) == 'A2FB_Thread_Orig'
        if has_fail:
            ok = ok and ('|PFAIL|plan=%s t=14 f=420 fac=EMPIRE n=1 age=1.5 spend=321 unalloc=123.5 cr=13000 args=2 ERR '
                         'E_GROUND_HEAVY_VEHICLE_FACTORY' % plan) in o
        check('hooks: plan %s end records (PRET%s%s)' % (plan, ', PEXIT' if has_exit else '', ', PFAIL' if has_fail else ''),
              rc == 0 and ok, o[-900:])
        check('hooks: plan %s: no evaluation that fails in retail' % plan, 'SCRIPT_FAILED' not in o,
              str([ln for ln in o.splitlines() if ln.startswith('SCRIPT_FAILED')]))
        if plan in build_a2f_mod.FULL_SAMPLE_PLANS:
            check('hooks: plan %s full samples at start and end, Flush_Category ran' % plan,
                  'FLUSHED MajorItem' in o and '|BF|tag=PS' in o and '|BF|tag=PE' in o and '|MJ|tag=PS' in o, o[-700:])


def build_test(out, zip_path):
    mod = os.path.join(out, 'mod')
    types = build_a2f_mod.build(None, mod, zip_path)
    exp = ['Data/XML/AI/PerceptualEquations/AI_Equations_ExpansionGeneric_Galactic.xml', 'Data/Scripts/Story/A2fb_Story.lua',
           'Data/Scripts/FreeStore/GalacticFreeStore.lua', 'A2FB_TYPES.json', build_a2f_mod.MARKER,
           'Data/Scripts/Miscellaneous/GameScoring.lua', 'A2FB_QUEUE_TYPES.json'] +           ['Data/XML/%s' % p[4] for p in a2fb_spec.PERC_PAIRS] +           ['Data/Scripts/Story/A2fb_Percept%s.lua' % p[3] for p in a2fb_spec.PERC_PAIRS] + \
          ['Data/XML/Story_Plots_Sandbox_12U_%s.xml' % f for f in build_a2f_mod.PLOTS] + \
          ['Data/Scripts/AI/%s.lua' % n for n, _ in build_a2f_mod.PLANS] + list(build_a2f_mod.CANARIES.values())
    missing = [e for e in exp if not os.path.exists(os.path.join(mod, e))]
    check('build: mod folder complete (%d files)' % len(exp), not missing, str(missing))
    shutil.copy(os.path.join(mod, 'A2FB_TYPES.json'), os.path.join(out, 'A2FB_TYPES.json'))
    shutil.copy(os.path.join(mod, 'A2FB_QUEUE_TYPES.json'), os.path.join(out, 'A2FB_QUEUE_TYPES.json'))
    ok = True
    for _f, _p, _pl, suf, plot in a2fb_spec.PERC_PAIRS:
        try:
            root = ET.fromstring(open(os.path.join(mod, 'Data', 'XML', plot), 'rb').read().decode('latin-1').lstrip('﻿ï»¿'))
            ok = ok and root.find('Lua_Script').text == 'A2fb_Percept' + suf and root.find('Active_Plot') is not None
        except (ET.ParseError, AttributeError) as e:
            ok = False
            print('   ', plot, e)
    check('build: AI plot files are XML with Lua_Script A2fb_Percept<U|E> and their shipped Active_Plot', ok)
    qt = json.load(open(os.path.join(mod, 'A2FB_QUEUE_TYPES.json')))
    want = {'EMPIRE_STAR_BASE_2': 'space', 'ACCLAMATOR_ASSAULT_SHIP': 'space', 'E_GROUND_BARRACKS': 'ground',
            'GROUND_MAGNEPULSE_CANNON': 'ground', 'TIE_FIGHTER_SQUADRON': 'space'}
    got = {k: qt.get(k) for k in want}
    check('build: queue types (ground / space) for sample Empire types', got == want, str(got))
    # --no-percept (an unperturbed t04 run): no AI plot files, perception scripts or their canaries; the rest unchanged
    mod2 = os.path.join(out, 'mod_nopercept')
    build_a2f_mod.build(None, mod2, zip_path, percept=False)
    gone = ['Data/XML/%s' % p[4] for p in a2fb_spec.PERC_PAIRS] + \
           ['Data/Scripts/Story/A2fb_Percept%s.lua' % p[3] for p in a2fb_spec.PERC_PAIRS] + \
           [fn for ch, fn in build_a2f_mod.CANARIES.items() if ch.startswith('Percept')]
    kept = [e for e in exp if e not in gone]
    check('build --no-percept: channel D left out, everything else built',
          not [g for g in gone if os.path.exists(os.path.join(mod2, g))] and
          all(os.path.exists(os.path.join(mod2, e)) for e in kept), str(gone))
    return mod, types


def lua_syntax(lua, mod, out):
    bad = []
    for root, _d, files in os.walk(mod):
        for fn in files:
            if fn.lower().endswith('.lua'):
                p = os.path.join(root, fn)
                code = 'local f, e = loadfile([[%s]]) if not f then io.write(e) os.exit(1) end' % p
                sp = os.path.join(out, 'syn.lua')
                open(sp, 'w').write(code)
                r = subprocess.run([lua, sp], capture_output=True, text=True)
                if r.returncode != 0:
                    bad.append('%s: %s' % (fn, (r.stdout + r.stderr)[:200]))
    check('build: every generated Lua file loads under Lua 5.0.2', not bad, str(bad))
    bad = [fn for root, _d, files in os.walk(mod) for fn in files if fn.lower().endswith('.lua') and
           b'A2FB1|' in open(os.path.join(root, fn), 'rb').read()]
    check('build: no Lua source contains the literal record needle', not bad, str(bad))


# ---- watcher ---------------------------------------------------------------------------------------------------------

class FakeMem:
    def __init__(self):
        self.m = {}

    def put(self, addr, data):
        for i, b in enumerate(data):
            self.m[addr + i] = b

    def read(self, addr, n):
        if addr < 0x10000:
            return None
        try:
            return bytes(self.m[addr + i] for i in range(n))
        except KeyError:
            return None

    def u64(self, addr):
        b = self.read(addr, 8)
        return int.from_bytes(b, 'little') if b else None


def watcher_test(out):
    import struct
    mem = FakeMem()
    player, ai, vec, ctx, goal = 0x100000, 0x200000, 0x300000, 0x400000, 0x500000
    mem.put(player + 0x360, struct.pack('<Q', ai))
    mem.put(ai + 0x38, struct.pack('<QQ', vec, vec + 8))
    mem.put(vec, struct.pack('<Q', ctx))
    ctxb = bytearray(0x70)
    struct.pack_into('<i', ctxb, 0x30, 1)
    struct.pack_into('<i', ctxb, 0x34, 0)
    struct.pack_into('<Q', ctxb, 0x40, goal)
    ctxb[0x69] = 0
    mem.put(ctx, bytes(ctxb))
    budget = goal + 0x190

    def set_budget(inc, per, flag, refund):
        b = bytearray(0x90)
        struct.pack_into('<f', b, 0x68, inc)
        struct.pack_into('<f', b, 0x70, per)
        struct.pack_into('<I', b, 0x74, flag)
        struct.pack_into('<f', b, 0x80, refund)
        mem.put(budget, bytes(b))

    set_budget(0, 0, 0, 0)

    class FakeGame:
        frame = 0

        def players(self):
            return {'by_id': [{'slot': 0, 'addr': player, 'faction': 'EMPIRE', 'human': 0, 'neutral': 0},
                    {'slot': 1, 'addr': 0x600000, 'faction': 'REBEL', 'human': 1, 'neutral': 0}], 'count': 2, 'vec': []}

        def mode(self):
            return {'frame': FakeGame.frame, 'gframe': FakeGame.frame, 'type': 0}

    clock = [0.0]
    path = os.path.join(out, 'watch.jsonl')
    with open(path, 'w') as f:
        w = a2f_watch.Watcher(mem, FakeGame(), f, clock=lambda: clock[0])
        w.resolve()
        script = [(0, lambda: None), (3, lambda: mem.put(ctx + 0x69, b'\x01')),
                  (4, lambda: set_budget(0, 45.0, 1, 0)), (6, lambda: set_budget(14.8, 45.0, 0, 0)),
                  (9, lambda: set_budget(14.8, 45.0, 0, 12.5))]
        step = 0
        for fr in range(0, 12):
            FakeGame.frame = fr
            clock[0] = fr * 0.033
            while step < len(script) and script[step][0] <= fr:
                script[step][1]()
                step += 1
            w.poll()
        clock[0] += 6
        w.poll()
        # a plausible-looking but broken chain is reported, not followed
        mem.put(player + 0x360, struct.pack('<Q', 0x1234))
        w.resolve()
    ev = a2f_watch.load_events(path)
    kinds = [e['ev'] for e in ev]
    check('watcher: found the budget at goal + 0x190', any(e['ev'] == 'found' and e['budget'] == hex(budget) for e in ev))
    check('watcher: human player skipped', all(e.get('fac') != 'REBEL' for e in ev))
    flags = [(e['mf'], e['from'], e['to']) for e in ev if e['ev'] == 'flag']
    check('watcher: reset flag 0 -> 1 at frame 4 and 1 -> 0 at frame 6', flags == [(4, 0, 1), (6, 1, 0)], str(flags))
    st = [e for e in ev if e['ev'] == 'state']
    check('watcher: init byte change logged', any(e['init'] == 1 and e['mf'] == 3 for e in st))
    check('watcher: income / T / refund decoded', any(abs(e['income_per_s'] - 14.8) < 1e-3 and e['T'] == 45.0 and
                                                       e['refund'] == 12.5 for e in st))
    check('watcher: heartbeat after 5 s', st[-1]['mf'] == 11 and kinds.count('state') >= 6)
    check('watcher: bad AI pointer reported as a chain line', any(e['ev'] == 'chain' for e in ev))
    # the report picks the watcher data up
    recs = os.path.join(out, 'empty_records.txt')
    open(recs, 'w').write('')
    res, text = a2f_report.run(recs, path, None, os.path.join(out, 'wrep'))
    check('report: watcher section lists the flag transitions', '4: 0 -> 1' in text and '6: 1 -> 0' in text, text[-600:])


def cleanup_test(out):
    game = os.path.join(out, 'fakegame')
    shutil.rmtree(game, ignore_errors=True)
    shutil.rmtree(os.path.join(out, 'rundone'), ignore_errors=True)
    mod = os.path.join(game, 'corruption', 'Mods', a2f_cleanup.MOD_NAME)
    other = os.path.join(game, 'corruption', 'Mods', 'Other')
    os.makedirs(mod)
    os.makedirs(other)
    open(os.path.join(other, 'x.txt'), 'w').write('x')
    r = a2f_cleanup.cleanup(game)
    check('cleanup: unmarked folder is left alone', os.path.isdir(mod) and r['ok'])
    open(os.path.join(mod, a2f_cleanup.MARKER), 'w').write('m')
    r = a2f_cleanup.cleanup(game, dry_run=True)
    check('cleanup: dry run deletes nothing', os.path.isdir(mod))
    r = a2f_cleanup.cleanup(game)
    check('cleanup: marked folder removed, others kept', not os.path.exists(mod) and os.path.isdir(other) and r['ok'])
    r = a2f_cleanup.cleanup(game)
    check('cleanup: second run is a no-op', r['ok'])
    ok, msg = a2f_cleanup.restore_profile(os.path.join(out, 'nonexistent'))
    check('cleanup: profile restore without a snapshot is a no-op', ok, msg)
    rd = os.path.join(out, 'rundone')
    os.makedirs(os.path.join(rd, 'profile_backup'))
    open(os.path.join(rd, 'profile_backup', 'profile.json'), 'w').write('{}')
    open(os.path.join(rd, a2f_cleanup.DONE), 'w').write('done')
    ok, msg = a2f_cleanup.restore_profile(rd)
    check('cleanup: an already restored snapshot is not applied again', ok and 'already restored' in msg, msg)


# ---- a2j t02: D4 perception plot scripts (channel D) and the GameScoring production hook (channel Q) -----------------

PERC_DRIVER = r'''
Base_Definitions()
local main_co = coroutine.create(main)
local last = -100
local services = 0
for frame = 0, (A2_FRAMES or 3600) do
	A2_T = frame / 30
	if A2_FAILED then break end
	if (A2_T - last) > ServiceRate then
		last = A2_T
		services = services + 1
		A2_CUR = main_co
		local ok, err = coroutine.resume(main_co)
		if not ok then A2_OUT:write("MAIN_ERROR " .. tostring(err) .. "\n") break end
	end
end
A2_OUT:write("SERVICES " .. services .. "\n")
A2_OUT:write("STATE " .. tostring(CurrentState) .. "\n")
for i = 1, A2FB_Seq do A2_OUT:write(A2FB_Records[i] .. "\n") end
A2_OUT:close()
'''


def percept_run(lua, lib, out, types, fkey, name, inject=''):
    env = dict(os.environ, LUA_PATH=os.path.join(lib, '?.lua'))
    outfile = os.path.join(out, '%s_out.txt' % name)
    script = os.path.join(out, '%s.lua' % name)
    with open(script, 'w', encoding='ascii') as f:
        f.write(prelude(outfile, types))
        f.write(inject)
        f.write(build_a2f_mod.percept_lua(fkey))
        f.write(PERC_DRIVER)
    rc, text = lua_run(lua, script, env)
    o = open(outfile).read() if os.path.exists(outfile) else ''
    recs_path = os.path.join(out, '%s_records.txt' % name)
    with open(recs_path, 'w') as f:
        for ln in o.splitlines():
            if ln.startswith('A2FB1|'):
                f.write(ln + '\n')
    return rc, text, o, a2f_report.parse_records(recs_path)


def percept_tests(lua, lib, out, types):
    want = sorted(set(list(range(0, 601, 30)) + list(range(900, 3601, 300))))
    for fkey, _pn, planet, suf, _plot in a2fb_spec.PERC_PAIRS:
        rc, text, o, recs = percept_run(lua, lib, out, types, fkey, 'percept_%s' % suf)
        check('percept %s: lua exit 0, no MAIN_ERROR, state machine ran' % suf, rc == 0 and 'MAIN_ERROR' not in o and
              'STATE A2FB_DState' in o, (text + o)[:500])
        check('percept %s: no evaluation that fails in retail' % suf, 'SCRIPT_FAILED' not in o,
              str([ln for ln in o.splitlines() if ln.startswith('SCRIPT_FAILED')]))
        check('percept %s: canary of its own' % suf, ('CANARYCALL Mods\\A2fBudgetProbe\\A2FB_Canary_Percept%s.txt' % suf) in o)
        ds = [r for r in recs if r['kind'] == 'DS']
        dt = [r for r in recs if r['kind'] == 'DT']
        frames = [int(r['f']['f']) for r in ds]
        check('percept %s: DS / DT every 30 frames to 600, then every 300 (frames %s ..)' % (suf, frames[:3]),
              frames == want and [int(r['f']['f']) for r in dt] == want, str(frames[:30]))
        check('percept %s: every key in DT / DS, record length <= 400' % suf, bool(ds) and all(
            (' %s=' % k) in (dt[1]['text'] + ds[1]['text']) for k in a2fb_spec.PERC_KEYS) and
            all(len(r['text']) + 60 <= 400 for r in recs), (ds[1]['text'] if len(ds) > 1 else ''))
        check('percept %s: head fac / tgt / own' % suf, bool(ds) and ds[1]['f'].get('fac') == fkey and
              ds[1]['f'].get('tgt') == planet and ds[1]['f'].get('own') in ('EMPIRE', 'UNDERWORLD'), ds[1]['text'] if ds else '')
        check('percept %s: before the AI Init the values are nil and sampling goes on' % suf, bool(ds) and
              'SCP=nil' in ds[0]['text'] and 'SCP=-0.306667' in ds[1]['text'], ds[0]['text'] if ds else '')
    # no human player: the needs-human equations are skipped, not evaluated (they would set the failed flag)
    rc, text, o, recs = percept_run(lua, lib, out, types, 'UNDERWORLD', 'percept_nohuman', 'A2_HAS_HUMAN = false\n')
    ds = [r for r in recs if r['kind'] == 'DS']
    check('percept: without a human, SCP / IGI / IGN = skip and the script keeps running', 'SCRIPT_FAILED' not in o and
          len(ds) > 25 and 'SCP=skip' in ds[3]['text'] and 'IGI=skip' in ds[3]['text'] and 'ONF=1' in ds[3]['text'],
          (o[-400:] if not ds else ds[3]['text']))
    # isolation: a token that fails in retail stops only that pair's script, after a complete record naming it
    rc, text, o, recs = percept_run(lua, lib, out, types, 'UNDERWORLD', 'percept_fail', 'A2_FAIL_EQ["A2FB_D4_Markup"] = true\n')
    dt = [r for r in recs if r['kind'] == 'DT']
    check('percept: a failing token -> failed flag, one complete record with MK=nil, then the script stops',
          'SCRIPT_FAILED A2FB_D4_Markup' in o and len(dt) == 1 and 'MK=nil' in dt[0]['text'] and
          'ACT=' in dt[0]['text'], o[:600])
    res, txt = a2f_report.run(os.path.join(out, 'percept_fail_records.txt'), None, None, os.path.join(out, 'pfrep'),
                              port_perception=None)
    return txt


SCORING_DRIVER = r'''
frag_index = 1
death_index = 2
Reset_Stats()
local tat = FindPlanet("Tatooine")
local ty = { Get_Name = function() return "GROUND_MAGNEPULSE_CANNON" end, Get_Build_Cost = function() return 2000 end,
	Get_Combat_Rating = function() return 5 end, Get_Score_Cost_Credits = function() return 100 end }
local sq = { Get_Name = function() return "TIE_FIGHTER_SQUADRON" end, Get_Build_Cost = function() return 300 end,
	Get_Combat_Rating = function() return 1 end, Get_Score_Cost_Credits = function() return 10 end }
A2_T = 103 / 30
A2_CREDITS = 8000
Galactic_Production_Begin_Event(tat, ty)
A2_CREDITS = 13000
A2_T = 110 / 30
Galactic_Production_Begin_Event(tat, sq)
A2_T = 140 / 30
Galactic_Production_Begin_Event(tat, sq)
A2_T = 200 / 30
Galactic_Production_Canceled_Event(tat, sq)
A2_T = 300 / 30
Galactic_Production_End_Event(tat, sq)
A2_T = 1543 / 30
local obj = { Get_Type = function() return ty end, Get_Game_Scoring_Type = function() return ty end }
Galactic_Production_End_Event(tat, obj)
local n = 0
for _, v in pairs(GalacticBuildStatsTable) do n = n + 1 end
A2_OUT:write("SHIPPED_STATS " .. n .. "\n")
for i = 1, A2FB_Seq do A2_OUT:write(A2FB_Records[i] .. "\n") end
A2_OUT:close()
'''


def scoring_test(lua, lib, out, types, z):
    names = {i.filename.upper(): i for i in z.infolist()}
    orig = zread(z, names, build_a2f_mod.SCORING.replace('\\', '/'))
    body = build_a2f_mod.assemble(orig, 'Q', 'a2fb_scoring_hook.lua', True)
    env = dict(os.environ, LUA_PATH=os.path.join(lib, '?.lua'))
    outfile = os.path.join(out, 'scoring_out.txt')
    script = os.path.join(out, 'scoring_test.lua')
    with open(script, 'w', encoding='latin-1') as f:
        f.write(prelude(outfile, types))
        f.write(body)
        f.write(SCORING_DRIVER)
    rc, text = lua_run(lua, script, env)
    o = open(outfile).read() if os.path.exists(outfile) else ''
    check('scoring: GameScoring + hook loads and runs (lua exit 0)', rc == 0 and 'SHIPPED_STATS' in o, (text + o)[:600])
    check('scoring: the shipped End handler still ran (its build stats table has an entry)', 'SHIPPED_STATS 1' in o, o[:300])
    check('scoring: no evaluation that fails in retail', 'SCRIPT_FAILED' not in o)
    check('scoring: QB / QC / QE records with frame, owner, planet, type, cost, the owner\'s credits',
          '|QB|t=3.43333 f=103 fac=EMPIRE pl=TATOOINE type=GROUND_MAGNEPULSE_CANNON cost=2000 cr=8000|' in o and
          '|QC|t=6.66667 f=200 fac=EMPIRE pl=TATOOINE type=TIE_FIGHTER_SQUADRON cost=300 cr=13000|' in o and
          '|QE|t=51.4333 f=1543 fac=EMPIRE pl=TATOOINE type=GROUND_MAGNEPULSE_CANNON cost=- cr=13000|' in o and
          'CANARYCALL Mods\\A2fBudgetProbe\\A2FB_Canary_Scoring.txt' in o, o[:900])
    recs = os.path.join(out, 'scoring_records.txt')
    with open(recs, 'w') as f:
        for ln in o.splitlines():
            if ln.startswith('A2FB1|'):
                f.write(ln + '\n')
    return recs


def a2j_report_test(out, perc_records, scoring_records):
    """The report on the D / Q records with a port reference table and a synthetic port trace (sections 6 - 8)."""
    rd = os.path.join(out, 'a2jrep')
    os.makedirs(rd, exist_ok=True)
    recs = os.path.join(rd, 'records.txt')
    with open(recs, 'w') as f:
        for p in (os.path.join(out, 'records.txt'), perc_records, scoring_records):
            f.write(open(p).read())
    shutil.copy(os.path.join(out, 'A2FB_QUEUE_TYPES.json'), os.path.join(rd, 'A2FB_QUEUE_TYPES.json'))
    ref = os.path.join(rd, 'port_ref.tsv')
    with open(ref, 'w') as f:
        f.write('# port reference, selftest\n' + '\t'.join(['fac', 'tgt', 'frame', 'own'] + a2fb_spec.PERC_KEYS) + '\n')
        for fr in range(30, 601, 30):
            v = {k: '0' for k in a2fb_spec.PERC_KEYS}
            v.update({'SCP': '0.311111' if fr >= 270 else '-0.306667', 'MK': '0.617778' if fr >= 270 else '0'})
            f.write('\t'.join(['UNDERWORLD', 'Kashyyyk', str(fr), 'EMPIRE'] + [v[k] for k in a2fb_spec.PERC_KEYS]) + '\n')
    trace = os.path.join(rd, 'port.err')
    with open(trace, 'w') as f:
        for fr in range(1, 2800, 30):
            f.write('A2G BUD frame f=%d p=1 cr=5000.0 gain=0.0 inc68=13.0 T=45.00 sumP=5000.0 | Offensive A=1.0 R=0.1 '
                    'P=1111.5 w=0.3 n=0 I=2.5; Infrastructure A=2.0 R=0.2 P=2222.5 w=0.5 n=1 I=5.5; MajorItem A=0.0 '
                    'R=0.0 P=333.0 w=0.2 n=0 I=1.0;\n' % fr)
        f.write('A2G BUD tick f=1351 p=1 cr=5000.0 gain=667.5 inc68=13.0 T=45.00 sumP=5000.0 |\n')
    res, txt = a2f_report.run(recs, None, os.path.join(out, 'A2FB_TYPES.json'), rd, port_trace=trace,
                              port_perception=ref)
    check('report a2j: sections 6 (D4), 7 (D3) and 8 (P / I) written', all(s in txt for s in ('== 6.', '== 7.', '== 8.')),
          txt[-1500:])
    sec6 = txt[txt.find('== 6.'):txt.find('== 7.')]
    check('report a2j: D4 table lines retail | port by frame (f=270: SCP 0.293333 | 0.311111, MK 0.6 | 0.617778)',
          re.search(r'\n\s+270 EMPIRE\s+0\.293333\|0\.311111\s+0\.293333\|0\s+0\.6\|0\.617778', sec6) is not None and
          'UNDERWORLD>Kashyyyk' in sec6 and 'EMPIRE>Felucia' in sec6, sec6[:1500])
    sec7 = txt[txt.find('== 7.'):txt.find('== 8.')]
    check('report a2j: D3 events and queue heads (Tatooine ground / space; Begin while busy = +1)',
          re.search(r'f=  103 EMPIRE\s+QB TATOOINE.*cr=8000', sec7) is not None and 'GROUND_MAGNEPULSE_CANNON@103' in sec7
          and 'TIE_FIGHTER_SQUADRON@110 +1' in sec7 and 'TATOOINE/ground' in sec7 and 'TATOOINE/space' in sec7,
          sec7[:1800])
    check('report a2j: first Begin per (player, planet, queue) with credits (lane A: per-queue start frames)',
          re.search(r'first Begin.*\n\s+f=  103 EMPIRE\s+TATOOINE\s+g GROUND_MAGNEPULSE_CANNON\s+cr=8000\n'
                    r'\s+f=  110 EMPIRE\s+TATOOINE\s+s TIE_FIGHTER_SQUADRON\s+cr=13000', sec7) is not None, sec7[:1800])
    sec8 = txt[txt.find('== 8.'):]
    check('report a2j: per-cycle P / I has retail and port values (OFF P port 1111.50, I port 2.50)',
          re.search(r'\n\s+1\s+1351\s+f\s+\d+\s+\S+\|\s*1111\.50\s+\S+\|\s*2\.50', sec8) is not None and
          'f1351 +667.5' in sec8, sec8[:1500])
    pt = open(os.path.join(rd, 'perception.tsv')).read().splitlines()
    check('report a2j: perception.tsv has retail and port columns', len(pt) > 40 and 'port_SCP' in pt[0], str(len(pt)))
    pr = open(os.path.join(rd, 'production.tsv')).read().splitlines()
    check('report a2j: production.tsv one row per event, with the queue', len(pr) == 7 and '\tground\t' in pr[1] and pr[0].endswith('\tcredits') and pr[1].endswith('\t8000'),
          str(pr[:3]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--lua')
    ap.add_argument('--out')
    a = ap.parse_args()
    lua = a.lua
    if not lua:
        for cand in (os.path.join(ROOT, 'tools', 'probe_mod', 'a1_tactical', 'out', 'tools', 'lua50.exe'),
                     os.path.join(os.path.dirname(ROOT), 'a1_tactical', 'out', 'tools', 'lua50.exe')):
            if os.path.isfile(cand):
                lua = cand
                break
    out = a.out or tempfile.mkdtemp(prefix='a2f_selftest_')
    os.makedirs(out, exist_ok=True)
    if not os.path.isfile(ZIP):
        print('extracted/foc_data_text.zip not found: cannot self-test')
        return 1
    z = zipfile.ZipFile(ZIP)
    global MODEL_EQ
    MODEL_EQ = model_equations(z)
    types = spec_checks(z)
    mod, types = build_test(out, ZIP)
    watcher_test(out)
    cleanup_test(out)
    if lua:
        lib = extract(z, out)
        lua_syntax(lua, mod, out)
        story_test(lua, lib, out, types)
        robust_test(lua, lib, out, types)
        hook_tests(lua, lib, out, types, z)
        percept_tests(lua, lib, out, types)
        sc = scoring_test(lua, lib, out, types, z)
        with open(sc, 'a') as f:      # the E pair's records go into the same report (section 6)
            f.write(open(os.path.join(out, 'percept_E_records.txt')).read())
        a2j_report_test(out, os.path.join(out, 'percept_U_records.txt'), sc)
    else:
        print('SKIPPED the Lua checks: no lua50.exe (tools\\probe_mod\\a1_tactical\\selftest\\build_lua50.cmd builds one)')
    bad = [n for n, ok in RESULTS if not ok]
    print('A2F SELFTEST %s (%d checks, %d failed) output: %s' % ('OK' if not bad else 'FAILED', len(RESULTS), len(bad), out))
    return 0 if not bad else 1


if __name__ == '__main__':
    sys.exit(main())
