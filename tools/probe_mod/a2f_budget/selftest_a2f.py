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
        check('spec: patched equation file is XML with %d probe equations' % want, len(n) == want == 2 + 36 + 9,
              str(len(n)))
        check('spec: shipped equations are kept', root.find('Roaming_Empire_Fleet_Target') is not None)
        bad = [e.tag for e in root if e.tag.startswith('A2FB_') and '#' in (e.text or '')]
        check('spec: probe equations have no #', not bad)
    except ET.ParseError as e:
        check('spec: patched equation file is XML', False, str(e))
    return types


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
function DumpCallStack() return debug.traceback() end
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
		Is_Human = function() return n == "Rebel" end,
		Get_Credits = function() return A2_CREDITS end,
		Get_Tech_Level = function() return 3 end,
		Get_Difficulty = function() return 1 end }
end
for _, nm in ipairs({ "Empire", "Rebel", "Underworld", "Pirates" }) do
	A2_PLAYERS[nm] = mkplayer(nm)
end
function Find_Player(n) return A2_PLAYERS[n] end
function EvaluatePerception(name, player)
	local t = GetCurrentTime()
	local pre = t < A2_INIT_T
	if name == "A2FB_CreditsUnnorm" then
		if pre then return 0 end
		return A2_CREDITS
	end
	if name == "A2FB_NetIncome" then return 667.5 end
	if A2_SHIPPED[name] ~= nil then return A2_SHIPPED[name] end
	local s, e, f3, k, cat = string.find(name, "^A2FB_(%u+)_BFB_(%d)_(%u+)$")
	if s == nil then return nil end
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
for frame = 0, 46000 do
	A2_T = frame / 30
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
        f.write('A2_OUTFILE = "%s"\nA2_INIT_T = 0.5\n' % outfile.replace('\\', '\\\\'))
        f.write(types_lua(types))
        f.write(STUBS)
        f.write(build_a2f_mod.story_lua())
        f.write(STORY_DRIVER)
    rc, text = lua_run(lua, script, env)
    out_text = open(outfile).read() if os.path.exists(outfile) else ''
    check('story: lua exit 0, no MAIN_ERROR', rc == 0 and 'MAIN_ERROR' not in out_text, (text + out_text)[:600])
    check('story: state machine ran A2FB_State', 'STATE A2FB_State' in out_text)
    check('story: canary call + game messages', 'CANARYCALL Mods\\A2fBudgetProbe\\A2FB_Canary_Story.txt' in out_text
          and 'GAME_MESSAGE TEXT_A2FB_LOADED' in out_text and 'GAME_MESSAGE TEXT_A2FB_DONE' in out_text
          and 'GAME_MESSAGE TEXT_A2FB_LONG_DONE' in out_text)
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
                                                                    'MJ', 'LONGDONE')),
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
    mt = open(os.path.join(out, 'major.tsv')).read().splitlines()
    check('report: major.tsv has one row per MJ sample', len(mt) == len(mj) + 1 and 'AGM' in mt[0], str(len(mt)))
    return ok_all


HOOK_FS = r'''
A2_OUT_CALLS = 0
function FreeStoreService() A2_OUT_CALLS = A2_OUT_CALLS + 1 end
'''

FS_DRIVER = r'''
PlayerObject = A2_PLAYERS["Empire"]
A2_T = 10
FreeStoreService()
A2_T = 18
FreeStoreService()
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
        f.write('A2_OUTFILE = "%s"\nA2_INIT_T = 0.5\n' % outfile.replace('\\', '\\\\'))
        f.write(types_lua(types))
        f.write(STUBS)
        f.write(body)
        f.write(FS_DRIVER)
    rc, text = lua_run(lua, script, env)
    o = open(outfile).read() if os.path.exists(outfile) else ''
    check('hooks: free store wrapper calls the shipped function twice', rc == 0 and 'ORIG_CALLS 2' in o, (text + o)[:400])
    check('hooks: free store samples PlayerObject (SM, BF, SH, PL)', all(('|%s|' % k) in o for k in ('SM', 'BF', 'SH', 'PL')))
    check('hooks: free store canary', 'CANARYCALL Mods\\A2fBudgetProbe\\A2FB_Canary_FreeStore.txt' in o)
    # the real shipped plan scripts with the hook
    names = {i.filename.upper(): i for i in z.infolist()}
    for plan, thread in build_a2f_mod.PLANS:
        orig = zread(z, names, 'DATA/SCRIPTS/AI/%s.LUA' % plan.upper())
        body = build_a2f_mod.assemble(orig, 'P', 'a2fb_plan_hook.lua', True, build_a2f_mod.plan_subs(plan, thread))
        outfile = os.path.join(out, 'plan_%s.txt' % plan)
        script = os.path.join(out, 'plan_%s.lua' % plan)
        with open(script, 'w', encoding='latin-1') as f:
            f.write('A2_OUTFILE = "%s"\nA2_INIT_T = 0.5\n' % outfile.replace('\\', '\\\\'))
            f.write(types_lua(types))
            f.write(STUBS)
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
        if plan in build_a2f_mod.FULL_SAMPLE_PLANS:
            check('hooks: plan %s full samples at start and end, Flush_Category ran' % plan,
                  'FLUSHED MajorItem' in o and '|BF|tag=PS' in o and '|BF|tag=PE' in o and '|MJ|tag=PS' in o, o[-700:])


def build_test(out, zip_path):
    mod = os.path.join(out, 'mod')
    types = build_a2f_mod.build(None, mod, zip_path)
    exp = ['Data/XML/AI/PerceptualEquations/AI_Equations_ExpansionGeneric_Galactic.xml', 'Data/Scripts/Story/A2fb_Story.lua',
           'Data/Scripts/FreeStore/GalacticFreeStore.lua', 'A2FB_TYPES.json', build_a2f_mod.MARKER] + \
          ['Data/XML/Story_Plots_Sandbox_12U_%s.xml' % f for f in build_a2f_mod.PLOTS] + \
          ['Data/Scripts/AI/%s.lua' % n for n, _ in build_a2f_mod.PLANS] + list(build_a2f_mod.CANARIES.values())
    missing = [e for e in exp if not os.path.exists(os.path.join(mod, e))]
    check('build: mod folder complete (%d files)' % len(exp), not missing, str(missing))
    shutil.copy(os.path.join(mod, 'A2FB_TYPES.json'), os.path.join(out, 'A2FB_TYPES.json'))
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
    types = spec_checks(z)
    mod, types = build_test(out, ZIP)
    watcher_test(out)
    cleanup_test(out)
    if lua:
        lib = extract(z, out)
        lua_syntax(lua, mod, out)
        story_test(lua, lib, out, types)
        hook_tests(lua, lib, out, types, z)
    else:
        print('SKIPPED the Lua checks: no lua50.exe (tools\\probe_mod\\a1_tactical\\selftest\\build_lua50.cmd builds one)')
    bad = [n for n, ok in RESULTS if not ok]
    print('A2F SELFTEST %s (%d checks, %d failed) output: %s' % ('OK' if not bad else 'FAILED', len(RESULTS), len(bad), out))
    return 0 if not bad else 1


if __name__ == '__main__':
    sys.exit(main())
