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
   Rows / Dartboard layout of sec. 7.2 (Dartboard: float Halton seeded id x NOW, laid at the container's next service
   after an order, retail slot order; re-lay when the heading changes by more than acos 0.99), the container turns at
   12 deg a frame then walks with the 7.7 acceleration (member OverrideDeceleration x 1.56) and straggler cap, members
   steer with the sec. 7 speed rule and the side pull. NOW (GetCurrentTime.Frame) runs 1234 frames ahead of the mode
   clock. Also checked: the mod's XML patch (Squad_Rebel_Trooper jiggle / raggedness 0) and the analyser's float32
   maths. Worlds:
     retail   members at their own Max_Speed, recovery to the slot while idle: Q1 TIGHT, CLOSES, Q2 STAGGERED, Q3 Rows
              MATCH, Dartboard MATCH on every leg at NOW + 1, retail slot order and re-lay rule MATCH, 7.7 MATCH;
     port     member vmax = the team speed, no recovery, no cap, the container's own acceleration: Rows Q1 TRAILING,
              STAYS OPEN, catch-up NO, 7.7 acceleration DIFFERS;
     nodelay  as retail with Max_Move_Frame_Delay 0: Q2 SIMULTANEOUS;
     badseed  Dartboard seeded id x (NOW + 777): no Dartboard leg matches;
     noframe  no GetCurrentTime.Frame: one APIERR, the run goes on, Dartboard n/a, Rows 7.7 acceleration MATCH;
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
-- GetCurrentTime() = the mode frame / 30; GetCurrentTime.Frame() = the global frame NOW / 30 = (FRAME + GOFF) / 30
if FRAMEAPI then
	GetCurrentTime = setmetatable({}, { __call = function() return NOW end })
	GetCurrentTime.Frame = function() return (FRAME + GOFF) / 30 end
else
	function GetCurrentTime() return NOW end
end
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
-- cv = team speed min(container, member Max_Speed) x 1.56; mv = the member's own 1.0 x 1.56 (FIGHTER_FLYING 7 step 4);
-- cacc = the member OverrideDeceleration x 1.56 (7.7 item 1); pacc = the container type's own acceleration (the port)
COMPANIES = {
	Imperial_Medium_Stormtrooper_Squad = { team = "Stormtrooper_Team", member = "Squad_Stormtrooper", n = 9,
		arr = "Rows", cv = 0.8 * 1.56, mv = 1.56, cacc = 0.05 * 1.56, pacc = 0.1, macc = 0.05, hx = 3, hy = 3, g = 0.3 },
	Rebel_Mini_Infantry_Squad = { team = "Rebel_Trooper_Team", member = "Squad_Rebel_Trooper", n = 5,
		arr = "Dartboard", cv = 1.0 * 1.56, mv = 1.56, cacc = 0.07 * 1.56, pacc = 0.08, macc = 0.07, hx = 3, hy = 3,
		g = 0.5 },
}
function VMAX(c)
	if OWNV then return c.mv end
	return c.cv
end
function CACC(c)
	if PORTACC then return c.pacc end
	return c.cacc
end
-- stable insertion sort of a[lo..hi], gt(x, y) = x before y (MSVC std::sort on <= 32 elements)
function ISORT(a, lo, hi, gt)
	for i = lo + 1, hi do
		local v = a[i]
		local j = i - 1
		while j >= lo and gt(v, a[j]) do
			a[j + 1] = a[j]
			j = j - 1
		end
		a[j + 1] = v
	end
end
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
-- FIGHTER_FLYING 7.2 Dartboard (in double here; the analyser fits in float32): the accepted points, Wd, m
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
	return pts, Wd, m
end
-- the layout at the container's next service (7.2 callers 1 / 2) with that frame's NOW
function LAYOUT(K, dx, dy)
	local c = K.sq
	local n = c.n
	for _, m in ipairs(K.members) do
		m.proj = (m.x - K.x) * dx + (m.y - K.y) * dy
		m.sproj = -(m.x - K.x) * dy + (m.y - K.y) * dx
	end
	local pts, Wd, mm = nil, nil, nil
	if c.arr ~= "Rows" then
		pts, Wd, mm = DART(n, c.hx, c.hy, c.g, math.mod(K.id * (FRAME + GOFF + SEEDOFF), 4294967296))
		if table.getn(pts) < n then pts = nil end
	end
	if pts == nil then
		-- Rows: members front-most first then by side, points the same (the layout's own rule is not tested here)
		pts = ROWS(n, c.hx, c.hy, c.g)
		local ms = {}
		for _, m in ipairs(K.members) do table.insert(ms, m) end
		table.sort(ms, function(a, b)
			if math.abs(a.proj - b.proj) > 4 then return a.proj > b.proj end
			return a.sproj < b.sproj
		end)
		table.sort(pts, function(a, b)
			if math.abs(a.a - b.a) > 4 then return a.a > b.a end
			return a.s < b.s
		end)
		for i, m in ipairs(ms) do m.slot = pts[i] end
	else
		-- 7.2 Dartboard step 6: points by x descending, members by projection descending; groups with the
		-- i = j + 1 skip, each sorted by side / y descending; member q gets point q
		local P, M = {}, {}
		for q = 1, n do P[q] = pts[q] end
		for q = 1, n do M[q] = K.members[q] end
		ISORT(P, 1, n, function(a, b) return a.a > b.a end)
		ISORT(M, 1, n, function(a, b) return a.proj > b.proj end)
		local lim = Wd / mm
		local i = 1
		repeat
			local j = i
			while j <= n and P[i].a - P[j].a <= lim do j = j + 1 end
			ISORT(M, i, j - 1, function(a, b) return a.sproj > b.sproj end)
			ISORT(P, i, j - 1, function(a, b) return a.s > b.s end)
			i = j + 1
		until i > n
		for q = 1, n do M[q].slot = P[q] end
	end
	-- 7.7 item 2: the joint slot box half-length (box seeded with 0) + the container's MaxJiggleDistance (0)
	local hi, lo = 0, 0
	for _, m in ipairs(K.members) do
		hi = math.max(hi, m.slot.a + c.hx)
		lo = math.min(lo, m.slot.a - c.hx)
	end
	K.hxbox = (hi - lo) / 2
	K.laid = true
	K.ldx, K.ldy = dx, dy
	LAYOUTS = (LAYOUTS or 0) + 1
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
	K.brk = nil
	K.dx, K.dy = dx, dy
	if K.v == 0 then
		for _, m in ipairs(K.members) do m.delay = math.random(0, MAXD) end
	end
	if not K.laid or dx * K.ldx + dy * K.ldy <= 0.99 then
		K.pend, K.pdx, K.pdy = true, dx, dy
	end
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
	local a = CACC(c)
	local rx, ry = K.tx - K.x, K.ty - K.y
	local rem = math.sqrt(rx * rx + ry * ry)
	-- 7.7 item 2: the straggler cap from the members' start-of-frame positions along the container's facing
	local T = c.cv
	if CAP then
		local lim = -(K.hxbox + 2)
		for _, m in ipairs(K.members) do
			local p = K.fx * (m.x - K.x) + K.fy * (m.y - K.y)
			if p < lim then
				T = math.min(T, (1 - math.min((lim - p) / 10, 1)) * c.cv)
			end
		end
	end
	-- braking latches once it starts (a constant-rate ramp to the plateau at v = a, 7.7 item 3)
	if K.brk or rem <= K.v * K.v / (2 * a) + 0.01 then
		K.brk = true
		K.v = math.max(a, K.v - a)
	elseif T > K.v then
		K.v = K.v + math.min(a, T - K.v)
	else
		K.v = K.v - math.min(a, K.v - T)
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
	local vmax = VMAX(c)
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
		local st = math.min(d, VMAX(K.sq))
		m.x, m.y = m.x + dx / d * st, m.y + dy / d * st
	else
		m.rec = false
	end
end
function SQUADS(f)
	for _, K in ipairs(OBJS) do
		if K.valid and K.sq then
			if not K.laid then LAYOUT(K, K.fx, K.fy) end
			if K.pend then
				K.pend = false
				LAYOUT(K, K.pdx, K.pdy)
			end
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
RETAIL = dict(OWNV='true', RECOVER='true', MAXD='6', DROP_BACK='25', HUMAN_EMPIRE='true', CAP='true',
              PORTACC='false', GOFF='1234', SEEDOFF='0', FRAMEAPI='true')


def world(land=LAND, **kw):
    """The world flags (RETAIL with the overrides) as Lua globals, then the land setup. They go before the stub."""
    flags = dict(RETAIL, **{k: str(v) for k, v in kw.items()})
    return ''.join('%s = %s\n' % kv for kv in sorted(flags.items())), land


WORLDS = {
    'retail': world(),
    'port': world(OWNV='false', RECOVER='false', CAP='false', PORTACC='true'),
    'nodelay': world(MAXD=0),
    'badseed': world(SEEDOFF=777),
    'noframe': world(FRAMEAPI='false'),
    'space': world(LAND.replace('MODE = "Land"', 'MODE = "Space"')),
    'menu': world(HUMAN_EMPIRE='false'),
}
FULL = ('retail', 'port', 'nodelay', 'badseed', 'noframe')

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


def check_xml_patch(cfg, out):
    """The probe mod's XML: Squad_Rebel_Trooper with FormationRaggedness / MaxJiggleDistance 0, the rest of the file
    as shipped; the config carries the values the mod runs with."""
    fails = []
    rel = 'Data\\XML\\GROUNDINFANTRY.XML'
    if rel not in cfg['files']:
        return ['the mod has no %s (files %s)' % (rel, cfg['files'])]
    text = open(os.path.join(out, rel), 'rb').read().decode('latin-1')
    for tag in ('FormationRaggedness', 'MaxJiggleDistance'):
        v = B._num(B._tag([text], 'Squad_Rebel_Trooper', tag))
        if v != 0.0:
            fails.append('patched Squad_Rebel_Trooper %s = %s, expected 0' % (tag, v))
    if B._num(B._tag([text], 'Squad_Stormtrooper', 'MaxJiggleDistance')) != 2.0:
        fails.append('patched file changed Squad_Stormtrooper MaxJiggleDistance')
    xd = cfg['xml']['dart']
    if (xd['m_jiggle'], xd['m_ragged'], xd['m_jiggle_shipped'], xd['m_ragged_shipped']) != (0.0, 0.0, 3.5, 1.6):
        fails.append('config dart jiggle / raggedness %s / %s (shipped %s / %s), expected 0 / 0 (3.5 / 1.6)' % (
            xd['m_jiggle'], xd['m_ragged'], xd['m_jiggle_shipped'], xd['m_ragged_shipped']))
    for key, a in (('rows', 0.078), ('dart', 0.1092)):
        if abs((cfg['xml'][key].get('c_accel') or 0) - a) > 1e-6:
            fails.append('config %s container acceleration %s, expected %s' % (key, cfg['xml'][key].get('c_accel'), a))
    return fails


def check_math():
    """The analyser's float32 Halton, the Dartboard dims and the retail slot order on fixed cases."""
    fails = []
    third, two_ninths = A.f32(1.0 / 3.0), A.f32(2.0 / A.f32(9.0))
    for i, b, want in ((1, 2, 0.5), (5, 2, 0.625), (1, 3, third), (0, 3, 0.0), (7, 3, A.f32(third + two_ninths))):
        got = A.radical_inverse(i, b)
        if got != want:
            fails.append('radical_inverse(%d, %d) = %r, expected %r' % (i, b, got, want))
    # float accumulation: a large index differs from the double result, but by less than 1e-6
    big = 0xFFFFFFFF
    exact = 0.0
    i, bk = big, 3.0
    while i:
        exact += (i % 3) / bk
        i //= 3
        bk *= 3
    got = A.radical_inverse(big, 3)
    if got == exact or abs(got - exact) > 1e-6 or got != A.f32(got):
        fails.append('radical_inverse(2^32 - 1, 3) = %r vs double %r: expected a float32 within 1e-6' % (got, exact))
    d = A.dart_dims(5, 3.0, 3.0, 0.5)
    if (d['k'], d['m'], d['L'], d['Wd'], d['r']) != (3, 2, 29.0, 16.0, A.f32(18 ** 0.5)):
        fails.append('dart_dims(5, 3, 3, 0.5) = %s, expected k 3, m 2, L 29, Wd 16, r sqrtf(18)' % d)
    pts = A.dartboard_points(5, 3.0, 3.0, 0.5, (41004 * 3000) & 0xFFFFFFFF)
    if not pts or len(pts) != 5 or any(abs(x) > 14.5 or abs(y) > 8.0 for x, y in pts) or \
            min(((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5 for i, a in enumerate(pts) for b in pts[i + 1:]) \
            < 5.0 + 18 ** 0.5 - 1e-5:
        fails.append('dartboard_points: %s (5 points inside +-14.5 x +-8, >= 9.24 apart expected)' % pts)
    # 7.2 step 6 with the i = j + 1 skip: points A..E, members m0..m4 (container at 0, facing +x, Wd / m = 8):
    # group {m0, m1} x {A, B} by side, m2 -> C (skipped index), group {m3, m4} x {D, E}
    P = [(10.0, 5.0), (9.0, -5.0), (-3.0, 6.0), (-4.0, -6.0), (-12.0, 0.0)]
    Mx = [(20.0, -1.0), (19.0, 1.0), (0.0, 2.0), (-1.0, 3.0), (-20.0, 0.0)]
    got = A.retail_assign(P, Mx, (0.0, 0.0), (1.0, 0.0), 16.0, 2)
    want = [P[1], P[0], P[2], P[4], P[3]]
    if got != want:
        fails.append('retail_assign: %s, expected %s (the i = j + 1 skip)' % (got, want))
    hx = A.joint_hx([a for a, _ in A.rows_slots(9, 3.0, 3.0, 0.3)], 3.0)
    if hx != 15.0:
        fails.append('joint_hx of the 3 x 3 Rows grid: %s, expected 15.0 (FIGHTER_FLYING 7.7)' % hx)
    return fails


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
    fails += check_xml_patch(cfg, out)
    fails += check_math()
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
    for name in FULL + ('space', 'menu'):
        log = os.path.join(work, name + '.log')
        consts = 'SPAWN_RAD = %r\nSEED = 11\n' % (sp['theta0'] * 3.141592653589793 / 180.0)
        flags, land = WORLDS[name]
        chunks = ['LOGFILE = "%s"' % lua_str(log), 'FRAMES = %d' % (frames if name in FULL else 900),
                  consts, flags, STUB, land, B.lua_config(cfg), probe, DRIVE]
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

    for name in FULL:
        if name not in results:
            continue
        text, res = results[name]
        log = os.path.join(work, name + '.log')
        errs = [line for line in open(log) if line.startswith(('S8Q|ERR|', 'S8Q|APIERR|'))]
        if name == 'noframe':
            # GetCurrentTime.Frame missing: one APIERR, the run goes on with g=?
            if len(errs) != 1 or 'api=GetCurrentTime.Frame' not in errs[0]:
                fails.append('noframe run: expected one APIERR for GetCurrentTime.Frame, got %s' % errs[:3])
        elif errs:
            fails.append('%s run recorded %d APIERR / ERR records' % (name, len(errs)))
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
        # as the retail run 20261110-043421: Stormtrooper members (vmax 1.25 x the team) catch up, Rebel members
        # (vmax = the team speed) do not and close up only while the container brakes
        'retail': [('rows', 'Q1', 'TIGHT'), ('rows', 'Q1_close', 'CLOSES'), ('rows', 'catch_up', 'YES'),
                   ('rows', 'q1_ref', 'pred'), ('dart', 'Q1', 'LOOSE'), ('dart', 'Q1_close', 'CLOSES'),
                   ('dart', 'catch_up', 'NO')],
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
        # the re-lay is judged by the fitted layouts (relay_rule below: leg 3 laid at its order, legs 2 and 4 keep the
        # last layout); the settled slot set change only has to rise on the 30 deg leg against the 5 deg one
        rel = v['dart']['relay']
        for (c3, _m3), (c4, _m4) in zip(rel.get(3, []) or [], rel.get(4, []) or []):
            if c3 is None or c4 is None or not c3 > c4:
                fails.append('retail: dart slot set change leg 3 %s vs leg 4 %s, expected leg 3 larger (re-lay)' % (c3, c4))
        if not rel.get(3) or not rel.get(4):
            fails.append('retail: dart slot set change missing for legs 3 / 4')
        for field in ('relay_rule', 'Q3_order'):
            if not str(v['dart'].get(field, '')).startswith('MATCH'):
                fails.append('retail: dart %s %s, expected MATCH' % (field, v['dart'].get(field)))
        for key in ('rows', 'dart'):
            for field in ('Q77_accel', 'Q77_straggler'):
                if not str(v[key].get(field, '')).startswith('MATCH'):
                    fails.append('retail: %s %s %s, expected MATCH' % (key, field, v[key].get(field)))
        df = results['retail'][1].get('dart_fit') or {}
        if not df.get('n_fit') or df.get('n_match') != df.get('n_fit'):
            fails.append('retail: Dartboard NOW fit %s / %s legs match, expected all' % (df.get('n_match'),
                                                                                     df.get('n_fit')))
        # the stub lays the slots at the container's next frame: NOW of the spawn / order + 1
        for h in v['dart'].get('halton', []):
            want_ev = 'leg 3 order' if h['leg'] >= 3 else 'spawn'
            if h.get('event') != want_ev or h.get('dnow') != 1:
                fails.append('retail: trial %d leg %d laid at %s +%s, expected %s +1' % (
                    h['k'], h['leg'], h.get('event'), h.get('dnow'), want_ev))
        ck = results['retail'][1].get('clock') or {}
        if (ck.get('g_minus_f_min'), ck.get('g_minus_f_max')) != (1234, 1234):
            fails.append('retail: g - f %s..%s, expected the stub offset 1234' % (ck.get('g_minus_f_min'),
                                                                                ck.get('g_minus_f_max')))
    if 'port' in results:
        v = results['port'][1]['verdicts']
        for key in ('rows', 'dart'):
            if not str(v[key].get('Q77_accel', '')).startswith('DIFFERS'):
                fails.append('port: %s Q77_accel %s, expected DIFFERS (the container type\'s own acceleration)' % (
                    key, v[key].get('Q77_accel')))
    if 'badseed' in results:
        v = results['badseed'][1]['verdicts']
        df = results['badseed'][1].get('dart_fit') or {}
        if not str(v['dart'].get('Q3', '')).startswith('NO MATCH') or df.get('n_match'):
            fails.append('badseed: dart Q3 %s (%s legs match), expected NO MATCH' % (v['dart'].get('Q3'),
                                                                                   df.get('n_match')))
    if 'noframe' in results:
        v = results['noframe'][1]['verdicts']
        if not str(v['dart'].get('Q3', '')).startswith('n/a'):
            fails.append('noframe: dart Q3 %s, expected n/a (no NOW)' % v['dart'].get('Q3'))
        if not str(v['rows'].get('Q77_accel', '')).startswith('MATCH'):
            fails.append('noframe: rows Q77_accel %s, expected MATCH (needs no NOW)' % v['rows'].get('Q77_accel'))
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
