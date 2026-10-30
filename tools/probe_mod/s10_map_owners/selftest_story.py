#!/usr/bin/env python3
"""selftest_story.py - runs the rendered story scripts S10_Story_<A|B>.lua under a stock Lua 5.0.2 interpreter
together with the game's own script library (Data\\Scripts\\Library from extracted/foc_data_text.zip: PGStoryMode,
PGStateMachine, PGBase, PGSpawnUnits, ...), with only the engine functions stubbed.

  selftest_story.py --lua <lua.exe built from toolchain\\lua-5.0.2>

The driver plays the engine: Base_Definitions(), main() as a coroutine resumed every 0.2 s of game time, threads made
by Create_Thread resumed the same way, and Story_Event_Trigger("S10_Begin") at 3 s (what the story event of
S10_Story_<L>.xml does). Checks: the state machine reaches S10_Begin, both sides are spawned on the right planets
for the right players, the fleet moves to the target and Invade() is called, the proof popups are produced.
"""
import argparse
import io
import os
import subprocess
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import build_mod  # noqa: E402

STUBS = r'''
S10_OUT = io.open(S10_LOG, "w")
function S10_W(s) S10_OUT:write(s .. "\n") end
Script = "S10_Story"
S10_T = 0
function GetCurrentTime() return S10_T end
S10_TV = {}
local function tv()
	local co = S10_CUR or "main"
	if not S10_TV[co] then S10_TV[co] = {} end
	return S10_TV[co]
end
ThreadValue = setmetatable({ Set = function(k, v) tv()[k] = v end, Reset = function() end },
	{ __call = function(_, k) return tv()[k] end })
GetEvent = setmetatable({ Reset = function() end, Params = function() return nil end },
	{ __call = function() return nil end })
function _ScriptMessage(s) end
function _OuputDebug(s) end
function _MessagePopup(s) S10_W("POPUP " .. s) end
function Game_Message(s) S10_W("GAME_MESSAGE " .. s) end
function DumpCallStack() return debug.traceback() end
function ScriptExit() S10_W("SCRIPT_EXIT") error("ScriptExit") end
S10_PLANETS = { Tatooine = true, Byss = true, Abregado_Rae = true, Jabiim = true }
S10_FACTIONS = { Rebel = true, Empire = true, Underworld = true, Pirates = true, Neutral = true }
function Find_Player(n) if S10_FACTIONS[n] then return { name = n } end return nil end
function FindPlanet(n)
	return { name = n, Is_Valid = function() return S10_PLANETS[n] == true end }
end
function Find_Object_Type(t) return { name = t } end
function Spawn_Unit(t, where, player)
	S10_W("SPAWN " .. t.name .. " at=" .. where.name .. " player=" .. player.name)
	local u = { type = t.name, player = player.name, at = where.name }
	u.Prevent_AI_Usage = function(b) end
	u.Mark_Parent_Mode_Object_For_Death = function() S10_W("MARK_DEATH " .. t.name) end
	return { u }
end
function Assemble_Fleet(units)
	local n = 0
	for _, u in pairs(units) do n = n + 1 end
	S10_W("ASSEMBLE " .. n)
	local fleet = { Is_Valid = function() return n > 0 end }
	fleet.Move_To = function(p)
		S10_W("MOVE_TO " .. p.name .. " t=" .. S10_T)
		local done_at = S10_T + 2
		return { IsFinished = function() return S10_T >= done_at end, Result = function() return true end }
	end
	fleet.Invade = function() S10_W("INVADE t=" .. S10_T) end
	return fleet
end
S10_THREADS = {}
function Create_Thread(name, param)
	S10_W("CREATE_THREAD " .. name)
	table.insert(S10_THREADS, coroutine.create(function() _G[name](param) end))
end
'''

DRIVER = r'''
Base_Definitions()
local main_co = coroutine.create(main)
local triggered = false
for step = 0, 100 do
	S10_T = step * 0.2
	if S10_T >= 3 and not triggered then
		triggered = true
		Story_Event_Trigger("S10_Begin")
	end
	S10_CUR = main_co
	local ok, err = coroutine.resume(main_co)
	if not ok then S10_W("MAIN_ERROR " .. tostring(err)) break end
	for _, co in ipairs(S10_THREADS) do
		if coroutine.status(co) == "suspended" then
			S10_CUR = co
			local tok, terr = coroutine.resume(co)
			if not tok then S10_W("THREAD_ERROR " .. tostring(terr)) end
		end
	end
end
S10_W("STATE " .. tostring(CurrentState))
S10_OUT:close()
'''


def library(dst):
    """Data\\Scripts\\Library\\*.lua of FoC from the repository's text zip, written as <dst>\\<name>.lua."""
    z = zipfile.ZipFile(os.path.join(ROOT, 'extracted', 'foc_data_text.zip'))
    n = 0
    for info in z.infolist():
        p = info.filename.replace('\\', '/').upper()
        if '/SCRIPTS/LIBRARY/' in p and p.endswith('.LUA'):
            with open(os.path.join(dst, os.path.basename(info.filename)), 'wb') as f:
                f.write(z.read(info))
            n += 1
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--lua', required=True)
    a = ap.parse_args()
    out_dir = os.path.join(HERE, 'out', 'selftest')
    lib = os.path.join(out_dir, 'library')
    os.makedirs(lib, exist_ok=True)
    nlib = library(lib)
    ok_all = nlib > 0
    for probe in build_mod.PROBES:
        label, target, source, defender, defense, _ = probe
        log = os.path.join(out_dir, 'story_%s.txt' % label)
        lua = os.path.join(out_dir, 'story_test_%s.lua' % label)
        with open(lua, 'w', encoding='ascii') as f:
            f.write('S10_LOG = %s\n' % ('"' + log.replace('\\', '\\\\') + '"'))
            f.write(STUBS)
            f.write(build_mod.story_lua(probe))
            f.write(DRIVER)
        env = dict(os.environ, LUA_PATH=os.path.join(lib, '?.lua'))
        r = subprocess.run([a.lua, lua], capture_output=True, text=True, env=env)
        sys.stdout.write(r.stdout + r.stderr)
        text = open(log).read() if os.path.exists(log) else ''
        lines = text.splitlines()
        attack = [ln for ln in lines if ln.startswith('SPAWN ') and ln.endswith('player=Underworld')]
        guard = [ln for ln in lines if ln.startswith('SPAWN ') and ln.endswith('player=' + defender)]
        checks = {
            'lua exit 0': r.returncode == 0,
            'no errors': 'ERROR' not in text and 'SCRIPT_EXIT' not in text,
            'state S10_Begin': 'STATE S10_Begin' in text,
            'one thread': text.count('CREATE_THREAD S10_Invade') == 1,
            'attackers at source': len(attack) == 4 and all(' at=%s ' % source in ln for ln in attack),
            'defenders at target': len(guard) == len(defense) and all(' at=%s ' % target in ln for ln in guard),
            'fleet of 4': 'ASSEMBLE 4' in text,
            'move to target': 'MOVE_TO %s ' % target in text,
            'invade after move': 'INVADE' in text and text.index('INVADE') > text.index('MOVE_TO'),
            'proof popups': 'POPUP S10 story %s: story script running' % target in text and
                            'Invade issued ok=true' in text,
            'game message': 'GAME_MESSAGE TEXT_S10_STORY_LUA' in text,
        }
        bad = [k for k, v in checks.items() if not v]
        print('story %s (%s from %s vs %s): %s' % (label, target, source, defender, 'OK' if not bad else
                                                   'FAILED ' + ', '.join(bad)))
        ok_all = ok_all and not bad
    print('library files: %d' % nlib)
    print('STORY SELFTEST OK' if ok_all else 'STORY SELFTEST FAILED')
    return 0 if ok_all else 1


if __name__ == '__main__':
    sys.exit(main())
