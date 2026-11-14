#!/usr/bin/env python3
"""selftest_cu9h.py - offline check of the cu9h bombing-run probe kit (no game is started or attached; nothing is
installed in the game folder).

  selftest_cu9h.py [--game DIR] [--source-zip extracted\\foc_data_text.zip] [--keep]

1. Builds the mod into a temp folder from the text zip (offline, no texts) and, when --game has an install, from the
   install (read only): every .xml parses, GameConstants.xml differs from the shipped file only in the two interval
   values, the campaign names the target, the story Lua compiles under stock Lua 5.0.2
   (tools\\probe_mod\\out\\tools\\lua50.exe, ..\\common\\build_lua50.cmd builds it on first use).
2. Runs the story Lua under Lua 5.0.2 with stub engine functions: human Rebel -> defenders spawned for the Empire on the
   target, the 5 attack units for the Rebel on Abregado Rae, one fleet, Move_To(target), BlockOnCommand, Invade, the
   two messages, and a second CU9H_Begin does nothing; Rebel not human -> nothing spawned, the NOT_HUMAN message.
3. cu9h_mem.snapshot on a fake memory image (every documented offset placed by hand): phase, record, dial, colour,
   ground point, cursor, button bits; expected_dial against the HUD_GAPS sec. 9.3 rules.
4. cu9h_dbg.py --selftest: the debugger attaches to a fake process and must see the four fields' writes with the
   ground budget and its refill, detach cleanly (stop file) and leave the process alive.
5. cu9h_report on synthetic run folders: verdict lines for Q1..Q4 (CALLBACK_NOT_RUN, then CALLBACK_RAN).
6. cu9h_cleanup on a fake game folder removes only the marked mod folder.
Ends with CU9H SELFTEST OK, or lists the failures (exit 1).
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
import lua50  # noqa: E402
import build_cu9h_mod as B  # noqa: E402
import cu9h_cleanup  # noqa: E402
import cu9h_mem as M  # noqa: E402
import cu9h_report as R  # noqa: E402

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print('FAIL', what)
    return cond


# ---- 1. build -------------------------------------------------------------------------------------------------------

def check_build(out, cfg, shipped_constants, label):
    for rel in cfg['files']:
        p = os.path.join(out, rel)
        if rel.lower().endswith('.xml'):
            try:
                ET.fromstring(open(p, 'rb').read())
            except ET.ParseError as e:
                check(False, '%s: %s does not parse: %s' % (label, rel, e))
    camp = open(os.path.join(out, 'Data', 'XML', 'CU9H_Probe_Campaigns.xml'), encoding='latin-1').read()
    root = ET.fromstring(camp.encode('latin-1'))
    cs = {c.get('Name'): c for c in root.findall('Campaign')}
    auto, manual = cs.get('CU9H_Probe_Bombing'), cs.get('CU9H_Probe_Bombing_Manual')
    if check(auto is not None and manual is not None, '%s: both campaigns present: %s' % (label, sorted(cs))):
        def forces(c):
            return [' '.join(x.text.split()) for x in c.findall('Starting_Forces')]
        check(auto.findtext('Rebel_Story_Name', '').strip() == 'CU9H_Story_Plots.xml' and
              manual.find('Rebel_Story_Name') is None, '%s: story only in the story campaign' % label)
        check('Empire, %s, E_Ground_Barracks' % cfg['planet'] in forces(auto) and
              [f for f in forces(manual) if f.startswith('Rebel, Abregado_Rae, ') and 'Barracks' not in f] ==
              ['Rebel, Abregado_Rae, %s' % u for u in B.ATTACK] and
              not [f for f in forces(auto) if 'Y-Wing' in f], '%s: Starting_Forces %s / %s' % (
                  label, forces(auto), forces(manual)))
        check(auto.findtext('Campaign_Set').strip() != manual.findtext('Campaign_Set').strip(),
              '%s: the two campaigns share a set' % label)
    cf = open(os.path.join(out, 'Data', 'XML', 'CampaignFiles.xml'), encoding='latin-1').read()
    check('CU9H_Probe_Campaigns.xml' in cf, '%s: CampaignFiles.xml does not list the campaign' % label)
    gc = open(os.path.join(out, 'Data', 'XML', 'GameConstants.xml'), 'rb').read()
    check(b'<Max_Bombing_Run_Interval_Seconds>60.0</Max_Bombing_Run_Interval_Seconds>' in gc and
          b'<Min_Bombing_Run_Interval_Seconds>60.0</Min_Bombing_Run_Interval_Seconds>' in gc,
          '%s: GameConstants.xml intervals' % label)
    back = gc.replace(b'<Max_Bombing_Run_Interval_Seconds>60.0<', b'<Max_Bombing_Run_Interval_Seconds>120.0<') \
             .replace(b'<Min_Bombing_Run_Interval_Seconds>60.0<', b'<Min_Bombing_Run_Interval_Seconds>120.0<')
    check(back == shipped_constants, '%s: GameConstants.xml differs from the shipped file elsewhere' % label)
    lua = os.path.join(out, 'Data', 'Scripts', 'Story', 'CU9H_Story.lua')
    t = open(lua, encoding='ascii').read()
    check('@@' not in t and 'CU9H_TARGET = "%s"' % cfg['planet'] in t, '%s: story Lua placeholders' % label)
    return lua


HARNESS = r'''
LOG = {}
function log(s) table.insert(LOG, s); print("LOG " .. s) end
function require(x) log("require " .. x) end
OnEnter = 1
function mkplayer(name, human) local p = {name = name}; p.Is_Human = function() return human end; return p end
PLAYERS = { Rebel = mkplayer("Rebel", @@HUMAN@@), Empire = mkplayer("Empire", false) }
function Find_Player(n) return PLAYERS[n] end
function FindPlanet(n) return { name = n } end
function TestValid(o) return o ~= nil end
function SpawnList(list, planet, player, a, b)
	log("spawn " .. player.name .. " " .. planet.name .. " " .. table.concat(list, ","))
	local out = {}
	for i, n in ipairs(list) do out[i] = { type = n } end
	return out
end
function Assemble_Fleet(units)
	log("assemble " .. table.getn(units))
	local f = {}
	f.Move_To = function(p) log("move " .. p.name); return "block" end
	f.Invade = function() log("invade") end
	return f
end
function BlockOnCommand(b) log("block " .. tostring(b)) end
function Sleep(n) log("sleep " .. n) end
function Game_Message(t) log("msg " .. t) end
function Create_Thread(name) log("thread " .. name); _G[name]() end
dofile([[@@SCRIPT@@]])
Definitions()
StoryModeEvents.CU9H_Begin(OnEnter)
StoryModeEvents.CU9H_Begin(OnEnter)
print("HARNESS_END")
'''


def run_story(lua, script, human, tmp):
    h = os.path.join(tmp, 'harness_%s.lua' % ('human' if human else 'ai'))
    open(h, 'w').write(HARNESS.replace('@@HUMAN@@', 'true' if human else 'false').replace('@@SCRIPT@@', script))
    r = subprocess.run([lua, h], capture_output=True, text=True, timeout=60)
    return [ln[4:] for ln in r.stdout.splitlines() if ln.startswith('LOG ')], r.stdout + r.stderr


def check_story(lua, script, planet, tmp):
    log, raw = run_story(lua, script, True, tmp)
    want = ['require PGStoryMode', 'thread CU9H_Invade',
            'spawn Empire %s %s' % (planet, ','.join(B.DEFENSE)),
            'spawn Rebel Abregado_Rae %s' % ','.join(B.ATTACK), 'assemble 5', 'msg TEXT_CU9H_LUA_MOVING',
            'move %s' % planet, 'block block', 'sleep 1', 'invade', 'msg TEXT_CU9H_LUA_INVADE']
    check(log == want and 'HARNESS_END' in raw, 'story (human Rebel): %s\n%s' % (log, raw[-400:]))
    log, raw = run_story(lua, script, False, tmp)
    check(log == ['require PGStoryMode', 'thread CU9H_Invade', 'msg TEXT_CU9H_LUA_NOT_HUMAN'] and 'HARNESS_END' in raw,
          'story (AI Rebel): %s' % log)


# ---- 3. snapshot on a fake image ---------------------------------------------------------------------------------------

class FakeMem:
    def __init__(self):
        self.regions = []

    def region(self, addr, size):
        b = bytearray(size)
        self.regions.append((addr, b))
        return b

    def read(self, addr, n):
        for a, b in self.regions:
            if a <= addr and addr + n <= a + len(b):
                return bytes(b[addr - a:addr - a + n])
        return None


def fake_image(flag, colour, rec_av=0, comp_disabled=1, dial=50):
    base = 0x140000000
    m = FakeMem()
    gmm = m.region(base + M.RVA_GMM, 0x58)
    struct.pack_into('<i', gmm, 0x1C, 1)
    struct.pack_into('<Q', gmm, 0x38, 0x5000000)
    struct.pack_into('<i', gmm, 0x50, 2)
    struct.pack_into('<I', m.region(base + M.RVA_FRAME_SYNC, 8), 0, 12345)
    mode = m.region(0x5000000, 0x400)
    struct.pack_into('<i', mode, 0x10, 1000)
    struct.pack_into('<Q', mode, 0x38, 0x6000000)
    mgr = m.region(0x6000000, 0x40)
    mgr[0] = 1
    struct.pack_into('<i', mgr, 0x14, 1800)
    struct.pack_into('<Q', mgr, 0x20, 0x6100000)
    struct.pack_into('<i', mgr, 0x28, 3)
    recs = m.region(0x6100000, 3 * M.REC_SIZE)
    o = M.REC_SIZE * 1
    recs[o], recs[o + 1] = 1, rec_av
    struct.pack_into('<iii', recs, o + 4, 1900, 1800, 100)   # ready 1900, interval 1800, last start 100: f = 0.5
    pl = m.region(base + M.RVA_PLAYER_LIST, 0x38)
    struct.pack_into('<QQ', pl, 0, 0x7000000, 0x7000000 + 3 * 8)
    struct.pack_into('<i', pl, 0x30, 2)
    vec = m.region(0x7000000, 3 * 8)
    struct.pack_into('<Q', vec, 16, 0x7100000)
    struct.pack_into('<i', m.region(0x7100000, 0x100), 0x4C, 1)
    hud = m.region(base + M.RVA_HUD, 0x2B00)
    struct.pack_into('<i', hud, M.HUD_MODE, 3)
    hud[M.HUD_FLAG] = flag
    struct.pack_into('<Q', hud, M.HUD_BLOB, 0x8000000 if colour else 0)
    struct.pack_into('<fff', hud, M.HUD_GROUND, 10.0, 20.0, 5.0)
    if colour:
        struct.pack_into('<ffff', m.region(0x8000000, 0x100), M.BLOB_COLOUR, *colour)
    cur = m.region(base + M.RVA_CURSOR_X, 8)
    struct.pack_into('<ii', cur, 0, 300, 400)
    comps = m.region(base + M.RVA_COMPONENTS, M.COMP_ENTRY * 0x100)
    struct.pack_into('<Q', comps, M.COMP_ENTRY * M.COMP_SPECIAL_WEAPON + 8, 0x9000000)
    comp = m.region(0x9000000, 0x700)
    struct.pack_into('<Q', comp, 0, base + M.VFT_BUTTON)
    comp[M.COMP_FLAGS] = 4 if comp_disabled else 0
    struct.pack_into('<i', comp, M.COMP_STATE, 4 if comp_disabled else 1)
    struct.pack_into('<i', comp, M.COMP_DIAL, dial)
    return m, base


def check_snapshot():
    m, base = fake_image(0, None)
    s = M.snapshot(m.read, base)
    check(s['phase'] == 'COOLDOWN' and s['local_id'] == 1 and s['rec_en'] == 1 and s['rec_av'] == 0 and
          s['rec_ready'] == 1900 and s['rec_last'] == 100 and s['comp_disabled'] == 1 and s['comp_dial'] == 50 and
          s['comp_vft'] == M.VFT_BUTTON and s['dial_expected'] == 50 and s['ground'] == (10.0, 20.0, 5.0) and
          s['cursor'] == (300, 400) and s['locked'] == 0 and s['ready'] == 0 and s['recs_enabled'] == '1' and
          s['mf'] == 1000 and s['gf'] == 12345 and s['hud_mode'] == 3,
          'snapshot COOLDOWN: %s' % M.line(s))
    check(abs(M.secs_to_ready(s) - 30.0) < 1e-6, 'secs_to_ready')
    m, base = fake_image(1, (1.0, 0.0, 0.0, 1.0), rec_av=1, comp_disabled=0, dial=100)
    s = M.snapshot(m.read, base)
    check(s['phase'] == 'TARGETING' and s['blob_col'] == 'RED' and s['ready'] == 1 and s['dial_expected'] == 100,
          'snapshot TARGETING: %s' % M.line(s))
    d = M.parse_line(M.line(s))
    check(d['blob_col'] == 'RED' and d['colour'] == '1.00,0.00,0.00,1.00' and d['comp_vft'] == '%x' % M.VFT_BUTTON,
          'line / parse_line round trip: %s' % d)
    check(M.colour_name((0.0, 1.0, 0.0, 1.0)) == 'GREEN' and M.colour_name((1, 1, 1, 1)) == 'OTHER', 'colour_name')

    def ed(**kw):
        s = {'rec_en': 1, 'rec_av': 0, 'rec_inp': 0, 'rec_ready': 1900, 'rec_last': 100, 'mf': 1000, 'ready': 0}
        s.update(kw)
        return M.expected_dial(s)
    check(ed() == 50, 'dial: half way')
    check(ed(ready=1, rec_av=1) == 100, 'dial: ready -> 100')
    check(ed(rec_en=0) == 100, 'dial: no record -> 0 shown as 100')
    check(ed(rec_inp=1) is None, 'dial: in progress not computed')
    check(ed(rec_av=1) == 100, 'dial: available but not ready (locked) -> 100')
    check(ed(mf=1900) == 100, 'dial: now >= ready -> 100')
    check(ed(rec_last=-1) == 100, 'dial: last start -1 -> shown 100 (rule 4)')
    check(ed(mf=100) == 100, 'dial: on the start frame -> shown 100')
    check(ed(mf=101) == 0, 'dial: one frame after the start -> 0 (f > 0 is not shown as 1.0)')
    check(ed(mf=1899) == 99, 'dial: one frame before ready -> 99')


# ---- 5. report ------------------------------------------------------------------------------------------------------

def write_run(d, disabled_click_sets_flag):
    os.makedirs(d, exist_ok=True)
    m, base = fake_image(0, None)
    cool = M.snapshot(m.read, base)
    m, base = fake_image(0, None, rec_av=1, comp_disabled=0, dial=100)
    ready = M.snapshot(m.read, base)
    m, base = fake_image(1, (0.0, 1.0, 0.0, 1.0), rec_av=1, comp_disabled=0, dial=100)
    green = M.snapshot(m.read, base)
    m, base = fake_image(1, (1.0, 0.0, 0.0, 1.0), rec_av=1, comp_disabled=0, dial=100)
    red = M.snapshot(m.read, base)
    gal = dict(cool, mt=0, stack=1, phase='GALAXY')
    lines = ['100.0 S ' + M.line(gal), '101.0 S ' + M.line(cool), '130.0 S ' + M.line(ready),
             '140.0 S ' + M.line(green), '140.1 T ' + M.line(green), '141.0 S ' + M.line(red),
             '142.0 S ' + M.line(ready)]
    open(os.path.join(d, 'reader.log'), 'w').write('\n'.join(lines) + '\n')
    marks = ['105.0 MARK label=q1_cooldown_50 ' + M.line(cool), '106.0 MARK label=q3_click_disabled ' + M.line(cool),
             '131.0 MARK label=q1_ready ' + M.line(ready), '140.5 MARK label=q2_open ' + M.line(green),
             '141.5 MARK label=q2_fog ' + M.line(red)]
    open(os.path.join(d, 'marks.log'), 'w').write('\n'.join(marks) + '\n')
    walk_toggle = 'walk=2e7990,346ac0,2f7000,61000'
    walk_ground = 'walk=2f800,3bb500,56500'
    dbg = ['CU9HDBG 1 t=x', 'ATTACH pid=1 base=140000000 hud=140b27f60 size_of_image=cc0000 sym=1',
           'BATTLE mode=5000000 t=100.5', 'ARM mask=0123 why=battle threads=20 failed=0 budget=240 refills=0 t=100.6']
    for i in range(5):
        dbg.append('HIT n=%d slot=0 what=ground_x val=%d.000 tid=1 rip=2f800 t=101.%d %s %s' % (
            i + 1, i, i, M.line(cool), walk_ground))
    if disabled_click_sets_flag:
        dbg.append('HIT n=6 slot=1 what=flag val=1 tid=1 rip=2e79a0 t=106.0 %s %s' % (
            M.line(dict(cool, flag=1)), walk_toggle))
    dbg.append('HIT n=7 slot=1 what=flag val=1 tid=1 rip=2e79a0 t=139.9 %s %s' % (M.line(green), walk_toggle))
    dbg.append('HIT n=8 slot=1 what=flag val=0 tid=1 rip=2f7100 t=142.0 %s %s' % (M.line(ready), walk_toggle))
    open(os.path.join(d, 'dbg.log'), 'w').write('\n'.join(dbg) + '\n')


def check_report(tmp, exe):
    d = os.path.join(tmp, 'run_ok')
    write_run(d, False)
    text, n = R.report(d, exe)
    check(n > 0, 'report: no battle samples')
    check('VERDICT Q3: CALLBACK_NOT_RUN' in text and 'control (flag:=1 by an enabled click): 1' in text,
          'report Q3 not-run:\n' + text)
    check('VERDICT Q2 (memory): both GREEN (0,1,0,1) and RED (1,0,0,1) written' in text, 'report Q2:\n' + text)
    check('VERDICT Q1 (memory): READY and COOLDOWN marks present' in text, 'report Q1:\n' + text)
    q4 = [ln for ln in text.splitlines() if ln.startswith('  VERDICT Q4')]
    check(q4 and ('2ee40' in q4[0] or '2f800' in q4[0]), 'report Q4: %s' % q4)
    check('0 differ by more than 1' in text, 'report: dial formula mismatches in the synthetic run')
    d2 = os.path.join(tmp, 'run_ran')
    write_run(d2, True)
    text2, _ = R.report(d2, exe)
    check('VERDICT Q3: CALLBACK_RAN' in text2, 'report Q3 ran:\n' + text2)
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=B.DEFAULT_GAME)
    ap.add_argument('--source-zip', default=os.path.join(ROOT, 'extracted', 'foc_data_text.zip'))
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix='cu9h_selftest_')
    lua = lua50.find_lua()
    check(lua is not None, 'lua50.exe missing (see %s)' % lua50.BUILD_LOG)
    import zipfile
    shipped = zipfile.ZipFile(a.source_zip).read('Data/XML/GAMECONSTANTS.XML')
    # 1 + 2: builds and story
    builds = [('zip', None, a.source_zip, True)]
    if os.path.isfile(os.path.join(a.game, 'corruption', 'StarWarsG.exe')):
        builds.append(('install', a.game, None, False))
    else:
        print('no install at %s: install build skipped' % a.game)
    for label, game, z, no_text in builds:
        for target in sorted(B.TARGETS):
            out = os.path.join(tmp, 'mod_%s_%s' % (label, target), B.MOD_NAME)
            cfg = B.build(game or a.game, out, target, 60, source_zip=z, no_text=no_text)
            script = check_build(out, cfg, shipped, '%s/%s' % (label, target))
            if label == 'install':
                check('found' in cfg['map_check'], 'install/%s: map check %s' % (target, cfg['map_check']))
                check(any(r.endswith('MasterTextFile_English.dat') for r in cfg['files']), 'install: no text file')
            if lua:
                r = subprocess.run([lua, '-e', 'assert(loadfile([[%s]]))' % script], capture_output=True, text=True)
                check(r.returncode == 0, '%s/%s: story Lua does not compile: %s' % (label, target, r.stderr))
                if label == 'zip':
                    check_story(lua, script, cfg['planet'], tmp)
            print('build %s/%s: %d files, map %s' % (label, target, len(cfg['files']), cfg['map_check']))
    # 3
    check_snapshot()
    print('snapshot / dial checks done')
    # 4
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'cu9h_dbg.py'), '--selftest'], capture_output=True,
                       text=True, timeout=300)
    print(r.stdout.strip())
    check(r.returncode == 0 and 'CU9H DBG SELFTEST OK' in r.stdout, 'debugger selftest: %s' % r.stderr[-600:])
    # 5
    exe = os.path.join(a.game, 'corruption', 'StarWarsG.exe')
    check_report(tmp, exe)
    print('report checks done (function names %s)' % ('from the exe' if os.path.isfile(exe) else 'raw'))
    # 6
    fake_game = os.path.join(tmp, 'game')
    mine = os.path.join(fake_game, 'corruption', 'Mods', B.MOD_NAME)
    other = os.path.join(fake_game, 'corruption', 'Mods', 'OtherMod')
    os.makedirs(mine)
    os.makedirs(other)
    open(os.path.join(mine, B.MARKER), 'w').write('x')
    rep = cu9h_cleanup.cleanup(fake_game)
    check(rep['ok'] and not os.path.exists(mine) and os.path.isdir(other), 'cleanup: %s' % rep)
    os.makedirs(os.path.join(fake_game, 'corruption', 'Mods', B.MOD_NAME))
    rep = cu9h_cleanup.cleanup(fake_game)
    check(os.path.isdir(os.path.join(fake_game, 'corruption', 'Mods', B.MOD_NAME)), 'cleanup removed an unmarked folder')
    if not a.keep:
        shutil.rmtree(tmp, ignore_errors=True)
    else:
        print('kept', tmp)
    print('CU9H SELFTEST', 'OK' if not FAILS else 'FAILED (%d)' % len(FAILS))
    return 0 if not FAILS else 1


if __name__ == '__main__':
    sys.exit(main())
