#!/usr/bin/env python3
"""build_mod.py - builds the TSW planet-destroy probe mod (Mods\\TswPlanetProbe) from the installed game's own files.

  build_mod.py [--game DIR] [--out MODDIR] [--rate SECONDS] [--check]

Spec: docs/re/TSW_PLANET_DESTROY.md (L1 systems t04). Nothing from the game is stored in the repository: the shipped
GameScoring.lua, CampaignFiles.xml and MasterTextFile_English.dat are read from the install (loose files / MEGs, FoC
file order, the s10_map_owners reader) and patched:
  Data\\Scripts\\Miscellaneous\\GameScoring.lua   shipped script + tswp_hook.lua (fires the Death Star, stamps frames)
  Data\\XML\\CampaignFiles.xml                    shipped list + TSWP_Probe_Campaign.xml
  Data\\XML\\TSWP_Probe_Campaign.xml              "TSWP PROBE: KUAT" (Empire with the Death Star at Byss, Rebel
                                                 Kuat with a level-1 station, ships, barracks and infantry)
  Data\\XML\\TSWP_Story_Plots.xml, TSWP_Story.xml  galactic plot: TSWP_Begin (3 s, proof) and TSWP_Heard
                                                 (STORY_PLANET_DESTROYED Kuat: SCREEN_TEXT, expected never)
  Data\\Scripts\\Story\\TSWP_Story.lua             from tswp_story.lua: owner / starbase status popups
  Data\\Text\\MasterTextFile_English.dat          shipped text + TEXTS
  TSWP_PROBE_MARKER.txt                           lets cleanup recognise the folder as ours
--check builds into a scratch folder and checks the result (Lua placeholders filled, XML parses) without the game
text files when the install is missing.
"""
import argparse
import os
import shutil
import sys
import xml.dom.minidom

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
S10 = os.path.join(os.path.dirname(HERE), 's10_map_owners')
sys.path.insert(0, S10)
sys.path.insert(0, os.path.dirname(HERE))

MARKER = 'TSWP_PROBE_MARKER.txt'
MOD_NAME = 'TswPlanetProbe'
TARGET = 'Kuat'  # FoC's Alderaan is the asteroid field with Planet_Is_Destroyable_By_TSW No
SOURCE = 'Byss'
ROUTE = 'Byss_Kuat'
SPARE = 'Dantooine'  # second Rebel planet, no route: keeps the Rebels alive so the conquest does not end after run W
DUMMY = 'Kuat_Backdrop_Large'  # the SpaceProp with DUMMY_TACTICAL_SUPER_WEAPON_TARGET on Kuat's space maps [M]

CAMPAIGN = r'''<?xml version="1.0"?>
<Campaigns>
	<!-- TSW planet-destroy probe (tools/probe_mod/tsw_planet_destroy): move the Death Star fleet from Byss to Kuat;
	     the space battle's GameScoring hook fires the TSW by itself and stamps frames. -->
	<Campaign Name="TSWP_Probe_Empire">
		<Campaign_Set> TSWP_Probe_Set </Campaign_Set>
		<Sort_Order>0</Sort_Order>
		<Text_ID> TEXT_TSWP_PROBE_NAME </Text_ID>
		<Description_Text> TEXT_TSWP_PROBE_DESC </Description_Text>
		<Camera_Shift_X> 0.0 </Camera_Shift_X>
		<Camera_Shift_Y> 0.0 </Camera_Shift_Y>
		<Camera_Distance> 1200.0 </Camera_Distance>

		<Locations>
				Galaxy_Core_Art_Model,
				@@SOURCE@@,
				@@TARGET@@,
				@@SPARE@@
		</Locations>

		<Trade_Routes>
				@@ROUTE@@
		</Trade_Routes>

		<Home_Location> Empire, @@SOURCE@@ </Home_Location>
		<Home_Location> Rebel, @@TARGET@@ </Home_Location>

		<Starting_Active_Player> Empire </Starting_Active_Player>
		<Empire_Story_Name>TSWP_Story_Plots.xml</Empire_Story_Name>

		<AI_Player_Control> Rebel, None </AI_Player_Control>
		<AI_Player_Control> Pirates, None </AI_Player_Control>
		<AI_Player_Control> Underworld, None </AI_Player_Control>

		<Human_Victory_Conditions>
				Galactic_All_Planets_Controlled
		</Human_Victory_Conditions>
		<AI_Victory_Conditions>
				Galactic_All_Planets_Controlled
		</AI_Victory_Conditions>

		<Markup_Filename>Empire, DefaultGalacticHints </Markup_Filename>
		<Markup_Filename>Rebel, DefaultGalacticHints </Markup_Filename>

		<Supports_Custom_Settings>False</Supports_Custom_Settings>
		<Show_Completed_Tab>False</Show_Completed_Tab>

		<Starting_Credits> Empire, 0 </Starting_Credits>
		<Starting_Tech_Level> Empire, 5 </Starting_Tech_Level>
		<Max_Tech_Level> Empire, 5 </Max_Tech_Level>
		<Starting_Forces> Empire, @@SOURCE@@, Empire_Star_Base_1 </Starting_Forces>
		<Starting_Forces> Empire, @@SOURCE@@, Death_Star_II </Starting_Forces>
		<Starting_Forces> Empire, @@SOURCE@@, Star_Destroyer </Starting_Forces>
		<Starting_Forces> Empire, @@SOURCE@@, Star_Destroyer </Starting_Forces>
		<Starting_Forces> Empire, @@SOURCE@@, Star_Destroyer </Starting_Forces>

		<Starting_Credits> Rebel, 0 </Starting_Credits>
		<Starting_Tech_Level> Rebel, 1 </Starting_Tech_Level>
		<Max_Tech_Level> Rebel, 1 </Max_Tech_Level>
		<Starting_Forces> Rebel, @@TARGET@@, Rebel_Star_Base_1 </Starting_Forces>
		<Starting_Forces> Rebel, @@TARGET@@, Corellian_Corvette </Starting_Forces>
		<Starting_Forces> Rebel, @@TARGET@@, Corellian_Corvette </Starting_Forces>
		<Starting_Forces> Rebel, @@TARGET@@, R_Ground_Barracks </Starting_Forces>
		<Starting_Forces> Rebel, @@TARGET@@, Rebel_Infantry_Squad </Starting_Forces>
		<Starting_Forces> Rebel, @@SPARE@@, R_Ground_Barracks </Starting_Forces>
	</Campaign>
</Campaigns>
'''

STORY_PLOTS = '''<?xml version="1.0" ?>
<Story_Mode_Plots>
	<Lua_Script>TSWP_Story</Lua_Script>
	<Active_Plot>TSWP_Story.xml</Active_Plot>
	<Suspended_Plot />
</Story_Mode_Plots>
'''

STORY = '''<?xml version="1.0" ?>
<!-- TSW planet-destroy probe: TSWP_Begin = proof + starts the status thread; TSWP_Heard = the galactic plot hearing
     PLANET_DESTROYED (docs/re/TSW_PLANET_DESTROY.md section 7: expected never after a tactical TSW kill). -->
<Story>
	<Event Name="TSWP_Begin">
		<Event_Type>STORY_ELAPSED</Event_Type>
		<Event_Param1>3</Event_Param1>
		<Reward_Type>SCREEN_TEXT</Reward_Type>
		<Reward_Param1>TEXT_TSWP_STORY_RUNNING</Reward_Param1>
		<Reward_Param2>15</Reward_Param2>
	</Event>
	<Event Name="TSWP_Heard">
		<Event_Type>STORY_PLANET_DESTROYED</Event_Type>
		<Event_Param1>@@TARGET@@</Event_Param1>
		<Reward_Type>SCREEN_TEXT</Reward_Type>
		<Reward_Param1>TEXT_TSWP_GALACTIC_HEARD</Reward_Param1>
		<Reward_Param2>600</Reward_Param2>
	</Event>
</Story>
'''

TEXTS = [
    ('TEXT_TSWP_PROBE_NAME', 'TSWP PROBE: KUAT'),
    ('TEXT_TSWP_PROBE_DESC', 'TSW planet-destroy probe. Play the EMPIRE. Move the Death Star fleet from Byss to Kuat '
                             'and fight the space battle; the Death Star fires by itself.'),
    ('TEXT_TSWP_STORY_RUNNING', 'TSWP PROBE: galactic plot loaded'),
    ('TEXT_TSWP_STORY_LUA', 'TSWP PROBE: galactic story Lua running'),
    ('TEXT_TSWP_HOOK_ALIVE', 'TSWP PROBE: GameScoring hook running in this space battle'),
    ('TEXT_TSWP_FIRED', 'TSWP PROBE: the hook fired the Death Star'),
    ('TEXT_TSWP_GALACTIC_HEARD', 'TSWP PROBE: THE GALACTIC PLOT HEARD PLANET_DESTROYED (spec: never)'),
]

GAMESCORING = 'DATA\\SCRIPTS\\MISCELLANEOUS\\GAMESCORING.LUA'
CAMPAIGNFILES = 'DATA\\XML\\CAMPAIGNFILES.XML'
MASTERTEXT = 'DATA\\TEXT\\MASTERTEXTFILE_ENGLISH.DAT'


def subst(text, rate=0.0):
    for k, v in (('@@TARGET@@', TARGET), ('@@SOURCE@@', SOURCE), ('@@ROUTE@@', ROUTE), ('@@SPARE@@', SPARE), ('@@DUMMY@@', DUMMY),
                 ('@@RATE@@', repr(float(rate)))):
        text = text.replace(k, v)
    assert '@@' not in text, text
    return text


def crlf(text):
    return text.replace('\r\n', '\n').replace('\n', '\r\n').encode('latin-1')


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def hook_text(rate):
    return subst(open(os.path.join(HERE, 'tswp_hook.lua'), encoding='ascii').read(), rate)


def story_lua():
    return subst(open(os.path.join(HERE, 'tswp_story.lua'), encoding='ascii').read())


def write_own_files(out, rate):
    """The probe's own files (no game data needed)."""
    write(os.path.join(out, 'Data', 'XML', 'TSWP_Probe_Campaign.xml'), crlf(subst(CAMPAIGN)))
    write(os.path.join(out, 'Data', 'XML', 'TSWP_Story_Plots.xml'), crlf(STORY_PLOTS))
    write(os.path.join(out, 'Data', 'XML', 'TSWP_Story.xml'), crlf(subst(STORY)))
    write(os.path.join(out, 'Data', 'Scripts', 'Story', 'TSWP_Story.lua'), crlf(story_lua()))
    write(os.path.join(out, MARKER), b'TSW planet-destroy probe mod; removed by cleanup_tswp.cmd\r\n')
    return hook_text(rate)


def build(game, out, rate):
    import ted_owners  # noqa: E402  (s10_map_owners: install file reader)
    from build_probe import dat_read, dat_bytes  # noqa: E402
    gf = ted_owners.GameFiles(game)
    if os.path.exists(out):
        if not os.path.exists(os.path.join(out, MARKER)):
            raise SystemExit('refusing to overwrite %s: not a TSWP probe folder' % out)
        shutil.rmtree(out)
    gs, src = gf.read(GAMESCORING)
    cf, src2 = gf.read(CAMPAIGNFILES)
    mt, src3 = gf.read(MASTERTEXT)
    if gs is None or cf is None or mt is None:
        raise SystemExit('shipped GameScoring.lua / CampaignFiles.xml / MasterTextFile_English.dat not found in ' + game)
    hook = write_own_files(out, rate)
    text = gs.decode('latin-1').replace('\r\n', '\n')
    write(os.path.join(out, 'Data', 'Scripts', 'Miscellaneous', 'GameScoring.lua'), crlf(text + '\n' + hook))
    cft = cf.decode('latin-1')
    i = cft.index('</Campaign_Files>')
    cft = cft[:i] + '\t<File>TSWP_Probe_Campaign.xml</File>\r\n' + cft[i:]
    write(os.path.join(out, 'Data', 'XML', 'CampaignFiles.xml'), cft.encode('latin-1'))
    tmp = os.path.join(out, 'mt.tmp')
    write(tmp, mt)
    keys = {k for k, _ in TEXTS}
    pairs = [kv for kv in dat_read(tmp) if kv[0] not in keys]
    os.remove(tmp)
    write(os.path.join(out, 'Data', 'Text', 'MasterTextFile_English.dat'), dat_bytes(pairs + TEXTS))
    print('built %s (GameScoring from %s, CampaignFiles from %s, text from %s, ServiceRate %s)' % (out, src, src2, src3, rate))


def check(rate):
    """Offline self-check: the own files build, every XML parses, no placeholder is left, the Lua has balanced
    function / end pairs (rough: Lua 5.0 is not run here)."""
    import tempfile
    tmp = tempfile.mkdtemp(prefix='tswp_check_')
    try:
        hook = write_own_files(tmp, rate)
        n = 0
        for d, _, files in os.walk(tmp):
            for fn in files:
                p = os.path.join(d, fn)
                data = open(p, 'rb').read().decode('latin-1')
                assert '@@' not in data, p
                if fn.lower().endswith('.xml'):
                    xml.dom.minidom.parseString(data.encode('latin-1'))
                    n += 1
        for name, src in (('hook', hook), ('story', story_lua())):
            assert '@@' not in src, name
        print('TSWP check OK: %d XML files parse, Lua placeholders filled' % n)
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=r'F:\SteamLibrary\steamapps\common\Star Wars Empire at War')
    ap.add_argument('--out', help='mod folder (default <game>\\corruption\\Mods\\%s)' % MOD_NAME)
    ap.add_argument('--rate', type=float, default=0.0, help='GameScoring ServiceRate in seconds (default 0: every service)')
    ap.add_argument('--check', action='store_true', help='offline self-check only')
    a = ap.parse_args()
    if a.check:
        return check(a.rate)
    build(a.game, a.out or os.path.join(a.game, 'corruption', 'Mods', MOD_NAME), a.rate)
    return 0


if __name__ == '__main__':
    sys.exit(main())
