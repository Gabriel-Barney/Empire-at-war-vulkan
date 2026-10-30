#!/usr/bin/env python3
"""selftest_hook.py - runs the rendered s10_hook.lua under a stock Lua 5.0.2 interpreter with stubbed engine
functions, then parses the produced log with s10_parse.py (offline check of the hook's Lua and its log format).

  selftest_hook.py --lua <lua.exe built from toolchain\\lua-5.0.2> [--map _MP_LAND_POLUS] [--xyz | --dist]

The stubs model one land battle: Find_Player(faction) -> player with id = faction index (the "faction" rule),
Find_All_Objects_Of_Type(player | type name) -> the map's placements from expected_objects.json, GetCurrentTime()
advancing 1 s per service. --dist removes PositionWrapper.Get_XYZ so the Get_Distance fallback is exercised.
"""
import argparse
import json
import os
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import build_mod  # noqa: E402

STUBS = r'''
S10_TEST_OUT = io.open(S10_TEST_LOG, "w")
function _CustomScriptMessage(file, text) S10_TEST_OUT:write(text) end
function _OuputDebug(text) end
function _MessagePopup(text) S10_TEST_OUT:write("POPUP " .. text .. "\n") end
function Game_Message(id) S10_TEST_OUT:write("GAME_MESSAGE " .. id .. "\n") end
function StringCompare(a, b) return string.upper(a) == string.upper(b) end
S10_TEST_TIME = 0
GetCurrentTime = setmetatable({ Frame = function() return math.floor(S10_TEST_TIME * 30) end },
	{ __call = function() return S10_TEST_TIME end })
function Create_Position(x, y, z) return { x = x, y = y, z = z } end
function make_player(id, fac)
	return {
		Get_ID = function() return id end,
		Get_Faction_Name = function() return string.upper(fac) end,
		Get_Name = function() return fac .. " Player" end,
		Is_Human = function() return fac == S10_TEST_HUMAN end,
		Is_Neutral = function() return fac == "Neutral" end,
	}
end
S10_TEST_PLAYERS = {}
for i, fac in ipairs({ "Rebel", "Empire", "Pirates", "Neutral", "Hostile", "Sarlacc", "Underworld", "Hutts" }) do
	S10_TEST_PLAYERS[string.upper(fac)] = make_player(i - 1, fac)
end
function Find_Player(name)
	if name == "local" then return S10_TEST_PLAYERS[string.upper(S10_TEST_HUMAN)] end
	return S10_TEST_PLAYERS[string.upper(name)]
end
function make_obj(t, x, y, z, owner)
	local pos = { Get_XYZ = function() return x, y, z end }
	if S10_TEST_DIST then pos = {} end
	return {
		Get_Type = function() return { Get_Name = function() return string.upper(t) end } end,
		Get_Owner = function() return owner end,
		Get_Position = function() return pos end,
		Get_Hull = function() return 1.0 end,
		Get_Distance = function(p) return math.sqrt((x - p.x) ^ 2 + (y - p.y) ^ 2 + (z - p.z) ^ 2) end,
	}
end
function Find_All_Objects_Of_Type(filter)
	local out = {}
	for _, o in ipairs(S10_TEST_OBJECTS) do
		local owner = o.Get_Owner()
		if type(filter) == "table" then
			if owner == filter then table.insert(out, o) end
		elseif string.upper(o.Get_Type().Get_Name()) == string.upper(filter) then
			table.insert(out, o)
		end
	end
	return out
end
-- the shipped GameScoring entry points the hook wraps
function Base_Definitions() ServiceRate = 10 end
function Game_Mode_Starting_Event(mode_name, map_name) end
function Game_Mode_Ending_Event(mode_name) end
function GameService() end
'''

DRIVER = r'''
Base_Definitions()
assert(ServiceRate == 1, "ServiceRate not lowered")
Game_Mode_Starting_Event("Galactic", "")
GameService()
Game_Mode_Ending_Event("Galactic")
Game_Mode_Starting_Event("Land", S10_TEST_MAP)
for i = 1, 45 do
	S10_TEST_TIME = S10_TEST_TIME + 1
	GameService()
end
Game_Mode_Ending_Event("Land")
S10_TEST_OUT:close()
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--lua', required=True)
    ap.add_argument('--map', default='_MP_LAND_POLUS')
    ap.add_argument('--dist', action='store_true')
    a = ap.parse_args()
    cfg = json.load(open(os.path.join(HERE, 'maps.json')))
    exp = json.load(open(os.path.join(HERE, 'expected_objects.json')))
    scen = next(s for s in cfg['scenarios'] if s['map'] == a.map)
    human = scen['attacker'] if scen['mode'] == 'campaign' else scen['slots'][0][0]
    order = cfg['faction_order']
    out_dir = os.path.join(HERE, 'out', 'selftest')
    os.makedirs(out_dir, exist_ok=True)
    log = os.path.join(out_dir, 'hook_%s%s.txt' % (a.map, '_dist' if a.dist else ''))
    objs = []
    for r in exp[a.map]['objects']:
        if r['kind'] == 'marker':
            continue
        fac = order[r['owner']] if r['owner'] < len(order) else 'Neutral'
        x, y, z = r['pos']
        objs.append('make_obj(%s, %r, %r, %r, S10_TEST_PLAYERS["%s"])' % (json.dumps(r['type']), x, y, z, fac.upper()))
    lua = os.path.join(out_dir, 'hook_test.lua')
    with open(lua, 'w', encoding='ascii') as f:
        f.write('S10_TEST_LOG = %s\nS10_TEST_MAP = "DATA\\\\ART\\\\MAPS\\\\%s.TED"\nS10_TEST_HUMAN = "%s"\n'
                'S10_TEST_DIST = %s\n' % (json.dumps(log), a.map, human, 'true' if a.dist else 'false'))
        f.write(STUBS)
        f.write('S10_TEST_OBJECTS = {\n' + ',\n'.join(objs) + '\n}\n')
        f.write(build_mod.hook_text(build_mod.probe_types(exp), popup=True))
        f.write(DRIVER)
    r = subprocess.run([a.lua, lua], capture_output=True, text=True)
    sys.stdout.write(r.stdout + r.stderr)
    if r.returncode != 0:
        print('HOOK SELFTEST FAILED: lua exit %d' % r.returncode)
        return 1
    text = open(log).read()
    dumps = text.count('\nDUMP ')
    import s10_parse
    res = s10_parse.cmd_parse(argparse.Namespace(log=log, out=os.path.join(out_dir, 'hook_table.txt'), tag='t0'))
    ok = (dumps == 3 and 'S10HOOK 1 loaded' in text and 'POPUP S10 t0' in text and len(res) == 1 and
          'POPUP S10 hook alive battle=1 mode=Land' in text and 'GAME_MESSAGE TEXT_S10_HOOK_ALIVE' in text)
    for rows in res.values():
        for raw, row in rows.items():
            if row['found'] and raw < 8 and (row['id_eq'] != 1.0 or row['fac_eq'] != 1.0):
                ok = False
                print('mismatch raw %s: %s' % (raw, row))
    print('dumps=%d' % dumps)
    print('HOOK SELFTEST OK' if ok else 'HOOK SELFTEST FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
