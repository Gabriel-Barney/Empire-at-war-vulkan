#!/usr/bin/env python3
"""selftest_s78n_t14.py - offline check of the s78n t14 land-squad formation probe kit (no game is started, nothing
is installed).

  selftest_s78n_t14.py [--game DIR] [--source-zip extracted\\foc_data_text.zip] [--lua <lua50.exe>] [--keep]

1. Builds the mod into a temp folder: from the install when --game has one (read only: the corridor search reads
   the map), else from the text zip (--no-text, the fixed Geonosis corridor).
2. The built BusyTacticalFreeStore.lua is compiled by a stock Lua 5.0.2 (tools\\probe_mod\\out\\tools\\lua50.exe,
   built by ..\\common\\build_lua50.cmd on first use). The probe must not use the math library, '#' or '%'.
3. The probe (SN_CFG + s78n_t14_probe.lua, without the shipped free store) runs under a stubbed engine with a squad
   model after FIGHTER_FLYING sec. 7 - 7.3: Spawn_Unit of a company returns its team container, the members stand 25
   behind their slots at the drop, Move_To on the container draws per-member start delays (0..6) when it stands, the
   Rows / Dartboard layout of sec. 7.2 (Dartboard: Halton, seed id x frame; re-lay when the heading changes by more
   than acos 0.99), the container turns at 12 deg a frame then walks (accel / brake), members steer with the sec. 7
   speed rule and the side pull. Worlds:
     retail   member vmax 1.5 x the team speed, recovery to the slot while idle: Q1 TIGHT, CLOSES, catch-up YES,
              Q2 STAGGERED, Q3 Rows MATCH, Dartboard MATCH (Halton), re-lay on leg 3 only;
     port     member vmax = the team speed and no recovery (the port with a target nearby): Rows Q1 TRAILING,
              STAYS OPEN, catch-up NO;
     nodelay  as retail with Max_Move_Frame_Delay 0: Q2 SIMULTANEOUS;
     space    a space battle: SKIP only; menu: Empire not human: SKIP only.
   Each run's records go through s78n_t14_analyse.py.
Ends with S78N T14 SELFTEST OK, or lists the failures (exit 1).
"""
import argparse
import json
import re
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
import build_s78n_t14_mod as B  # noqa: E402
import s78n_t14_analyse as A  # noqa: E402

STUB = r'''
OUT = io.open(LOGFILE, "w")
NOW = 0
FRAME = 0
math.randomseed(SEED or 7)
function GetCurrentTime() return NOW end
function Game_Message(s) MSGS = (MSGS or "") .. s .. ";" end
function Point_Camera_At(p) if type(p) ~= "table" or p.x == nil then error("camera needs a position") end end
OBJS = {}
NEXT_ID = 41000
function PLAYER(name, human)
	local p = { name = name }
	p.Get_Faction_Name = function() return name end
	p.Is_Human = function() return human end
	return p
end
function Find_Player(n) return PLAYERS[string.upper(n)] end
function Create_Position(x, y, z) return { x = x, y = y, z = z } end
function TYPE(n)
	local t = { name = n }
	t.Get_Name = function() return string.upper(n) end
	return t
end
function Find_Object_Type(n) return TYPE(n) end
function NEWOBJ(tname, x, y, player)
	NEXT_ID = NEXT_ID + 1
	local o = { tname = tname, x = x, y = y, owner = player, id = NEXT_ID, valid = true, v = 0 }
	o.Is_Valid = function() return o.valid end
	o.Get_Object_ID = function() return o.id end
	o.Get_Type = function() return TYPE(o.tname) end
	o.Get_Owner = function() return o.owner end
	o.Get_Position = function() return { Get_XYZ = function() return o.x, o.y, 0 end } end
	o.Despawn = function() o.valid = false end
	o.Get_Parent_Object = function() return o.parent end
	for _, m in ipairs({ "Prevent_AI_Usage", "Prevent_All_Fire", "Make_Invulnerable" }) do
		o[m] = function(v) if type(v) ~= "boolean" then error(m .. " needs a bool") end end
	end
	o.Move_To = function(p)
		if type(p) ~= "table" or p.x == nil then error("Parameter 1 is not a valid destination") end
		if not o.valid then error("this object is already dead") end
		local k = o
		if o.parent then k = o.parent end
		ORDER(k, p)
		return {}
	end
	table.insert(OBJS, o)
	return o
end
function Find_All_Objects_Of_Type(n)
	local l = {}
	for _, o in ipairs(OBJS) do
		if o.valid and type(n) == "string" and string.upper(o.tname) == string.upper(n) then table.insert(l, o) end
	end
	return l
end
function Get_Game_Mode() return MODE end

-- ---- the squad model ----
COMPANIES = {
	Imperial_Medium_Stormtrooper_Squad = { team = "Stormtrooper_Team", member = "Squad_Stormtrooper", n = 9,
		arr = "Rows", cv = 0.8 * 1.57, cacc = 0.1, macc = 0.05, hx = 3, hy = 3, g = 0.3 },
	Rebel_Mini_Infantry_Squad = { team = "Rebel_Trooper_Team", member = "Squad_Rebel_Trooper", n = 5,
		arr = "Dartboard", cv = 1.1 * 1.57, cacc = 0.08, macc = 0.07, hx = 3, hy = 3, g = 0.5 },
}
function CEILSQRT(n)
	local s = math.floor(math.sqrt(n))
	if s * s == n then return s end
	return s + 1
end
function ROWS(n, hx, hy, g)
	local k = CEILSQRT(n)
	local G = g * 20
	local along, HX0, D = 0, nil, 0
	local out = {}
	local left = n
	while left > 0 do
		local r = left
		if r > k then r = k end
		left = left - r
		local W = 2 * hy
		if r > 1 then W = 2 * (G / 2 + 2 * hy) + (r - 2) * (2 * hy + G) end
		if HX0 == nil then HX0 = hx else along = along - (G / 2 + hx) end
		if left == 0 then D = HX0 - (along - hx) end
		local side = -W / 2
		for i = 1, r do
			if r == 1 then side = 0 elseif i == 1 then side = side + hy else side = side + G / 2 + hy end
			table.insert(out, { a = along, s = side })
			side = side + G / 2 + hy
		end
		along = along - (G / 2 + hx)
	end
	for _, p in ipairs(out) do p.a = p.a + D / 2 - HX0 end
	return out
end
function RADINV(i, b)
	local f, bk = 0, b
	while i > 0 do
		f = f + math.mod(i, b) / bk
		i = math.floor(i / b)
		bk = bk * b
	end
	return f
end
function DART(n, hx, hy, g, seed)
	local k = CEILSQRT(n)
	local m = math.floor(n / k)
	if math.mod(n, k) ~= 0 then m = m + 1 end
	local r = math.sqrt(hx * hx + hy * hy)
	local L = (k - 1) * g * 20 + k * hx
	local Wd = (m - 1) * g * 20 + m * hy
	local i2, i3 = seed, seed
	local pts = {}
	for t = 1, 1000 do
		if table.getn(pts) >= n then break end
		local h2 = RADINV(i2, 2)
		i2 = math.mod(i2 + 1, 4294967296)
		local h3 = RADINV(i3, 3)
		i3 = math.mod(i3 + 1, 4294967296)
		local x, y = (h2 - 0.5) * L, (h3 - 0.5) * Wd
		local ok = true
		for _, p in ipairs(pts) do
			if math.sqrt((y - p.s) * (y - p.s) + (x - p.a) * (x - p.a)) < 5 + r then ok = false end
		end
		if ok then table.insert(pts, { a = x, s = y }) end
	end
	return pts
end
-- slots by projection: the members sorted front-most first (rows: then by side), points front-most first
function LAYOUT(K, dx, dy)
	local c = K.sq
	local pts
	if c.arr == "Rows" then
		pts = ROWS(c.n, c.hx, c.hy, c.g)
	else
		pts = DART(c.n, c.hx, c.hy, c.g, math.mod(K.id * FRAME, 4294967296))
	end
	local ms = {}
	for _, m in ipairs(K.members) do
		m.proj = (m.x - K.x) * dx + (m.y - K.y) * dy
		m.sproj = -(m.x - K.x) * dy + (m.y - K.y) * dx
		table.insert(ms, m)
	end
	table.sort(ms, function(a, b)
		if math.abs(a.proj - b.proj) > 4 then return a.proj > b.proj end
		return a.sproj < b.sproj
	end)
	table.sort(pts, function(a, b)
		if math.abs(a.a - b.a) > 4 then return a.a > b.a end
		return a.s < b.s
	end)
	for i, m in ipairs(ms) do m.slot = pts[i] end
	K.laid = true
	K.ldx, K.ldy = dx, dy
end
function Spawn_Unit(ty, pos, player)
	local c = COMPANIES[ty.name]
	if c == nil then return {} end
	local K = NEWOBJ(c.team, pos.x, pos.y, player)
	K.sq = c
	K.fx, K.fy = math.cos(SPAWN_RAD), math.sin(SPAWN_RAD)
	K.state = "idle"
	K.members = {}
	local grid = ROWS(c.n, c.hx, c.hy, c.g)
	for i = 1, c.n do
		local g = grid[i]
		local a, s = g.a - DROP_BACK, g.s
		local m = NEWOBJ(c.member, pos.x + a * K.fx - s * K.fy, pos.y + a * K.fy + s * K.fx, player)
		m.parent = K
		m.delay = 0
		table.insert(K.members, m)
	end
	return { K }
end
function ORDER(K, p)
	local dx, dy = p.x - K.x, p.y - K.y
	local l = math.sqrt(dx * dx + dy * dy)
	if l < 0.01 then return end
	dx, dy = dx / l, dy / l
	K.tx, K.ty = p.x, p.y
	K.dx, K.dy = dx, dy
	if K.v == 0 then
		for _, m in ipairs(K.members) do m.delay = math.random(0, MAXD) end
	end
	if not K.laid or dx * K.ldx + dy * K.ldy <= 0.99 then LAYOUT(K, dx, dy) end
	K.state = "turn"
end
function TURNK(K)
	local cr = K.fx * K.dy - K.fy * K.dx
	local dt = K.fx * K.dx + K.fy * K.dy
	local ang = math.atan2(cr, dt)
	local step = math.rad(12)
	if math.abs(ang) <= step then
		K.fx, K.fy = K.dx, K.dy
		K.state = "move"
		return
	end
	if ang < 0 then step = -step end
	local c, s = math.cos(step), math.sin(step)
	K.fx, K.fy = K.fx * c - K.fy * s, K.fx * s + K.fy * c
end
function MOVEK(K)
	local c = K.sq
	local rx, ry = K.tx - K.x, K.ty - K.y
	local rem = math.sqrt(rx * rx + ry * ry)
	if rem <= K.v * K.v / (2 * c.cacc) + 0.01 then
		K.v = math.max(c.cacc, K.v - c.cacc)
	else
		K.v = math.min(c.cv, K.v + c.cacc)
	end
	if rem <= K.v then
		K.x, K.y = K.tx, K.ty
		K.v = 0
		K.state = "idle"
		return
	end
	K.x, K.y = K.x + K.fx * K.v, K.y + K.fy * K.v
end
function STEER(K, m)
	local c = K.sq
	local vmax = c.cv * VMAXF
	local dx, dy = m.x - K.x, m.y - K.y
	local ae = m.slot.a - (K.fx * dx + K.fy * dy)
	local se = m.slot.s - (-K.fy * dx + K.fx * dy)
	local vt = K.v
	if ae > 1 then
		local t = math.min(ae / 10, 1)
		vt = (vmax - K.v) * t + K.v
	elseif ae < -1 then
		local t = math.min(-ae / 10, 1)
		vt = (0 - K.v) * t * t + K.v
	end
	if K.state == "turn" then vt = 0 end
	if m.v < vt then m.v = math.min(vt, m.v + c.macc) else m.v = math.max(vt, m.v - c.macc) end
	local w = math.min(math.abs(se) / 10, 0.5)
	local px, py = -K.fy, K.fx
	if se <= 0 then px, py = K.fy, -K.fx end
	local hx, hy = K.fx + w * (px - K.fx), K.fy + w * (py - K.fy)
	local l = math.sqrt(hx * hx + hy * hy)
	m.x, m.y = m.x + hx / l * m.v, m.y + hy / l * m.v
end
function RECOVERM(K, m)
	local sx = K.x + m.slot.a * K.fx - m.slot.s * K.fy
	local sy = K.y + m.slot.a * K.fy + m.slot.s * K.fx
	local dx, dy = sx - m.x, sy - m.y
	local d = math.sqrt(dx * dx + dy * dy)
	m.v = 0
	if d > 4 or (m.rec and d > 0.5) then
		m.rec = true
		local st = math.min(d, K.sq.cv * VMAXF)
		m.x, m.y = m.x + dx / d * st, m.y + dy / d * st
	else
		m.rec = false
	end
end
function SQUADS(f)
	for _, K in ipairs(OBJS) do
		if K.valid and K.sq then
			if not K.laid then LAYOUT(K, K.fx, K.fy) end
			if K.state == "turn" then TURNK(K) elseif K.state == "move" then MOVEK(K) end
			for _, m in ipairs(K.members) do
				if m.valid then
					if K.state ~= "idle" then
						if m.delay > 0 then m.delay = m.delay - 1 else STEER(K, m) end
					elseif RECOVER then
						RECOVERM(K, m)
					end
				end
			end
		end
	end
end
-- shipped free store stand-ins
function Base_Definitions() ServiceRate = 20 end
function FreeStoreService() end
function On_Unit_Service(o) end
'''

LAND = r'''
MODE = "Land"
PLAYERS = { EMPIRE = PLAYER("Empire", HUMAN_EMPIRE), REBEL = PLAYER("Rebel", false) }
PlayerObject = PLAYERS.REBEL
function WORLD(f) SQUADS(f) end
'''
WORLDS = {
    'retail': 'VMAXF = 1.5\nRECOVER = true\nMAXD = 6\nDROP_BACK = 25\nHUMAN_EMPIRE = true\n' + LAND,
    'port': 'VMAXF = 1.0\nRECOVER = false\nMAXD = 6\nDROP_BACK = 25\nHUMAN_EMPIRE = true\n' + LAND,
    'nodelay': 'VMAXF = 1.5\nRECOVER = true\nMAXD = 0\nDROP_BACK = 25\nHUMAN_EMPIRE = true\n' + LAND,
    'space': 'VMAXF = 1.5\nRECOVER = true\nMAXD = 6\nDROP_BACK = 25\nHUMAN_EMPIRE = true\n' +
             LAND.replace('MODE = "Land"', 'MODE = "Space"'),
    'menu': 'VMAXF = 1.5\nRECOVER = true\nMAXD = 6\nDROP_BACK = 25\nHUMAN_EMPIRE = false\n' + LAND,
}

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
    work = tempfile.mkdtemp(prefix='s78n_t14_selftest_')
    have_game = os.path.isfile(os.path.join(a.game, 'corruption', 'StarWarsG.exe'))
    print('source:', 'install ' + a.game + ' (read only)' if have_game else 'text zip ' + a.source_zip)
    out = os.path.join(work, 'mod', B.MOD_NAME)
    cfg = B.build(a.game, out, None if have_game else a.source_zip, no_text=not have_game)
    print('built %s' % ', '.join(cfg['files']))
    print('\n'.join(B.describe(cfg)))
    for rel in cfg['files']:
        if rel.lower().endswith('.lua'):
            rc, msg = run_lua(lua, work, 'compile', ['assert(loadfile("%s"))' % lua_str(os.path.join(out, rel))])
            if rc != 0:
                fails.append('%s: Lua 5.0 compile failed: %s' % (rel, msg))
            else:
                print('compiled %s with stock Lua 5.0.2' % rel)
    if have_game and not any(r.lower().endswith('.dat') for r in cfg['files']):
        fails.append('the install build has no MasterTextFile_English.dat')
    xv = cfg['xml']
    if xv['rows']['arrangement'] != 'Rows' or xv['dart']['arrangement'] != 'Dartboard':
        fails.append('arrangements %s / %s, expected Rows / Dartboard' % (xv['rows']['arrangement'],
                                                                        xv['dart']['arrangement']))
    if xv['rows']['n'] != 9 or xv['dart']['n'] != 5:
        fails.append('company sizes %d / %d, expected 9 / 5' % (xv['rows']['n'], xv['dart']['n']))
    if (xv['rows']['hx'], xv['rows']['m_spacing'], xv['dart']['m_spacing']) != (3.0, 0.3, 0.5):
        fails.append('member half-extent / spacing %s / %s / %s, expected 3.0 / 0.3 / 0.5' % (
            xv['rows']['hx'], xv['rows']['m_spacing'], xv['dart']['m_spacing']))
    if cfg['est_frames'] > 5 * 1800:
        fails.append('estimated battle time %.1f minutes (> 5)' % (cfg['est_frames'] / 1800.0))
    probe = open(os.path.join(HERE, 's78n_t14_probe.lua'), encoding='ascii').read()
    code = '\n'.join(line.split('--')[0] for line in probe.splitlines())
    for bad in ('math.', '_MessagePopup', '#'):
        if bad in code:
            fails.append('s78n_t14_probe.lua uses %r (not available in retail free-store Lua)' % bad)
    if re.search(r'[\w)\]]\s*%\s*[\w(]', re.sub(r'"[^"]*"', '""', code)):
        fails.append('s78n_t14_probe.lua uses the % operator (Lua 5.1, not 5.0)')
    cfg_path = os.path.join(work, 'config.json')
    with open(cfg_path, 'w') as f:
        json.dump({k: v for k, v in cfg.items() if k != 'files'}, f, default=str)
    sp = cfg['sq']
    frames = cfg['est_frames'] + 1500
    results = {}
    for name in ('retail', 'port', 'nodelay', 'space', 'menu'):
        log = os.path.join(work, name + '.log')
        consts = 'SPAWN_RAD = %r\nSEED = 11\n' % (sp['theta0'] * 3.141592653589793 / 180.0)
        chunks = ['LOGFILE = "%s"' % lua_str(log), 'FRAMES = %d' % (frames if name in ('retail', 'port', 'nodelay')
                                                                    else 900),
                  consts, STUB, WORLDS[name], B.lua_config(cfg), probe, DRIVE]
        rc, msg = run_lua(lua, work, 'run_' + name, chunks)
        if rc != 0:
            fails.append('stub run %s failed: %s' % (name, msg[:600]))
            continue
        text, res = A.report(log, os.path.join(work, name + '_report.txt'), os.path.join(work, name + '_report.json'),
                             cfg_path)
        results[name] = (text, res)
        print('=' * 30, name, '=' * 30)
        print(text)

    def vv(name, key, field):
        return (results[name][1]['verdicts'].get(key) or {}).get(field)

    for name in ('retail', 'port', 'nodelay'):
        if name not in results:
            continue
        text, res = results[name]
        log = os.path.join(work, name + '.log')
        nerr = sum(1 for line in open(log) if line.startswith(('S8Q|ERR|', 'S8Q|APIERR|')))
        if nerr:
            fails.append('%s run recorded %d APIERR / ERR records' % (name, nerr))
        if not any(s['done'] for s in res['scripts']):
            fails.append('%s run did not reach DONE' % name)
        if len(res['trials']) != len(sp['order']):
            fails.append('%s: %d trials analysed, expected %d' % (name, len(res['trials']), len(sp['order'])))
        for t in res['trials']:
            legs = [L for L in t['legs'] if not L.get('missing')]
            if len(legs) != len(sp['legs']):
                fails.append('%s trial %s: %d legs analysed' % (name, t['k'], len(legs)))
            for L in legs:
                if L.get('dev_deg') is None or abs(L['dev_deg']) > 1.0:
                    fails.append('%s trial %s leg %s: heading %s deg off the plan' % (name, t['k'], L['leg'],
                                                                                    L.get('dev_deg')))
    want = {
        'retail': [('rows', 'Q1', 'TIGHT'), ('rows', 'Q1_close', 'CLOSES'), ('rows', 'catch_up', 'YES'),
                   ('rows', 'q1_ref', 'pred'), ('dart', 'Q1', 'TIGHT'), ('dart', 'Q1_close', 'CLOSES'),
                   ('dart', 'q1_ref', 'pred')],
        'port': [('rows', 'Q1', 'TRAILING'), ('rows', 'Q1_close', 'STAYS OPEN'), ('rows', 'catch_up', 'NO')],
        'nodelay': [('rows', 'Q2', 'SIMULTANEOUS'), ('dart', 'Q2', 'SIMULTANEOUS')],
    }
    for name, checks in want.items():
        if name not in results:
            continue
        for key, field, w in checks:
            got = vv(name, key, field)
            if got != w:
                fails.append('%s: %s %s = %s, expected %s' % (name, key, field, got, w))
    if 'retail' in results:
        v = results['retail'][1]['verdicts']
        for key in ('rows', 'dart'):
            if not str(v[key].get('Q2', '')).startswith('STAGGERED'):
                fails.append('retail: %s Q2 %s, expected STAGGERED' % (key, v[key].get('Q2')))
            if not str(v[key].get('Q3', '')).startswith('MATCH'):
                fails.append('retail: %s Q3 %s, expected MATCH' % (key, v[key].get('Q3')))
        rel = v['dart']['relay']
        for leg, lo, hi in ((2, None, 2.5), (3, 4.0, None), (4, None, 2.5)):
            for setc, _mem in rel.get(leg, []) or [(None, None)]:
                if setc is None or (lo is not None and setc < lo) or (hi is not None and setc > hi):
                    fails.append('retail: dart leg %d slot set change %s, expected %s' % (
                        leg, setc, '> %.1f (re-lay)' % lo if lo else '< %.1f (no re-lay)' % hi))
        if not str(v['dart'].get('relay_rule', '')).startswith('MATCH'):
            fails.append('retail: dart re-lay rule %s, expected MATCH' % v['dart'].get('relay_rule'))
        df = results['retail'][1].get('dart_fit') or {}
        if df.get('o') != 0 or not df.get('match'):
            fails.append('retail: Dartboard run fit offset %s match %s (stub: 0, True)' % (df.get('o'), df.get('match')))
        for h in v['dart'].get('halton', []):
            want_ev = 'leg 3 order' if h['leg'] >= 3 else 'spawn'
            if h['event'] != want_ev or abs(h['df']) > 1:
                fails.append('retail: trial %d leg %d laid out at %s %+d, expected %s +0 / +1' % (
                    h['k'], h['leg'], h['event'], h['df'], want_ev))
    for name in ('space', 'menu'):
        if name in results:
            res = results[name][1]
            if any(s['armed'] for s in res['scripts']) or not any(s['skip'] for s in res['scripts']):
                fails.append('%s run: expected SKIP and no ARM' % name)
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print('work folder kept:', work)
    if fails:
        print('S78N T14 SELFTEST FAILED:')
        for x in fails:
            print('  ' + x)
        return 1
    print('S78N T14 SELFTEST OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
