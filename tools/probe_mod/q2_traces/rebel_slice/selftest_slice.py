#!/usr/bin/env python3
r"""selftest_slice.py - offline self-test of the a2m t07 Rebel slice kit (no game).

  toolchain\venv\Scripts\python.exe -I tools\probe_mod\q2_traces\rebel_slice\selftest_slice.py [--lua PATH] [--out DIR]

Checks: the mod build / remove (marker rule); the hooked RebelAdvanceTechPlan.lua under stock Lua 5.0.2 with the real PG
library (PGBase BlockOnCommand / ScriptExit, PGTaskForce LandUnits) as a coroutine, three runs: a landing with a slice,
a pool release in between (the real PGBase Flush_G + Base_Definitions), a run after it, and a production failure;
the orders are the same as the unhooked shipped script's; math / debug removed; q2lua_reader.py parses every record;
the watcher on a fake process (credits, tech, unlocked-list vector); the report joins slice, price and credits to the
plan run and prints the The_Maw line. Ends with "SLICE SELFTEST OK (<n> checks, 0 failed)".
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(HERE)
PROBE_MOD = os.path.dirname(KIT)
sys.path.insert(0, HERE)
sys.path.insert(0, KIT)
sys.path.insert(0, os.path.join(PROBE_MOD, 'common'))
import make_slice_mod  # noqa: E402
import q2lua_reader  # noqa: E402
import slice_report  # noqa: E402
import slice_watch  # noqa: E402

RESULTS = []


def check(name, ok, detail=''):
    RESULTS.append((name, bool(ok)))
    print('%-70s %s%s' % (name, 'ok' if ok else 'FAILED', ('  ' + detail) if detail and not ok else ''))
    return ok


STUBS = r'''
Script = "Q2S_Test"
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
local A2_DBG = debug
function DumpCallStack() return A2_DBG.traceback() end
GlobalValue = { Set = function() end, Get = function() return nil end }
'''

DRIVER = r'''
math = nil
debug = nil
Base_Definitions()
function A2_FR() return tonumber(string.format("%d", A2_T * 30 + 0.5)) end
A2_LOG = {}
local function log(s) table.insert(A2_LOG, s .. " @" .. A2_FR()) end
A2_CR = 5000
local function ty(n) return { Get_Name = function() return n end } end
local function player(n) return { Get_Faction_Name = function() return n end, Get_Credits = function() return A2_CR end,
	Get_Tech_Level = function() return A2_TECH end } end
A2_TECH = 2
A2_PLAYERS = { REBEL = player("REBEL"), EMPIRE = player("EMPIRE"), UNDERWORLD = player("UNDERWORLD") }
local function planet(n, owner)
	return { Get_Type = function() return ty(string.upper(n)) end, Get_Owner = function() return A2_PLAYERS[owner] end }
end
local function block(done, result, on_done)
	return { IsFinished = function()
			if A2_FR() >= done then
				if on_done then on_done() on_done = nil end
				return true
			end
			return false
		end, Result = function() return result end }
end
local function engine_setup(f0, planet_name, owner)
	-- engine bindings (set again per run, as FindTarget: a release flushes the script's globals)
	FindPlanet = { Get_All_Planets = function()
		return { planet("Yavin", "REBEL"), planet("Corellia", "EMPIRE"), planet("Shola", "UNDERWORLD") } end }
	EvaluatePerception = function(eq, pl, p)
		if eq ~= "Is_Good_Slice_Target" then return nil end
		if p.Get_Type().Get_Name() == "CORELLIA" then return 0 end
		return 1
	end
	PlayerObject = A2_PLAYERS.REBEL
	Budget = { Get_Spendable_Resources = function() return 0 end, Get_Unallocated_Resources = function() return 0 end }
	FindTarget = function(tf, eq, kind, v)
		log("FindTarget " .. eq)
		return { Get_Game_Object = function() return planet(planet_name, owner) end }
	end
	A2_COUNT = 1
	TechForce = {
		Set_As_Goal_System_Removable = function(b) log("Removable " .. tostring(b)) end,
		Produce_Force = function(t) log("Produce_Force " .. t.Get_Game_Object().Get_Type().Get_Name())
			if A2_PROD_FAIL then return nil end return block(f0 + 60, true) end,
		Move_To = function(t) log("Move_To " .. t.Get_Game_Object().Get_Type().Get_Name()) return block(f0 + 300, true) end,
		Land_Units = function() log("Land_Units") return block(f0 + 330, true, function()
			if A2_SLICE_PRICE then A2_CR = A2_CR - A2_SLICE_PRICE A2_COUNT = 0 end end) end,
		Set_Plan_Result = function(r) log("Set_Plan_Result " .. tostring(r)) end,
		Get_Force_Count = function() return A2_COUNT end,
	}
end
_ScriptExit = function() log("ScriptExit") end
GetThreadID = function() return 0 end
local function run_thread(f0, fail_at)
	local co = coroutine.create(TechForce_Thread)
	for frame = f0, f0 + 3000 do
		A2_T = frame / 30
		A2_CUR = co
		if fail_at ~= nil and frame == fail_at then
			GetThreadID = function() return -1 end
			TechForce_Production_Failed(TechForce, ty("DROIDS_TEAM"))
			GetThreadID = function() return 0 end
			A2_OUT:write("FAILED_AT " .. frame .. "\n")
			return
		end
		local ok, v = coroutine.resume(co)
		if not ok then A2_OUT:write("THREAD_ERROR " .. tostring(v) .. "\n") return end
		if coroutine.status(co) == "dead" or v == false then return end
	end
end
local function release()
	local keep = {}
	for k, v in pairs(_G) do
		if (type(v) == "table" or type(v) == "userdata") and (string.find(k, "^A2_") or k == "ThreadValue" or
				k == "GetEvent" or k == "GlobalValue") then
			keep[k] = v
		end
	end
	for k, _ in pairs(keep) do _G[k] = nil end
	Flush_G()
	for k, v in pairs(keep) do _G[k] = v end
	Base_Definitions()
	A2_OUT:write("RELEASED\n")
end
-- run 1: Shola (Empire), the landing slices (price 1200)
engine_setup(120, "Shola", "EMPIRE")
A2_SLICE_PRICE = 1200
run_thread(120)
if A2_HOOKED then release() end
-- run 2: The_Maw (Underworld), no slice
engine_setup(2000, "The_Maw", "UNDERWORLD")
A2_SLICE_PRICE = nil
run_thread(2000)
if A2_HOOKED then release() end
-- run 3: production fails at 4030
engine_setup(4000, "Corellia", "EMPIRE")
A2_PROD_FAIL = nil
run_thread(4000, 4030)
for i = 1, table.getn(A2_LOG) do A2_OUT:write("LOG " .. A2_LOG[i] .. "\n") end
if Q2S_Count ~= nil then for i = 1, Q2S_Count() do A2_OUT:write(tostring(Q2S_Rec(i)) .. "\n") end end
A2_OUT:close()
'''


def extract(z, dst):
    lib = os.path.join(dst, 'library')
    os.makedirs(lib, exist_ok=True)
    for info in z.infolist():
        p = info.filename.replace('\\', '/').upper()
        if '/SCRIPTS/LIBRARY/' in p and p.endswith('.LUA'):
            with open(os.path.join(lib, os.path.basename(info.filename)), 'wb') as f:
                f.write(z.read(info))
    return lib


def lua_case(lua, lib, out, name, body, hooked):
    outfile = os.path.join(out, name + '.txt')
    script = os.path.join(out, name + '.lua')
    with open(script, 'w', encoding='latin-1', newline='\n') as f:
        f.write('A2_OUTFILE = "%s"\n' % outfile.replace('\\', '\\\\'))
        f.write(STUBS)
        f.write(body + '\n')
        f.write('A2_HOOKED = %s\n' % ('true' if hooked else 'false'))
        f.write(DRIVER)
    env = dict(os.environ, LUA_PATH=os.path.join(lib, '?.lua'))
    r = subprocess.run([lua, script], capture_output=True, text=True, env=env)
    o = open(outfile).read() if os.path.exists(outfile) else ''
    return r.returncode, r.stdout + r.stderr, o


def lua_tests(lua, out, z):
    lib = extract(z, out)
    body = make_slice_mod.plan_lua()
    shipped = make_slice_mod.shipped(make_slice_mod.ZIP)
    rc, text, o = lua_case(lua, lib, out, 'hooked', body, True)
    rc0, text0, o0 = lua_case(lua, lib, out, 'shipped', shipped, False)
    check('lua: hooked plan runs under the real PG library, no THREAD_ERROR', rc == 0 and 'THREAD_ERROR' not in o and
          'LOG Land_Units' in o and o.count('RELEASED') == 2, (text + o)[:900])
    log = [ln for ln in o.splitlines() if ln.startswith('LOG ')]
    log0 = [ln for ln in o0.splitlines() if ln.startswith('LOG ')]
    check('lua: same orders at the same frames as the unhooked shipped script (%d)' % len(log0),
          rc0 == 0 and bool(log0) and log == log0, str(log) + ' vs ' + str(log0) + text0[:300])
    check('lua: the shipped plan ran after each release (3 FindTarget, 2 landings, the failure ends run 3)',
          sum(1 for ln in log if ln.startswith('LOG FindTarget')) == 3 and
          sum(1 for ln in log if ln.startswith('LOG Land_Units')) == 2 and 'FAILED_AT 4030' in o, str(log))
    recs = [ln.split('|') for ln in o.splitlines() if ln.startswith('Q2L1|')]
    seqs = [int(r[3]) for r in recs]
    check('lua: records 1..N of one sid kept across both releases', bool(recs) and seqs == list(range(1, len(recs) + 1))
          and len({r[2] for r in recs}) == 1, str(seqs))
    kinds = [(r[4], re.search(r' n=(\d+)', r[5]).group(1) if re.search(r' n=(\d+)', r[5]) else '') for r in recs]
    exp1 = [('PSTART', '1'), ('PTARGET', '1'), ('PPROD', '1'), ('PMOVE', '1'), ('PLANDPRE', '1'), ('PLAND', '1'),
            ('PLANDX', '1'), ('PEXIT', '1')]
    check('lua: run 1 records PSTART PTARGET PPROD PMOVE PLANDPRE PLAND PLANDX PEXIT', all(k in kinds for k in exp1),
          str(kinds))
    txt = {(r[4], re.search(r' n=(\d+)', r[5]).group(1) if re.search(r' n=(\d+)', r[5]) else ''): r[5] for r in recs}
    check('lua: PTARGET target / owner from Target.Get_Game_Object(); PLANDPRE / PLAND credits around the slice',
          'target=SHOLA owner=EMPIRE blk=table' in txt.get(('PTARGET', '1'), '') and
          ' f=421 ' in txt.get(('PLANDPRE', '1'), '') and ' cr=5000 ' in txt.get(('PLANDPRE', '1'), '') and
          ' k=3 ret=true ' in txt.get(('PLAND', '1'), '') and ' cr=3800 ' in txt.get(('PLAND', '1'), '') and
          ' count=0' in txt.get(('PLAND', '1'), ''), str(txt.get(('PLANDPRE', '1'))) + ' / ' + str(txt.get(('PLAND', '1'))))
    check('lua: run 2 after the release: PSTART n=2 prev=PEXIT, target THE_MAW, credits unchanged at PLAND',
          ' prev=PEXIT ' in txt.get(('PSTART', '2'), '') and 'target=THE_MAW owner=UNDERWORLD' in
          txt.get(('PTARGET', '2'), '') and ' cr=3800 ' in txt.get(('PLAND', '2'), '') and
          ' count=1' in txt.get(('PLAND', '2'), ''), str(txt.get(('PSTART', '2'))))
    check('lua: run 3: PFAIL then PEXIT (production failure) after the second release',
          ('PFAIL', '3') in kinds and ('PEXIT', '3') in kinds and 'args=2 DROIDS_TEAM' in txt.get(('PFAIL', '3'), ''),
          str(kinds[-6:]))
    pc = [r[5] for r in recs if r[4] == 'PCAND']
    check('lua: PCAND (a2m r2 t14.4): 3 planets per run in list order, owner and Is_Good_Slice_Target, before PTARGET',
          len(pc) == 9 and 'i=1 planet=YAVIN owner=REBEL slice=1' in pc[0] and
          'i=2 planet=CORELLIA owner=EMPIRE slice=0' in pc[1] and 'i=3 planet=SHOLA owner=UNDERWORLD slice=1' in pc[2]
          and [r[4] for r in recs].index('PCAND') < [r[4] for r in recs].index('PTARGET'), str(pc[:3]))
    check('lua: canary called once, no ERR record', o.count('CANARYCALL Mods\\Q2RebelSlice\\Q2RebelSlice_Canary_Plan.txt')
          == 1 and not any(r[4] == 'ERR' for r in recs), str([r[5] for r in recs if r[4] == 'ERR']))
    hits = [(0, ln.encode('ascii')) for ln in o.splitlines() if ln.startswith('Q2L1|')]
    parsed = q2lua_reader.parse_hits(hits)
    check('q2lua_reader: every record matches its RECORD_RE (src P, kind <= 12 letters)', len(parsed) == len(recs),
          '%d of %d' % (len(parsed), len(recs)))
    return [(p[0], p[1], p[2], p[3], p[4]) for p in parsed]


class FakeProc:
    def __init__(self):
        self.mem = {}

    def put(self, addr, data):
        for i, b in enumerate(data):
            self.mem[addr + i] = b

    def read(self, addr, n):
        try:
            return bytes(self.mem[addr + i] for i in range(n))
        except KeyError:
            return None

    def u64(self, addr):
        b = self.read(addr, 8)
        return struct.unpack('<Q', b)[0] if b else None


class FakeGame:
    def __init__(self, proc):
        self.p = proc
        self.mf = 0

    def mode(self):
        return {'frame': self.mf, 'gframe': self.mf + 100, 'type': 0}

    def players(self):
        return {'by_id': [{'addr': 0x100000, 'faction': 'REBEL', 'human': 0, 'id': 0},
                          {'addr': 0x200000, 'faction': 'EMPIRE', 'human': 1, 'id': 1}]}

    def type_name(self, tp):
        return {0x500000: 'R_GROUND_LIGHT_VEHICLE_FACTORY', 0x500100: 'REBEL_X_WING_SQUADRON',
                0x500200: 'BTL_Y_WING_SQUADRON'}.get(tp, '?')


def watcher_and_report_tests(out, parsed):
    p = FakeProc()
    g = FakeGame(p)

    def player(addr, cr, tech, unlocked):
        blk = bytearray(slice_watch.P_READ)
        struct.pack_into('<f', blk, slice_watch.P_CREDITS, cr)
        struct.pack_into('<ii', blk, slice_watch.P_TECH, tech, 5)
        vec = addr + 0x10000
        struct.pack_into('<Qii', blk, 0x1B0, vec, len(unlocked), 8)   # DynamicVector {data, count, capacity}
        p.put(addr, bytes(blk))
        p.put(vec, b''.join(struct.pack('<Q', t) for t in unlocked) + b'\0' * 64)
    wpath = os.path.join(out, 'slice_watch.jsonl')
    with open(wpath, 'w') as f:
        clock = [0.0]
        w = slice_watch.Watcher(p, g, f, os.path.join(out, 'slice_live.json'), clock=lambda: clock[0])
        player(0x200000, 9000, 1, [])
        steps = [(0, 5000, 2, [0x500000]), (100, 5000, 2, [0x500000]),
                 (449, 3800, 2, [0x500000]), (449, 3800, 2, [0x500000, 0x500100]),   # the slice (credits first)
                 (2330, 3800, 2, [0x500000, 0x500100]), (3000, 4500, 2, [0x500000, 0x500100])]
        w.resolve()
        for mf, cr, tech, ul in steps:
            g.mf = mf
            clock[0] += 1.5
            player(0x100000, cr, tech, ul)
            w.poll()
    ev = slice_watch.load(wpath)
    adds = [e for e in ev if e.get('ev') == 'list' and e.get('fac') == 'REBEL' and e.get('added')]
    check('watcher: unlocked vector decoded, the slice seen as one addition with its frame',
          len(adds) == 2 and adds[1]['added'] == ['REBEL_X_WING_SQUADRON'] and adds[1]['mf'] == 449 and adds[1]['ok'],
          str(adds))
    crs = [e for e in ev if e.get('ev') == 'cr' and e.get('fac') == 'REBEL']
    check('watcher: credit changes with prev (5000 -> 3800 at 449)', any(e['mf'] == 449 and e['prev'] == 5000 and
                                                                         e['cr'] == 3800 for e in crs), str(crs))
    live = json.load(open(os.path.join(out, 'slice_live.json')))
    check('watcher: slice_live.json has the mode frame and the players', 'mode_frame' in live and 'REBEL' in
          live.get('players', {}), str(live))
    with open(os.path.join(out, 'lua_records.txt'), 'w') as f:
        for src, sid, seq, kind, text in parsed:
            f.write('%s %s %d %s %s\n' % (src, sid, seq, kind, text))
    res, text = slice_report.report(out)
    check('report: run 1 -> REBEL_X_WING_SQUADRON price 1200 cr_before 5000 at f=449',
          re.search(r'\n\s+1\s+120\s+SHOLA\s+EMPIRE .*REBEL_X_WING_SQUADRON price=1200.00 cr_before=5000.00 f=449',
                    text) is not None, text[:2500])
    check('report: run 2 (THE_MAW) landed without a slice; The_Maw line "never slices"',
          re.search(r'\n\s+2\s+2000\s+THE_MAW\s+UNDERWORLD .* none\n', text) is not None and
          'The_Maw: 1 run(s), 1 landed, 0 sliced -> never slices' in text, text[:2500])
    check('report: the initial unlocked entry is listed as not from a plan run; slice_plans.tsv one row per run',
          '(no plan run landing at that frame' in text and
          len(open(os.path.join(out, 'slice_plans.tsv')).read().splitlines()) == 4, text[-900:])
    check('report: section 4 lists the PCAND candidates per run and the enemy planets scoring > 0 (a2m r2 t14.4)',
          'run 1   target SHOLA        | YAVIN:REB=1 CORELLIA:EMP=0 SHOLA:UND=1' in text and
          'enemy planets scoring > 0 (picks / runs): SHOLA 1/3' in text and 'never picked: none' in text,
          text[text.find('== 4.'):][:700])
    # Raw bytes of the real run 20261108-212955 (FoC 1.121): DynamicVector {data, i32 count, i32 capacity}.
    real = bytes.fromhex('b00076d3710100000b00000014000000d8592bb2f77f0000')
    check('decode_raw: the retail unlocked list is a DynamicVector (count 11, capacity 20)',
          slice_watch.decode_raw(real) == ('dv', 0x171d37600b0, 11), str(slice_watch.decode_raw(real)))
    check('report: target keys ignore the hook\'s obj: prefix', slice_report.tgt_key('obj:THE_MAW') == 'THE_MAW' and
          slice_report.tgt_key('The_Maw') == 'THE_MAW')
    cdir = os.path.join(out, 'count_only')
    os.makedirs(cdir, exist_ok=True)
    with open(os.path.join(cdir, 'slice_watch.jsonl'), 'w') as f:
        for mf, cr, prev, n in ((1, 5000.0, None, 11), (448, 4996.0, 5000.0, None), (449, 4292.0, 4996.0, 12)):
            f.write(json.dumps({'ev': 'cr', 'fac': 'REBEL', 'mf': mf, 'mt': 0, 'cr': cr, 'prev': prev}) + '\n')
            if n is not None:
                raw = struct.pack('<Qii', 0x171d37600b0, n, 20) + b'\0' * 8
                f.write(json.dumps({'ev': 'list', 'fac': 'REBEL', 'which': 'unlocked', 'mf': mf, 'mt': 0, 'ok': False,
                                    'added': [], 'raw': raw.hex()}) + '\n')
    _crs, ul, _t, (okl, _bad), bases = slice_report.watch_events(os.path.join(cdir, 'slice_watch.jsonl'), 'REBEL')
    check('report: count-only decode (old watcher) gives #12 at f=449, price 700 net of the 4/frame drain',
          okl and bases == [(1, 11)] and len(ul) == 1 and ul[0]['type'] == '#12' and ul[0]['mf'] == 449 and
          ul[0]['price'] == 700.0, str(ul))


# a2m t13: every distinct list raw of run 20261108-212955 (slice_watch.jsonl, old watcher: all ok=false or empty) and
# what the fixed decode_raw() makes of it. Rows 2-3 were read through a stale player address at the mode change.
REAL_RAWS = (
    ('00000000000000000000000000000000d8592bb2f77f0000', ('dv', 0, 0)),                 # empty list (all players, f3)
    ('ca03683f6a61d83e742f373300b810401d64bbbe45ee483f', (None, None, None)),           # stale address: floats
    ('f7ffff3e5eab08406161583ec303e8bedab35d3f8184ee3f', (None, None, None)),
    ('0000803f22a111b24825f53100000000d1b3dd320000803f', (None, None, None)),
    ('000080b3000080b300008028000000000000803f00000033', (None, None, None)),
    ('b00076d3710100000b00000014000000d8592bb2f77f0000', ('dv', 0x171d37600b0, 11)),    # REBEL unlocked at f1
    ('404e95c971010000010000000a000000d8592bb2f77f0000', ('dv', 0x171c9954e40, 1)),     # REBEL locked
    ('605895c971010000010000000a000000d8592bb2f77f0000', ('dv', 0x171c9955860, 1)),     # EMPIRE unlocked
    ('b00076d3710100000c00000014000000d8592bb2f77f0000', ('dv', 0x171d37600b0, 12)),    # slice f349
    ('b00076d3710100000d00000014000000d8592bb2f77f0000', ('dv', 0x171d37600b0, 13)),    # slice f3185
    ('b00076d3710100000e00000014000000d8592bb2f77f0000', ('dv', 0x171d37600b0, 14)),    # slice f6271
    ('b00076d3710100000f00000014000000d8592bb2f77f0000', ('dv', 0x171d37600b0, 15)),    # slice f9200
    ('10952ed571010000010000000a000000d8592bb2f77f0000', ('dv', 0x171d52e9510, 1)),     # UNDERWORLD unlocked f11087
    ('b00076d3710100001000000014000000d8592bb2f77f0000', ('dv', 0x171d37600b0, 16)),    # slice f11950
)


def decode_tests():
    """No Lua needed: the fixed decoder on the real run's raw bytes, and the watcher naming entries from them."""
    bad = [(raw, slice_watch.decode_raw(bytes.fromhex(raw)), exp) for raw, exp in REAL_RAWS
           if slice_watch.decode_raw(bytes.fromhex(raw)) != exp]
    check('decode_raw: all %d list raws of run 20261108-212955 (counts 11..16, stale reads rejected)' % len(REAL_RAWS),
          not bad, str(bad))
    p = FakeProc()
    data = 0x171d37600b0
    p.put(data, b''.join(struct.pack('<Q', t) for t in (0x500000, 0x500100, 0x500200)))
    w = slice_watch.Watcher(p, FakeGame(p), open(os.devnull, 'w'))
    ok, names = w.decode_list(bytes.fromhex('b00076d3710100000300000014000000d8592bb2f77f0000'))
    check('watcher: a retail-layout DynamicVector raw is named entry by entry (type_name of each T*)',
          ok and names == ['R_GROUND_LIGHT_VEHICLE_FACTORY', 'REBEL_X_WING_SQUADRON', 'BTL_Y_WING_SQUADRON'], str((ok, names)))


def build_tests(out):
    mod = os.path.join(out, 'mod', make_slice_mod.MOD_NAME)
    make_slice_mod.build(mod)
    lua = os.path.join(mod, 'Data', 'Scripts', 'AI', 'RebelAdvanceTechPlan.lua')
    body = open(lua, encoding='latin-1').read()
    check('build: plan script = shipped + probe block, canary and marker files',
          os.path.isfile(os.path.join(mod, make_slice_mod.CANARY)) and
          os.path.isfile(os.path.join(mod, make_slice_mod.MARKER)) and body.startswith('-- $Id: ') and
          'function TechForce_Thread()' in body and 'Q2S_Say' in body)
    check('build: no math, no #, no needle literal in the probe block',
          'math.' not in make_slice_mod.BLOCK and '#' not in make_slice_mod.BLOCK and
          'Q2L1|' not in make_slice_mod.BLOCK and 'string.rep' not in make_slice_mod.BLOCK)
    other = os.path.join(out, 'mod', 'NotOurs')
    os.makedirs(other, exist_ok=True)
    check('remove: refuses a folder without the marker, removes ours',
          make_slice_mod.remove(other).startswith('kept') and make_slice_mod.remove(mod) == 'removed' and
          not os.path.exists(mod))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--lua')
    ap.add_argument('--out')
    a = ap.parse_args()
    out = a.out or tempfile.mkdtemp(prefix='slice_selftest_')
    os.makedirs(out, exist_ok=True)
    if not os.path.isfile(make_slice_mod.ZIP):
        print('extracted/foc_data_text.zip not found: cannot self-test')
        return 1
    build_tests(out)
    decode_tests()
    lua = a.lua
    if not lua:
        import lua50
        lua = lua50.find_lua(build=False)
    if lua:
        with zipfile.ZipFile(make_slice_mod.ZIP) as z:
            parsed = lua_tests(lua, out, z)
        watcher_and_report_tests(out, parsed)
    else:
        print('SKIPPED the Lua checks: no lua50.exe (tools\\probe_mod\\common\\build_lua50.cmd builds one)')
    bad = [n for n, ok in RESULTS if not ok]
    print('SLICE SELFTEST %s (%d checks, %d failed) output: %s' % ('OK' if not bad else 'FAILED', len(RESULTS), len(bad),
                                                                  out))
    return 0 if not bad else 1


if __name__ == '__main__':
    sys.exit(main())
