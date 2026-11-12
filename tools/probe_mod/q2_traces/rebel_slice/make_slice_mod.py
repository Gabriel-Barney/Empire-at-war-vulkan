#!/usr/bin/env python3
r"""make_slice_mod.py - builds the a2m t07 probe mod Mods\Q2RebelSlice: the shipped RebelAdvanceTechPlan.lua (plan of the
Rebel goal Advance_Tech_Rebel) with a read-only probe block appended, a canary file and a marker.

  make_slice_mod.py --out <game>\corruption\Mods\Q2RebelSlice [--zip extracted\foc_data_text.zip]
  make_slice_mod.py --remove <game>\corruption\Mods\Q2RebelSlice      (only a folder that holds the marker)
  make_slice_mod.py --print                                            (the generated plan script to stdout)

The shipped script (Data\SCRIPTS\AI\REBELADVANCETECHPLAN.LUA from the extracted text zip, CRLF -> LF, otherwise unchanged)
does:  Target = FindTarget(TechForce, "Is_Good_Slice_Target", "Enemy", 0.1)   (ScriptExit when none)
       BlockOnCommand(TechForce.Produce_Force(Target))      block k=1  (Droids_Team = 1, produced or taken from the store)
       BlockOnCommand(TechForce.Move_To(Target))            block k=2
       BlockOnCommand(LandUnits(TechForce))                 LandUnits = BlockOnCommand(taskforce.Land_Units()): k=3,
                                                            then the outer BlockOnCommand(nil): k=4
       TechForce.Set_Plan_Result(true); ScriptExit()
The slice itself is engine side: landing R2D2 on an enemy planet fires its Ground_Activated slicer ability
(docs/re/REMAINING_GAPS.md section 1.3: AI pick among affordable entries, price charged with Add_Credits, the type
appended to the player's unlocked list). Lua cannot read the unlocked list, so slice_watch.py reads it (and the credits)
from memory; the plan records give the plan, its target and the credits / tech level around the landing.

Records (Q2L1|P|<sid>|<seq>|<KIND>|<text>|END, the q2 kit's format, so q2lua_reader.py collects them; plan=Advance_Tech_Rebel):
  LOAD     once per pooled script instance
  PSTART   thread start: n (run number in the instance), prev (end of the previous run; none = killed by a release)
  PCAND    a2m r2 (t14.4): one per planet of FindPlanet.Get_All_Planets() before the shipped FindTarget: i, planet,
           owner, slice = EvaluatePerception("Is_Good_Slice_Target", PlayerObject, planet)
  PTARGET  before block 1: target (planet type of Target.Get_Game_Object()), owner (its faction), blk (type of the status)
  PPROD / PMOVE / PLAND / PLANDX   after blocks 1 / 2 / 3 / 4: ret, dt (seconds blocked); PLANDPRE before block 3
  PDONE / PEXIT / PFAIL            thread returned / ScriptExit / TechForce_Production_Failed
  ERR      a probe error (each distinct one once; the shipped code still runs)
Every record: t, f (mode frame = round(t x 30)), plan, player, n, cr (PlayerObject.Get_Credits), tech (Get_Tech_Level),
count (TechForce.Get_Force_Count). Rules (docs/re/LUA_SCRIPT_OUTPUT.md section 4): records and state in a closure (they
survive the PGBase Flush_G of a pool release), every probe step in pcall with the shipped calls outside it, no math /
'#' / '%', library functions wrapped once per instance, only bindings shipped scripts call on the same wrappers.
Standard library only.
"""
import argparse
import os
import re
import shutil
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..'))
ZIP = os.path.join(REPO, 'extracted', 'foc_data_text.zip')
MOD_NAME = 'Q2RebelSlice'
MARKER = 'Q2REBELSLICE_MARKER.txt'
CANARY = 'Q2RebelSlice_Canary_Plan.txt'
PLAN_FILE = 'RebelAdvanceTechPlan'

BLOCK = r'''
-- a2m t07 Rebel slice probe (tools/probe_mod/q2_traces/rebel_slice/make_slice_mod.py; edit the generator, not this
-- file): read-only records of each Advance_Tech_Rebel plan run. See the generator's docstring for the record list.
Q2S_PLAN = "Advance_Tech_Rebel"

if Q2S_Say == nil then
	local recs = {}
	local seq = 0
	local errs = {}
	local st = {}
	local needle = "Q2L" .. "1|P|"
	local sid = string.gsub(tostring(recs), "^table: ", "")
	function Q2S_Say(kind, text)
		if seq >= 4000 then
			return
		end
		seq = seq + 1
		local s = string.gsub(tostring(text), "[^ -~]", "?")
		s = string.gsub(s, "|", "/")
		s = string.sub(s, 1, 320)
		recs[seq] = needle .. sid .. "|" .. seq .. "|" .. kind .. "|" .. s .. "|END"
	end
	function Q2S_Error(where, err)
		local key = string.sub(where .. " " .. tostring(err), 1, 200)
		if errs[key] == nil then
			errs[key] = true
			Q2S_Say("ERR", key)
		end
	end
	function Q2S_Count()
		return seq
	end
	function Q2S_Rec(i)
		return recs[i]
	end
	function Q2S_Get(key, default)
		local v = st[key]
		if v == nil then
			return default
		end
		return v
	end
	function Q2S_Set(key, v)
		st[key] = v
	end
	Q2S_Say("LOAD", "plan=" .. Q2S_PLAN .. " tfs=TechForce kit=rebel_slice")
	pcall(_CustomScriptMessage, "Mods\\@MOD@\\@CANARY@", "q2 rebel slice canary\n")
end

function Q2S_Str(f)
	local ok, v = pcall(f)
	if ok and v ~= nil then
		return tostring(v)
	end
	return "?"
end

-- Runs a probe step; an error becomes one ERR record.
function Q2S_Try(where, f, a, b, c)
	local ok, err = pcall(f, a, b, c)
	if not ok then
		pcall(Q2S_Error, where, err)
	end
end

function Q2S_Now()
	local ok, t = pcall(GetCurrentTime)
	if ok and type(t) == "number" then
		return t
	end
	return -1
end

-- mode frame = round(t x 30), no math library
function Q2S_Frame(t)
	if t < 0 then
		return "?"
	end
	return string.format("%d", t * 30 + 0.5)
end

-- Target is FindTarget's result, an AITargetLocationWrapper (Get_Game_Object, as GalacticFreeStore uses on FindTarget
-- results); a game object wrapper (no Get_Game_Object: a Lua error, caught) falls back to its own Get_Type.
function Q2S_Target()
	if not Target then
		return "none"
	end
	local n = Q2S_Str(function() return Target.Get_Game_Object().Get_Type().Get_Name() end)
	if n == "?" then
		n = "obj:" .. Q2S_Str(function() return Target.Get_Type().Get_Name() end)
	end
	return n
end

function Q2S_Owner()
	if not Target then
		return "none"
	end
	return Q2S_Str(function() return Target.Get_Game_Object().Get_Owner().Get_Faction_Name() end)
end

function Q2S_Record(kind, extra)
	local t = Q2S_Now()
	Q2S_Say(kind, "t=" .. Q2S_Str(function() return string.format("%.3f", t) end) .. " f=" .. Q2S_Frame(t) ..
		" plan=" .. Q2S_PLAN .. " player=" .. Q2S_Str(function() return PlayerObject.Get_Faction_Name() end) ..
		" n=" .. Q2S_Get("n", 0) .. (extra or "") ..
		" cr=" .. Q2S_Str(function() return PlayerObject.Get_Credits() end) ..
		" tech=" .. Q2S_Str(function() return PlayerObject.Get_Tech_Level() end) ..
		" count=" .. Q2S_Str(function() return TechForce.Get_Force_Count() end))
end

function Q2S_Ended(kind)
	local e = Q2S_Get("end", "first")
	if e == "none" then
		e = kind
	else
		e = string.sub(e .. "+" .. kind, 1, 40)
	end
	Q2S_Set("end", e)
end

function Q2S_Begin()
	local prev = Q2S_Get("end", "first")
	Q2S_Set("n", Q2S_Get("n", 0) + 1)
	Q2S_Set("k", 0)
	Q2S_Set("end", "none")
	Q2S_Record("PSTART", " prev=" .. prev ..
		" spend=" .. Q2S_Str(function() return Budget.Get_Spendable_Resources() end) ..
		" unalloc=" .. Q2S_Str(function() return Budget.Get_Unallocated_Resources() end))
end

function Q2S_Pre(block)
	local k = Q2S_Get("k", 0) + 1
	Q2S_Set("k", k)
	Q2S_Set("bt", Q2S_Now())
	if k == 1 then
		Q2S_Record("PTARGET", " target=" .. Q2S_Target() .. " owner=" .. Q2S_Owner() .. " blk=" .. type(block))
	elseif k == 3 then
		Q2S_Record("PLANDPRE", " target=" .. Q2S_Target() .. " owner=" .. Q2S_Owner() .. " blk=" .. type(block))
	end
end

function Q2S_Post(v)
	local k = Q2S_Get("k", 0)
	local names = { "PPROD", "PMOVE", "PLAND", "PLANDX" }
	local kind = names[k] or "PBLOCK"
	local ret = type(v)
	if ret == "boolean" then
		ret = tostring(v)
	end
	local t = Q2S_Now()
	local b = Q2S_Get("bt", -1)
	local dt = "?"
	if b >= 0 and t >= 0 then
		dt = string.format("%.3f", t - b)
	end
	local extra = " k=" .. k .. " ret=" .. ret .. " dt=" .. dt
	if k >= 3 then
		extra = extra .. " target=" .. Q2S_Target() .. " owner=" .. Q2S_Owner()
	end
	Q2S_Record(kind, extra)
end

function Q2S_Done()
	Q2S_Ended("PDONE")
	Q2S_Record("PDONE", " target=" .. Q2S_Target())
end

function Q2S_Exiting()
	Q2S_Ended("PEXIT")
	Q2S_Record("PEXIT", " target=" .. Q2S_Target() .. " owner=" .. Q2S_Owner())
end

function Q2S_Fail(args)
	local s = " args=" .. table.getn(args)
	for i = 2, table.getn(args) do
		local a = args[i]
		s = s .. " " .. Q2S_Str(function() return a.Get_Name() end)
	end
	Q2S_Ended("PFAIL")
	Q2S_Record("PFAIL", s .. " target=" .. Q2S_Target())
end

-- a2m r2 (docs/re/A2M_T08_T10_T14_RE.md t14.4): before the shipped FindTarget runs, one PCAND record per planet of
-- FindPlanet.Get_All_Planets() (list order): its type, owner faction and EvaluatePerception("Is_Good_Slice_Target",
-- PlayerObject, planet), the perception FindTarget scores. Read-only: the equation has no random token, and these are
-- the bindings PathBonusForDistantTargets.lua calls. A planet the pick never lands on with slice > 0 points to a
-- filter in front of the weighted pick (Applies / TargetPlayerID); slice = 0 shows the perception excludes it.
function Q2S_Cands()
	local ok, list = pcall(function() return FindPlanet.Get_All_Planets() end)
	if not ok or type(list) ~= "table" then
		Q2S_Record("PCAND", " i=0 planet=? err=" .. Q2S_Str(function() return list end))
		return
	end
	local i = 0
	for _, planet in pairs(list) do
		i = i + 1
		local p = planet
		Q2S_Record("PCAND", " i=" .. i ..
			" planet=" .. Q2S_Str(function() return p.Get_Type().Get_Name() end) ..
			" owner=" .. Q2S_Str(function() return p.Get_Owner().Get_Faction_Name() end) ..
			" slice=" .. Q2S_Str(function() return EvaluatePerception("Is_Good_Slice_Target", PlayerObject, p) end))
	end
end

-- The thread wrap is redone each time this chunk runs, around the function the shipped part above just defined; Flush_G
-- keeps functions, so it stays across releases.
Q2S_Thread = TechForce_Thread
function TechForce_Thread(...)
	Q2S_Try("PSTART", Q2S_Begin)
	Q2S_Try("PCAND", Q2S_Cands)
	local r = { Q2S_Thread(unpack(arg)) }
	Q2S_Try("PDONE", Q2S_Done)
	return unpack(r)
end

Q2S_Failed = TechForce_Production_Failed
if Q2S_Failed ~= nil then
	function TechForce_Production_Failed(...)
		Q2S_Try("PFAIL", Q2S_Fail, arg)
		return Q2S_Failed(unpack(arg))
	end
end

-- BlockOnCommand (PGBase) yields inside: the wrapper calls it outside any pcall. Wrapped once per script instance
-- (require does not run PGBase again).
if Q2S_Block == nil and BlockOnCommand ~= nil then
	Q2S_Block = BlockOnCommand
	function BlockOnCommand(...)
		Q2S_Try("PRE", Q2S_Pre, arg[1])
		local r = { Q2S_Block(unpack(arg)) }
		Q2S_Try("PBLOCK", Q2S_Post, r[1])
		return unpack(r)
	end
end

-- ScriptExit (PGBase): _ScriptExit() and, in a thread, coroutine.yield(false). Record first, then a tail call.
if Q2S_Exit == nil and ScriptExit ~= nil then
	Q2S_Exit = ScriptExit
	function ScriptExit()
		Q2S_Try("PEXIT", Q2S_Exiting)
		return Q2S_Exit()
	end
end
'''


def shipped(zip_path):
    """The shipped RebelAdvanceTechPlan.lua as LF text."""
    with zipfile.ZipFile(zip_path) as z:
        names = {n.upper(): n for n in z.namelist()}
        raw = z.read(names['DATA/SCRIPTS/AI/%s.LUA' % PLAN_FILE.upper()]).decode('latin-1')
    text = raw.replace('\r\n', '\n')
    for pat in (r'(?m)^function TechForce_Thread\(', r'Category = "Advance_Tech_Rebel"', r'LandUnits\(TechForce\)'):
        if not re.search(pat, text):
            raise SystemExit('shipped %s.lua does not match the hook (%s)' % (PLAN_FILE, pat))
    return text if text.endswith('\n') else text + '\n'


def plan_lua(zip_path=ZIP):
    return shipped(zip_path) + BLOCK.replace('@MOD@', MOD_NAME).replace('@CANARY@', CANARY)


def build(out, zip_path=ZIP):
    """Writes the mod folder (refuses to overwrite a folder without the marker)."""
    if os.path.exists(out):
        if not os.path.isfile(os.path.join(out, MARKER)):
            raise SystemExit('refusing to overwrite %s: no %s' % (out, MARKER))
        shutil.rmtree(out)
    ai = os.path.join(out, 'Data', 'Scripts', 'AI')
    os.makedirs(ai)
    with open(os.path.join(ai, PLAN_FILE + '.lua'), 'wb') as f:
        f.write(plan_lua(zip_path).encode('latin-1'))
    with open(os.path.join(out, CANARY), 'wb') as f:
        f.write(b'q2 rebel slice canary: _CustomScriptMessage deletes it when the hooked plan script loads\r\n')
    with open(os.path.join(out, MARKER), 'wb') as f:
        f.write(b'a2m t07 Rebel slice probe mod; removed by run_rebel_slice.ps1 (make_slice_mod.py --remove)\r\n')
    return out


def remove(out):
    """Removes the mod folder if it holds the marker. Returns a status word."""
    if not os.path.exists(out):
        return 'absent'
    if not os.path.isfile(os.path.join(out, MARKER)):
        return 'kept (no marker: not ours)'
    shutil.rmtree(out)
    return 'removed'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out')
    ap.add_argument('--remove')
    ap.add_argument('--print', action='store_true')
    ap.add_argument('--zip', default=ZIP)
    a = ap.parse_args()
    if a.print:
        sys.stdout.write(plan_lua(a.zip))
        return 0
    if a.remove:
        print('mod %s: %s' % (a.remove, remove(a.remove)))
        return 0
    if not a.out:
        ap.error('--out, --remove or --print')
    print('built %s' % build(a.out, a.zip))
    return 0


if __name__ == '__main__':
    sys.exit(main())
