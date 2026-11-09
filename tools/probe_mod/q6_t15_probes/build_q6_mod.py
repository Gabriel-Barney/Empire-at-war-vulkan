#!/usr/bin/env python3
"""build_q6_mod.py - builds the two Q6 t15 probe mods from the installed game (and Thrawn's Revenge for the second).

  build_q6_mod.py vanilla [--game DIR] [--out MODDIR] [--source-zip extracted\\foc_data_text.zip] [--no-text]
  build_q6_mod.py tr      [--tr DIR] [--out MODDIR]

Nothing from the game or from TR is stored in the repository: the shipped files are read from the install (loose files
/ MEGs in the FoC order, through tools/probe_mod/common/ted_owners.py) or from TR's folder, and patched.

Mods\\Q6T15Probe (vanilla FoC, MODPATH=Mods\\Q6T15Probe):
  Data\\XML\\CampaignFiles.xml                    shipped list + Q6_Probe_Campaigns.xml
  Data\\XML\\Q6_Probe_Campaigns.xml               6 campaigns: Q6 PROBE: STORY TIMING, ELIM A .. E (human Empire)
  Data\\XML\\Q6_Story_Plots_<X>.xml, Q6_Story_<X>.xml    one plot per campaign (X = Timing, Elim_A .. Elim_E)
  Data\\Scripts\\Story\\Q6_Story_Timing.lua         q6_story_timing.lua (probes 3 and 4)
  Data\\Scripts\\Story\\Q6_Story_Elim_<A|B|C>.lua   q6_story_elim.lua, Q6E_MODE = A .. E (probe 1; D, E: q6 t25)
  Data\\Scripts\\FreeStore\\BusyTacticalFreeStore.lua   shipped + q6_tactical_hook.lua (probes 2 and 6)
  Data\\Scripts\\GameObject\\<script>.lua           shipped + q6_objscript_hook.lua, for OBJ_SCRIPTS (probe 2)
  Data\\Text\\MasterTextFile_English.dat           shipped text + TEXTS
  Q6T15_PROBE_MARKER.txt
Mods\\Q6T15ProbeTR (overlay in front of TR: MODPATH=Mods\\Q6T15ProbeTR MODPATH=<TR folder>):
  Data\\Scripts\\FreeStore\\LandTacticalFreeStore.lua   TR's own file + q6_tr_land_hook.lua (probe 5)
  Q6T15_PROBE_MARKER.txt
"""
import argparse
import os
import re
import shutil
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
sys.path.insert(0, os.path.dirname(HERE))
import ted_owners  # noqa: E402
from build_probe import dat_read, dat_bytes  # noqa: E402

MOD_NAME = 'Q6T15Probe'
TR_MOD_NAME = 'Q6T15ProbeTR'
MARKER = 'Q6T15_PROBE_MARKER.txt'
DEFAULT_TR = r'F:\SteamLibrary\steamapps\workshop\content\32470\1125571106'

# Object scripts the probe copies with the record hook: name -> ServiceRate it gets (None = shipped rate, 1 for all).
OBJ_SCRIPTS = {
    'ObjectScript_PowerToShields': None,   # space: Nebulon_B_Frigate (vanilla rate 1)
    'ObjectScript_MissileShield': None,    # land: AT_AA_Walker (vanilla rate 1)
    'ObjectScript_MC30': 6,                # space: MC30_Frigate
    'ObjectScript_SelfDestructLand': 6,    # land: TIE_Crawler
    'ObjectScript_PointDefense': 20,       # space: Crusader_Gunship; land: HAV_Juggernaut
    'Interdictor': 0.3,                    # space: Interdictor_Cruiser
}

CAMPAIGNFILES = 'DATA\\XML\\CAMPAIGNFILES.XML'
MASTERTEXT = 'DATA\\TEXT\\MASTERTEXTFILE_ENGLISH.DAT'
FREESTORE = 'DATA\\SCRIPTS\\FREESTORE\\BUSYTACTICALFREESTORE.LUA'

# Galaxy of every probe campaign: Tatooine, Byss, Abregado Rae, Jabiim (routes as the a1h probe). No space units, every
# non-human player AI None: nothing moves. The human is the Empire.
CAMPAIGN = r'''	<Campaign Name="Q6_Probe_{key}">
		<Campaign_Set> Q6_Probe_{key}_Set </Campaign_Set>
		<Sort_Order>0</Sort_Order>
		<Text_ID> TEXT_Q6_CAMP_{key}_NAME </Text_ID>
		<Description_Text> TEXT_Q6_CAMP_{key}_DESC </Description_Text>
		<Camera_Shift_X> 0.0 </Camera_Shift_X>
		<Camera_Shift_Y> 0.0 </Camera_Shift_Y>
		<Camera_Distance> 1200.0 </Camera_Distance>
		<Locations>
				Galaxy_Core_Art_Model,
				Tatooine,
				Byss,
				Abregado_Rae,
				Jabiim
		</Locations>
		<Trade_Routes>
				Byss_Tatooine,
				Abregado_Rae_Byss,
				Abregado_Rae_Jabiim
		</Trade_Routes>
		<Home_Location> Rebel, Jabiim </Home_Location>
		<Home_Location> Empire, Tatooine </Home_Location>
		<Starting_Active_Player> Empire </Starting_Active_Player>
		<Empire_Story_Name>Q6_Story_Plots_{key}.xml</Empire_Story_Name>
		<AI_Player_Control> Rebel, None </AI_Player_Control>
		<AI_Player_Control> Underworld, None </AI_Player_Control>
		<AI_Player_Control> Pirates, None </AI_Player_Control>
		<Human_Victory_Conditions>
				Galactic_All_Planets_Controlled
		</Human_Victory_Conditions>
		<AI_Victory_Conditions>
				{ai_victory}
		</AI_Victory_Conditions>
		<Markup_Filename>Empire, DefaultGalacticHints </Markup_Filename>
		<Markup_Filename>Pirates, DefaultGalacticHints </Markup_Filename>
		<Markup_Filename>Rebel, DefaultGalacticHints </Markup_Filename>
		<Markup_Filename>Underworld, DefaultGalacticHints </Markup_Filename>
		<Supports_Custom_Settings>False</Supports_Custom_Settings>
		<Show_Completed_Tab>False</Show_Completed_Tab>
		<Starting_Credits> Rebel, 0 </Starting_Credits>
		<Starting_Tech_Level> Rebel, 1 </Starting_Tech_Level>
		<Max_Tech_Level> Rebel, 1 </Max_Tech_Level>
		<Starting_Forces> Rebel, Jabiim, R_Ground_Barracks </Starting_Forces>
		<Starting_Credits> Empire, 0 </Starting_Credits>
		<Starting_Tech_Level> Empire, 1 </Starting_Tech_Level>
		<Max_Tech_Level> Empire, 1 </Max_Tech_Level>
{empire_forces}	</Campaign>
'''

# key -> (story Lua, AI victory condition, Empire starting forces, campaign name, description)
CAMPAIGNS = {
    'Timing': ('Q6_Story_Timing', 'Galactic_All_Planets_Controlled',
               ['Tatooine, E_Ground_Barracks', 'Tatooine, Imperial_Stormtrooper_Squad'],
               'Q6 PROBE: STORY TIMING',
               'Q6 t15 probes 3 and 4. Play the EMPIRE, click nothing, do not pause or change the game speed. Wait for '
               '"Q6 PROBE: story timing done" (about 2 minutes and 10 seconds).'),
    'Elim_A': ('Q6_Story_Elim_A', 'Galactic_All_Planets_Controlled', [],
               'Q6 PROBE: ELIM A (OWN NOTHING)',
               'Q6 t15 probe 1A. You own nothing. Click nothing for 3 minutes. Note whether the campaign ends.'),
    'Elim_B': ('Q6_Story_Elim_B', 'Galactic_All_Planets_Controlled', [],
               'Q6 PROBE: ELIM B (OWN, THEN LOSE)',
               'Q6 t15 probe 1B. You own nothing; after 20 s the script gives you Abregado Rae, after 50 s it takes it '
               'back. Click nothing for 3 minutes. Note whether and when the campaign ends.'),
    'Elim_C': ('Q6_Story_Elim_C', 'Galactic_Opponent_Controls_No_Planets', [],
               'Q6 PROBE: ELIM C (CONTROL)',
               'Q6 t15 probe 1C (control). You own nothing and the Rebel wins when its opponent owns no planet. '
               'Expected: the campaign ends at once.'),
    'Elim_D': ('Q6_Story_Elim_D', 'Galactic_All_Planets_Controlled', [],
               'Q6 PROBE: ELIM D (AI GAINS A PLANET)',
               'q6 t25. You own nothing; after 20 s the script gives the neutral Abregado Rae to the Rebel. Click nothing '
               'for 3 minutes. Note whether and when the campaign ends.'),
    'Elim_E': ('Q6_Story_Elim_E', 'Galactic_All_Planets_Controlled', ['Tatooine, Star_Destroyer'],
               'Q6 PROBE: ELIM E (FLEET, NO PLANET)',
               'q6 t25. You have a Star Destroyer at Tatooine; the script gives you Tatooine at 10 s and makes it '
               'neutral again at 40 s. Click nothing for 3 minutes. Note whether and when the campaign ends.'),
}

PLOTS = '''<?xml version="1.0" ?>
<Story_Mode_Plots>
	<Lua_Script>{lua}</Lua_Script>
	<Active_Plot>Q6_Story_{key}.xml</Active_Plot>
	<Suspended_Plot />
</Story_Mode_Plots>
'''


def elapsed_event(name, seconds, reward, perpetual=False):
    r = {'flag': '\t\t<Reward_Type>INCREMENT_FLAG</Reward_Type>\n\t\t<Reward_Param1>%s</Reward_Param1>\n'
                 '\t\t<Reward_Param2>1</Reward_Param2>\n',
         'text': '\t\t<Reward_Type>SCREEN_TEXT</Reward_Type>\n\t\t<Reward_Param1>%s</Reward_Param1>\n'
                 '\t\t<Reward_Param2>10</Reward_Param2>\n'}[reward[0]] % reward[1]
    return ('\t<Event Name="%s">\n\t\t<Event_Type>STORY_ELAPSED</Event_Type>\n\t\t<Event_Param1>%s</Event_Param1>\n%s%s'
            '\t</Event>\n') % (name, seconds, r, '\t\t<Perpetual>true</Perpetual>\n' if perpetual else '')


def story_xml(key):
    if key == 'Timing':
        ev = [elapsed_event('Q6S_Begin0', '0', ('text', 'TEXT_Q6_BEGIN0')),
              elapsed_event('Q6S_Begin3', '3', ('flag', 'Q6_BEGIN3')),
              elapsed_event('Q6S_Once2', '2', ('flag', 'Q6_ONCE2')),
              elapsed_event('Q6S_Cycle2', '2', ('flag', 'Q6_CYCLE2'), True),
              elapsed_event('Q6S_Cycle1', '1', ('flag', 'Q6_CYCLE1'), True),
              elapsed_event('Q6S_Cycle07', '0.7', ('flag', 'Q6_CYCLE07'), True),
              elapsed_event('Q6S_Cycle40', '40', ('flag', 'Q6_CYCLE40'), True)]
    else:
        ev = [elapsed_event('Q6E_Begin', '1', ('text', 'TEXT_Q6_ELIM_BEGIN'))]
    return '<?xml version="1.0" ?>\n<!-- Q6 t15 probe (tools/probe_mod/q6_t15_probes) -->\n<Story>\n%s</Story>\n' % ''.join(ev)


TEXTS = [
    ('TEXT_Q6_BEGIN0', 'Q6 PROBE: event Q6S_Begin0 (STORY_ELAPSED 0) fired'),
    ('TEXT_Q6_STORY_RUNNING', 'Q6 PROBE: story timing running. Click nothing, no pause, no speed change (about 2 min)'),
    ('TEXT_Q6_STORY_DONE', 'Q6 PROBE: story timing done. Quit to the main menu (do not save).'),
    ('TEXT_Q6_ELIM_BEGIN', 'Q6 PROBE: elimination plot loaded'),
    ('TEXT_Q6_ELIM_RUNNING_A', 'Q6 PROBE ELIM A: you own nothing. Click nothing; watch for 3 minutes.'),
    ('TEXT_Q6_ELIM_RUNNING_B', 'Q6 PROBE ELIM B: you own nothing; Abregado Rae comes at 20 s and goes at 50 s.'),
    ('TEXT_Q6_ELIM_RUNNING_C', 'Q6 PROBE ELIM C: control, the Rebel should win at once.'),
    ('TEXT_Q6_ELIM_GIVEN', 'Q6 PROBE ELIM B: the script gave you Abregado Rae'),
    ('TEXT_Q6_ELIM_TAKEN', 'Q6 PROBE ELIM B: the script took Abregado Rae back. You own nothing again.'),
    ('TEXT_Q6_ELIM_RUNNING_D', 'Q6 PROBE ELIM D: you own nothing; at 20 s the Rebel gets Abregado Rae.'),
    ('TEXT_Q6_ELIM_RUNNING_E', 'Q6 PROBE ELIM E: you have a fleet; Tatooine is yours at 10 s and neutral at 40 s.'),
    ('TEXT_Q6_ELIM_E_GIVEN', 'Q6 PROBE ELIM E: the script gave you Tatooine'),
    ('TEXT_Q6_ELIM_AI_GIVEN', 'Q6 PROBE ELIM D: the script gave Abregado Rae to the Rebel'),
    ('TEXT_Q6_ELIM_E_TAKEN', 'Q6 PROBE ELIM E: the script made Tatooine neutral. You own a fleet and no planet.'),
    ('TEXT_Q6_ELIM_DONE', 'Q6 PROBE ELIM: 170 s passed and the campaign is still running. Quit (do not save).'),
    ('TEXT_Q6_OBJ_WAVE1', 'Q6 PROBE: object-script test units spawned (wave 1)'),
    ('TEXT_Q6_SEL_READY_LAND', 'Q6 PROBE: selection test set (land) is next to your units. Press Ctrl+A, then screenshot.'),
    ('TEXT_Q6_SEL_READY_SPACE', 'Q6 PROBE: selection test set (space) is next to your fleet. Press Ctrl+A, then screenshot.'),
    ('TEXT_Q6_TACT_DONE', 'Q6 PROBE: tactical part done (2 min). Do the last Ctrl+A screenshot, then quit the battle.'),
] + [('TEXT_Q6_CAMP_%s_NAME' % k, v[3]) for k, v in CAMPAIGNS.items()] + \
    [('TEXT_Q6_CAMP_%s_DESC' % k, v[4]) for k, v in CAMPAIGNS.items()]


class Source:
    """Shipped files from the install (GameFiles) or from a text zip (offline check, no .dat inside)."""

    def __init__(self, game, source_zip=None):
        self.zip = zipfile.ZipFile(source_zip) if source_zip else None
        self.names = {n.upper().replace('/', '\\'): n for n in self.zip.namelist()} if self.zip else None
        self.gf = None if self.zip else ted_owners.GameFiles(game)

    def read(self, name):
        if self.zip:
            n = self.names.get(name.upper())
            return (self.zip.read(n), 'zip:' + n) if n else (None, None)
        return self.gf.read(name)


def crlf(text):
    return text.replace('\r\n', '\n').replace('\n', '\r\n').encode('latin-1')


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def hook(name):
    return open(os.path.join(HERE, name), encoding='ascii').read()


def appended(shipped, hook_text):
    text = shipped.decode('latin-1').replace('\r\n', '\n')
    return crlf(text.rstrip('\n') + '\n\n' + hook_text)


def prepare(out, label):
    if os.path.exists(out):
        if not os.path.exists(os.path.join(out, MARKER)):
            raise SystemExit('refusing to overwrite %s: not a Q6 t15 probe folder' % out)
        shutil.rmtree(out)
    os.makedirs(out)
    write(os.path.join(out, MARKER), ('Q6 t15 probe mod (%s, tools/probe_mod/q6_t15_probes); removed by cleanup_q6.cmd\r\n'
                                      % label).encode('ascii'))


def build_vanilla(game, out, source_zip=None, no_text=False):
    src = Source(game, source_zip)
    prepare(out, 'vanilla')
    cf, cf_src = src.read(CAMPAIGNFILES)
    if cf is None:
        raise SystemExit('shipped CampaignFiles.xml not found')
    cft = cf.decode('latin-1')
    i = cft.index('</Campaign_Files>')
    cft = cft[:i] + '\t<File>Q6_Probe_Campaigns.xml</File>\r\n' + cft[i:]
    write(os.path.join(out, 'Data', 'XML', 'CampaignFiles.xml'), cft.encode('latin-1'))
    camps = []
    for key, (lua, ai_victory, forces, _, _) in CAMPAIGNS.items():
        ef = ''.join('\t\t<Starting_Forces> Empire, %s </Starting_Forces>\n' % f for f in forces)
        camps.append(CAMPAIGN.format(key=key, ai_victory=ai_victory, empire_forces=ef))
        write(os.path.join(out, 'Data', 'XML', 'Q6_Story_Plots_%s.xml' % key), crlf(PLOTS.format(lua=lua, key=key)))
        write(os.path.join(out, 'Data', 'XML', 'Q6_Story_%s.xml' % key), crlf(story_xml(key)))
    write(os.path.join(out, 'Data', 'XML', 'Q6_Probe_Campaigns.xml'),
          crlf('<?xml version="1.0"?>\n<Campaigns>\n\t<!-- Q6 t15 probes (tools/probe_mod/q6_t15_probes) -->\n%s'
               '</Campaigns>\n' % ''.join(camps)))
    story = os.path.join(out, 'Data', 'Scripts', 'Story')
    write(os.path.join(story, 'Q6_Story_Timing.lua'), crlf(hook('q6_story_timing.lua')))
    elim = hook('q6_story_elim.lua')
    for m in 'ABCDE':
        write(os.path.join(story, 'Q6_Story_Elim_%s.lua' % m), crlf(elim.replace('@@MODE@@', m)))
    fs, fs_src = src.read(FREESTORE)
    if fs is None:
        raise SystemExit('shipped BusyTacticalFreeStore.lua not found')
    write(os.path.join(out, 'Data', 'Scripts', 'FreeStore', 'BusyTacticalFreeStore.lua'),
          appended(fs, hook('q6_tactical_hook.lua')))
    oh = hook('q6_objscript_hook.lua')
    for name, rate in OBJ_SCRIPTS.items():
        data, _ = src.read('DATA\\SCRIPTS\\GAMEOBJECT\\%s.LUA' % name.upper())
        if data is None:
            raise SystemExit('shipped object script %s.lua not found' % name)
        text = oh.replace('@@SCRIPT@@', name).replace('@@RATE@@', 'nil' if rate is None else repr(float(rate)))
        write(os.path.join(out, 'Data', 'Scripts', 'GameObject', '%s.lua' % name), appended(data, text))
    n_text = 0
    if not no_text:
        mt, mt_src = src.read(MASTERTEXT)
        if mt is None:
            raise SystemExit('shipped MasterTextFile_English.dat not found (use --no-text for an offline check)')
        tmp = os.path.join(out, 'mt.tmp')
        write(tmp, mt)
        keys = {k for k, _ in TEXTS}
        pairs = [kv for kv in dat_read(tmp) if kv[0] not in keys]
        os.remove(tmp)
        write(os.path.join(out, 'Data', 'Text', 'MasterTextFile_English.dat'), dat_bytes(pairs + TEXTS))
        n_text = len(TEXTS)
    print('built %s (CampaignFiles from %s, free store from %s, %d object scripts, %d probe strings)' % (
        out, cf_src, fs_src, len(OBJ_SCRIPTS), n_text))


def tr_factions(tr):
    p = os.path.join(tr, 'Data', 'XML', 'Factions.xml')
    names = re.findall(r'<Faction\s+Name\s*=\s*"([^"]+)"', open(p, encoding='latin-1').read())
    return [n for n in names if n.upper() != 'NEUTRAL']


def build_tr(tr, out):
    fs = os.path.join(tr, 'Data', 'Scripts', 'FreeStore', 'LandTacticalFreeStore.lua')
    if not os.path.isfile(fs):
        raise SystemExit('TR LandTacticalFreeStore.lua not found under ' + tr)
    prepare(out, 'Thrawn\'s Revenge overlay')
    facs = tr_factions(tr)
    text = hook('q6_tr_land_hook.lua').replace('@@FACTIONS@@', ', '.join('"%s"' % f for f in facs))
    write(os.path.join(out, 'Data', 'Scripts', 'FreeStore', 'LandTacticalFreeStore.lua'),
          appended(open(fs, 'rb').read(), text))
    print('built %s (TR LandTacticalFreeStore.lua from %s, %d factions)' % (out, fs, len(facs)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('which', choices=('vanilla', 'tr'))
    ap.add_argument('--game', default=ted_owners.DEFAULT_GAME)
    ap.add_argument('--tr', default=DEFAULT_TR)
    ap.add_argument('--out')
    ap.add_argument('--source-zip')
    ap.add_argument('--no-text', action='store_true')
    a = ap.parse_args()
    if a.which == 'vanilla':
        build_vanilla(a.game, a.out or os.path.join(a.game, 'corruption', 'Mods', MOD_NAME), a.source_zip, a.no_text)
    else:
        build_tr(a.tr, a.out or os.path.join(a.game, 'corruption', 'Mods', TR_MOD_NAME))


if __name__ == '__main__':
    main()
