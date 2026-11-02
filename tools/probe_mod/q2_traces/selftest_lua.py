#!/usr/bin/env python3
"""selftest_lua.py - runs the Q2 Lua probe scripts under a stock Lua 5.0.2 with stubbed engine functions (no game).

  python -I selftest_lua.py        ends with SELFTEST_LUA OK / FAILED / SKIPPED

Galactic: mod\\Q2Trace\\Data\\Scripts\\Story\\Q2_Trace_Galactic.lua is loaded, Definitions() runs, the harness plays
PGStateMachine's Process_States (OnEnter, then OnUpdate once per game second for 10 fiscal cycles). Checks: the
canary call names Mods\\Q2Trace\\Q2Trace_Canary_Galactic.txt with two strings, StoryModeEvents is a table, LOAD / DEF /
ENTER, 11 cycles with players, planets and perception rows for both AI players, a failing equation gives ERR once.
Scoring: the probe part of mod\\...\\Miscellaneous\\GameScoring.lua (after its marker) runs on stub originals; one
space mode with a thread, kills and a build. Checks the records, that the shipped handlers are still called and the
shipped GameService still runs every 10th service.
Live memory: while lua50.exe still holds its records, q2lua_reader's parser over an a1mem scan of that process
(ReadProcessMemory) must find exactly the records the script printed.
Lua: out\\tools\\lua50.exe, built here with the project MSVC at /Od on first use (as fow_regrow / a1_tactical do), or
an existing a1_tactical / fow_regrow build. Without the toolchain the test is SKIPPED.
"""
import os
import subprocess
import sys
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'a1_tactical'))
import q2lua_reader  # noqa: E402

MOD = os.path.join(HERE, 'mod', 'Q2Trace', 'Data', 'Scripts')
OUT = os.path.join(HERE, 'out', 'selftest', 'lua')
TOOLS = os.path.join(HERE, 'out', 'tools')
LUA_SRC = ['lapi', 'lcode', 'ldebug', 'ldo', 'ldump', 'lfunc', 'lgc', 'llex', 'lmem', 'lobject', 'lopcodes', 'lparser',
           'lstate', 'lstring', 'ltable', 'ltm', 'lundump', 'lvm', 'lzio']
LUA_LIB = ['lauxlib', 'lbaselib', 'ldblib', 'liolib', 'lmathlib', 'loadlib', 'lstrlib', 'ltablib']
FAILS = []

COMMON = r'''
Q2S = { t = 0, frame = 0, canary = {}, calls = {} }
GetCurrentTime = setmetatable({ Frame = function() return Q2S.frame end }, { __call = function() return Q2S.t end })
function require(name) end
function _CustomScriptMessage(a, b)
	if type(a) ~= "string" or type(b) ~= "string" then error("LuaCustomScriptMessage -- expected two string parameters.") end
	table.insert(Q2S.canary, a)
end
function Q2S_Player(name, id, credits)
	return { Get_ID = function() return id end, Get_Credits = function() return credits end,
		Get_Tech_Level = function() return 2 end, Get_Faction_Name = function() return string.upper(name) end }
end
Q2S_Players = { Empire = Q2S_Player("Empire", 1, 10000), Rebel = Q2S_Player("Rebel", 0, 8000),
	Underworld = Q2S_Player("Underworld", 6, 7000) }
function Find_Player(name) return Q2S_Players[name] end
function Q2S_Dump()
	for i = 1, table.getn(Q2L_Records) do print(Q2L_Records[i]) end
	io.write("READY\n")
	io.flush()
	io.read("*l")
end
'''

GALACTIC = r'''
function Q2S_Planet(name, owner)
	return { Get_Type = function() return { Get_Name = function() return name end } end,
		Get_Owner = function() return Q2S_Players[owner] end }
end
FindPlanet = { Get_All_Planets = function()
	return { Q2S_Planet("Nal_Hutta", "Underworld"), Q2S_Planet("Coruscant", "Empire"), Q2S_Planet("Yavin", "Rebel") }
end }
function EvaluatePerception(eq, player, target)
	if type(eq) ~= "string" or player == nil then error("Evaluate_Perception: Invalid parameter type") end
	if eq == "Q2_Target_Player" and target.Get_Type().Get_Name() == "Yavin" then error("bad equation") end
	return string.len(eq) + player.Get_ID()
end
-- PGStateMachine / PGStoryMode as shipped: Base_Definitions -> Definitions, PG_Story_Mode_Init, main -> Process_States
StateMachine = {}
OnEnter = 1  OnUpdate = 2  OnExit = 3
NextState = nil
function Define_State(state, f) StateMachine[state] = f; if NextState == nil then NextState = state end end
dofile(Q2S_SCRIPT)
Definitions()
assert(type(StoryModeEvents) == "table", "StoryModeEvents must be a table for PG_Story_Mode_Init")
for k, v in pairs(StoryModeEvents) do Define_State(k, v) end
Define_State("PG_Story_State_Init", function() end)
assert(NextState == "Q2_State_Trace", "first state must be Q2_State_Trace, got " .. tostring(NextState))
local f = StateMachine[NextState]
f(OnEnter)
for s = 0, 460 do
	Q2S.t = s + 0.0333
	Q2S.frame = s * 30 + 1
	f(OnUpdate)
end
print("CANARY_CALLS " .. table.getn(Q2S.canary) .. " " .. tostring(Q2S.canary[1]))
Q2S_Dump()
'''

SCORING = r'''
Q2S.orig = { start = 0, ending = 0, kill = 0, built = 0, service = 0 }
function StringCompare(a, b) return string.upper(a) == string.upper(b) end
function Game_Mode_Starting_Event(mode, map) LastModeName = mode; Q2S.orig.start = Q2S.orig.start + 1 end
function Game_Mode_Ending_Event(mode) Q2S.orig.ending = Q2S.orig.ending + 1 end
function Tactical_Unit_Destroyed_Event(o, k) Q2S.orig.kill = Q2S.orig.kill + 1 end
function Tactical_Production_End_Event(t, p, l) Q2S.orig.built = Q2S.orig.built + 1 end
function GameService() Q2S.orig.service = Q2S.orig.service + 1 end
function Find_All_Objects_Of_Type(p) return { 1, 2, 3 } end
Q2S.threads = {}
function Create_Thread(name, param) table.insert(Q2S.threads, { name, param }) end
Q2S.sleeps = 0
function Sleep(s)
	Q2S.sleeps = Q2S.sleeps + 1
	Q2S.t = Q2S.t + s
	Q2S.frame = Q2S.frame + 30 * s
	if Q2S.sleeps == 6 then Game_Mode_Ending_Event("Space") end
end
dofile(Q2S_SCRIPT)
Definitions()
for i = 1, 20 do GameService() end
Game_Mode_Starting_Event("Space", "_SPACE_PLANET_GEONOSIS_01")
local unit = { Get_Type = function() return { Get_Name = function() return "X_WING" end } end,
	Get_Owner = function() return Q2S_Players.Rebel end }
Q2S.t = Q2S.t + 0.5
Tactical_Unit_Destroyed_Event(unit, Q2S_Players.Empire)
Tactical_Production_End_Event({ Get_Name = function() return "TIE_FIGHTER" end }, Q2S_Players.Empire, nil)
local th = Q2S.threads[1]
getfenv(0)[th[1]](th[2])
print("ORIG " .. Q2S.orig.start .. " " .. Q2S.orig.ending .. " " .. Q2S.orig.kill .. " " .. Q2S.orig.built .. " " ..
	Q2S.orig.service .. " THREADS " .. table.getn(Q2S.threads))
print("CANARY_CALLS " .. table.getn(Q2S.canary) .. " " .. tostring(Q2S.canary[1]))
Q2S_Dump()
'''


def check(cond, what):
    print(('  ok    ' if cond else '  FAIL  ') + what)
    if not cond:
        FAILS.append(what)


def find_lua():
    for p in (os.path.join(TOOLS, 'lua50.exe'), os.path.join(HERE, '..', 'a1_tactical', 'out', 'tools', 'lua50.exe'),
              os.path.join(HERE, '..', 'fow_regrow', 'out', 'tools', 'lua50.exe')):
        if os.path.isfile(p):
            return p
    setup = os.path.join(REPO, 'toolchain', 'msvc', 'setup_x64.bat')
    src = os.path.join(REPO, 'toolchain', 'lua-5.0.2')
    if not (os.path.isfile(setup) and os.path.isfile(os.path.join(src, 'src', 'lapi.c'))):
        return None
    os.makedirs(TOOLS, exist_ok=True)
    files = ['"%s"' % os.path.join(src, 'src', n + '.c') for n in LUA_SRC] + \
        ['"%s"' % os.path.join(src, 'src', 'lib', n + '.c') for n in LUA_LIB] + \
        ['"%s"' % os.path.join(src, 'src', 'lua', 'lua.c')]
    cmd = 'call "%s" >nul && cl /nologo /Od /W0 /MT /D_CRT_SECURE_NO_WARNINGS /I"%s" /I"%s" %s /Fe"%s"' % (
        setup, os.path.join(src, 'include'), os.path.join(src, 'src'), ' '.join(files),
        os.path.join(TOOLS, 'lua50.exe'))
    bat = os.path.join(TOOLS, 'build_lua50.cmd')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n' + cmd + '\r\n')
    r = subprocess.run(['cmd', '/c', bat], cwd=TOOLS, capture_output=True, text=True)
    with open(os.path.join(TOOLS, 'lua50_build.txt'), 'w') as f:
        f.write(r.stdout + r.stderr)
    for n in os.listdir(TOOLS):
        if n.endswith('.obj'):
            os.remove(os.path.join(TOOLS, n))
    p = os.path.join(TOOLS, 'lua50.exe')
    return p if os.path.isfile(p) else None


def lua_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def run_harness(lua, name, script_path, body):
    """Runs one harness; returns (printed lines, records found in the live process, scan ok)."""
    os.makedirs(OUT, exist_ok=True)
    h = os.path.join(OUT, 'harness_%s.lua' % name)
    with open(h, 'w', encoding='ascii', newline='\n') as f:
        f.write('Q2S_SCRIPT = %s\n' % lua_str(script_path) + COMMON + body)
    p = subprocess.Popen([lua, h], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, cwd=OUT)
    lines = []
    while True:
        line = p.stdout.readline()
        if not line:
            break
        line = line.rstrip('\n')
        if line == 'READY':
            break
        lines.append(line)
    found = None
    if p.poll() is None:
        try:
            import a1mem
            proc = a1mem.Proc(p.pid, test_base=0)
            hits, _st = proc.scan([q2lua_reader.NEEDLE])
            found = {r[5] for r in q2lua_reader.parse_hits(hits[q2lua_reader.NEEDLE])}
            proc.close()
        except (OSError, ImportError) as e:
            print('  note  live scan unavailable: %s' % e)
        p.stdin.write('\n')
        p.stdin.flush()
    try:
        _out, err = p.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        p.kill()
        _out, err = p.communicate()
    check(p.returncode == 0, '%s harness exit 0 %s' % (name, err.strip()[-300:] if err else ''))
    return lines, found


def records_of(lines):
    recs = [r for r in q2lua_reader.parse_hits([(0, ln.encode('ascii', 'replace')) for ln in lines])]
    return recs, [ln for ln in lines if ln.startswith('Q2L1|')]


def main():
    lua = find_lua()
    if not lua:
        print('SELFTEST_LUA SKIPPED (no lua50.exe and no toolchain to build it)')
        return 0
    print('lua: %s' % lua)

    gal = os.path.join(MOD, 'Story', 'Q2_Trace_Galactic.lua')
    lines, found = run_harness(lua, 'galactic', gal, GALACTIC)
    recs, raw = records_of(lines)
    check(len(recs) == len(raw) and recs, 'galactic: every printed record parses (%d)' % len(recs))
    check(all(len(r) <= 400 for r in raw), 'galactic: every record fits one scan hit')
    kinds = {}
    for r in recs:
        kinds[r[3]] = kinds.get(r[3], 0) + 1
    check(all(r[0] == 'G' for r in recs), 'galactic: src G')
    check(kinds.get('LOAD') == 1 and kinds.get('DEF') == 1 and kinds.get('ENTER') == 1, 'galactic: LOAD, DEF, ENTER')
    check(any(ln == 'CANARY_CALLS 1 Mods\\Q2Trace\\Q2Trace_Canary_Galactic.txt' for ln in lines),
          'galactic: one canary call on Mods\\Q2Trace\\Q2Trace_Canary_Galactic.txt')
    check(kinds.get('CANARY') == 1, 'galactic: CANARY record')
    check(kinds.get('CYCLE') == 11 and kinds.get('CYCLEEND') == 11, 'galactic: 11 cycles (0..10) (%s)' % kinds.get('CYCLE'))
    check(kinds.get('PLAYER') == 33, 'galactic: 3 players x 11 cycles')
    check(kinds.get('PLANET') == 33 and kinds.get('PERCEPTION') == 66, 'galactic: planets and 2 AI x 3 planets x 11')
    check(kinds.get('ERR') == 1, 'galactic: failing equation reported once')
    _u, rows, eqs, s = q2lua_reader.summarize([r[:5] for r in recs])
    check(eqs == ['Q2_Space_Total', 'Q2_Ground_Total', 'Q2_Has_Space_Force', 'Q2_Target_Player',
                  'Roaming_Empire_Fleet_Target'], 'galactic: perception columns %s' % eqs)
    yav = [d for _sid, _seq, d in rows if d.get('planet') == 'Yavin' and d.get('ai') == 'Rebel']
    check(yav and yav[0].get('Q2_Target_Player') == 'ERR' and yav[0].get('Q2_Space_Total') == '14',
          'galactic: values and ERR per equation')
    check(s['scripts']['G']['verdict'].startswith('ran: 11 cycles'), 'galactic: verdict "%s"' % s['scripts']['G']['verdict'])
    if found is not None:
        check(found == set(raw), 'galactic: live memory scan finds exactly the printed records (%d of %d)' % (
            len(found & set(raw)), len(raw)))

    sc_full = os.path.join(MOD, 'Miscellaneous', 'GameScoring.lua')
    with open(sc_full, encoding='latin-1') as f:
        text = f.read()
    mark = text.find('-- Q2 reference traces (tools/probe_mod/q2_traces): tactical Lua probe')
    check(mark > 0, 'scoring: probe marker found')
    probe = os.path.join(OUT, 'scoring_probe_part.lua')
    with open(probe, 'w', encoding='latin-1', newline='\n') as f:
        f.write(text[mark:])
    lines, found = run_harness(lua, 'scoring', probe, SCORING)
    recs, raw = records_of(lines)
    kinds = {}
    for r in recs:
        kinds[r[3]] = kinds.get(r[3], 0) + 1
    check(len(recs) == len(raw) and recs and all(r[0] == 'T' for r in recs), 'scoring: records parse, src T (%d)' % len(recs))
    check(any(ln == 'CANARY_CALLS 1 Mods\\Q2Trace\\Q2Trace_Canary_Scoring.txt' for ln in lines),
          'scoring: one canary call on Mods\\Q2Trace\\Q2Trace_Canary_Scoring.txt')
    check(kinds.get('LOAD') == 1 and kinds.get('DEF') == 1 and kinds.get('SERVICE') == 1, 'scoring: LOAD, DEF, SERVICE')
    check(kinds.get('TMODE') == 2 and kinds.get('TKILL') == 1 and kinds.get('TBUILT') == 1, 'scoring: mode, kill, build')
    check(kinds.get('THREAD') == 2, 'scoring: thread start and end')
    check(kinds.get('TSAMPLE', 0) >= 6 and kinds.get('TPLAYER') == 3 * kinds.get('TSAMPLE', 0),
          'scoring: samples with 3 players each (%s)' % kinds.get('TSAMPLE'))
    check(not kinds.get('ERR'), 'scoring: no ERR')
    check(any(ln == 'ORIG 1 1 1 1 2 THREADS 1' for ln in lines), 'scoring: shipped handlers called, GameService every 10th')
    if found is not None:
        check(found == set(raw), 'scoring: live memory scan finds exactly the printed records')

    print('SELFTEST_LUA ' + ('FAILED (%d)' % len(FAILS) if FAILS else 'OK'))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
