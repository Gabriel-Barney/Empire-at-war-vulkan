#!/usr/bin/env python3
"""build_cu8h_mod.py - builds the cu8h probe mod (roadmap group cu8h, t01 / t02) from the installed game.

  build_cu8h_mod.py <session> [--game DIR] [--out MODDIR] [--source-zip extracted\\foc_data_text.zip] [--no-text]
                    [--config-out FILE]

Sessions (one mod folder, Mods\\CU8HProbe, rebuilt for each session; MODPATH=Mods\\CU8HProbe):
  galendor  t01 galactic half (CU8G_T02_ENV_PICK_RE sec. 7 P1): campaign "CU8H PROBE: ENDOR GALACTIC" with Abregado Rae
            (human Rebel: barracks, two infantry squads, a light tank brigade) and Jabiim (Empire: barracks, one
            stormtrooper squad; no AI, no fleet). Planets.xml: Jabiim's Land_Tactical_Map := _MP_LAND_ENDOR.ted, so the
            invasion loads the same map as the skirmish half through the campaign path (no reseed, 0x2a1eb0 after the
            pick). Files: CampaignFiles.xml, CU8H_Probe_Campaigns.xml, Planets.xml, texts. The synced draws are traced by
            cu8h_dbg.py (a debugger), not by the mod.
  luke      t02 rerun of the Saber_Throw half: as hum without Vader (HUM below, LUKE overrides).
  hum       t02 (CU8F_T01_T02_T07_RE sec. 2.5): land skirmish _MP_LAND_DAGOBAH, human EMPIRE vs AI REBEL. The AI Rebel free
            store spawns Darth_Vader for the human Empire in an open spot, leaves him idle, gives him a target, removes
            it, waits past the 15 s minimum drawn time; then Luke_Skywalker_Jedi (also for the human player) with
            SABER_THROW on autofire against a target. cu8h_run.py watches obj +0x3b9 (engine loops suppressed) and the
            HOLSTER_WEAPON behaviour state in the game's memory. Files: BusyTacticalFreeStore.lua (shipped + C8_CFG +
            ../cu8g_probes/cu8g_common.lua + cu8h_hum.lua), texts. No XML changed.

Shipped files are read from the install (../common/ted_owners.py GameFiles) or, offline, from the text zip.
"""
import argparse
import json
import os
import shutil
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PROBE_MOD = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(PROBE_MOD, 'cu8g_probes'))
sys.path.insert(0, os.path.join(PROBE_MOD, 'common'))
sys.path.insert(0, PROBE_MOD)
import build_cu8g_mod as g  # noqa: E402  (Source, land_site, lua_value, crlf, write, texts_dat, patch helpers)

MOD_NAME = 'CU8HProbe'
MARKER = 'CU8H_PROBE_MARKER.txt'
SESSIONS = ('galendor', 'hum', 'luke')
DEFAULT_GAME = g.DEFAULT_GAME
LAND_MAP = '_MP_LAND_DAGOBAH'
ENDOR_MAP = '_MP_LAND_ENDOR.ted'

HUM = {
    'human_faction': 'Empire',
    'vader': 'Darth_Vader', 'luke': 'Luke_Skywalker_Jedi', 'target': 'Rebel_Trooper',
    'target_alt': 'Rebel_Infantry_Squad',
    'start_frame': 300,        # 10 s after the battle starts
    'idle': 600,               # 20 s idle with no target
    'target_dx': 140.0,
    'engage': 300,             # 10 s with a target, Attack_Target every 30 frames
    'after': 750,              # 25 s after the target is removed (holster expected 450 frames after the last engaged frame)
    'luke_dy': 400.0, 'luke_target_dx': 150.0,
    'luke_idle': 150, 'luke_watch': 900, 'luke_fallback': 450, 'luke_after': 600,
}
# session luke (rerun of the Saber_Throw half): no Vader; Luke at 10 s, his target T2 at 15 s, the explicit throw order
# 15 frames later (Luke still about 150 units away, inside the 20..200 activation range). At luke_second (35 s after T2,
# past the 30 s recharge, Luke drawn and fighting T2 in melee) a second target T3 appears 120 units from T2, Luke is
# ordered onto T3 and, `luke_second_throw` frames later, to SABER_THROW it: a throw while drawn. 50 s with targets,
# 20 s after.
LUKE = dict(HUM, luke_only=True, luke_idle=150, luke_fallback=15, luke_second=1050, luke_second_dy=120.0,
            luke_second_throw=5, luke_watch=1500, luke_after=600)

TEXTS = {
    'galendor': [
        ('TEXT_CU8H_CAMP_NAME', 'CU8H PROBE: ENDOR GALACTIC'),
        ('TEXT_CU8H_CAMP_DESC', 'cu8h t01 probe. Play the REBEL. Move the ground units at Abregado Rae to Jabiim and choose '
                                'Begin. The battle loads _MP_LAND_ENDOR. Never save.'),
    ],
    'luke': [
        ('TEXT_CU8H_HUM_WAIT', 'CU8H PROBE: saber-throw probe armed. Give no orders; Luke appears at 10 s.'),
        ('TEXT_CU8H_HUM_LUKE', 'CU8H PROBE: Luke saber-throw part running. Give no orders.'),
        ('TEXT_CU8H_HUM_DONE', 'CU8H PROBE: lightsaber probe done. Quit the battle and the game.'),
    ],
    'hum': [
        ('TEXT_CU8H_HUM_WAIT', 'CU8H PROBE: lightsaber probe armed. Give no orders; Vader appears at 10 s.'),
        ('TEXT_CU8H_HUM_LUKE', 'CU8H PROBE: Vader part done; Luke saber-throw part running. Give no orders.'),
        ('TEXT_CU8H_HUM_DONE', 'CU8H PROBE: lightsaber probe done. Quit the battle and the game.'),
    ],
}

CAMPAIGN = r'''<?xml version="1.0"?>
<Campaigns>
	<!-- cu8h t01 probe (tools/probe_mod/cu8h_probes) -->
	<Campaign Name="CU8H_Probe_Endor">
		<Campaign_Set> CU8H_Probe_Endor_Set </Campaign_Set>
		<Sort_Order>0</Sort_Order>
		<Text_ID> TEXT_CU8H_CAMP_NAME </Text_ID>
		<Description_Text> TEXT_CU8H_CAMP_DESC </Description_Text>
		<Camera_Shift_X> 0.0 </Camera_Shift_X>
		<Camera_Shift_Y> 0.0 </Camera_Shift_Y>
		<Camera_Distance> 1200.0 </Camera_Distance>
		<Locations>
				Galaxy_Core_Art_Model,
				Abregado_Rae,
				Jabiim
		</Locations>
		<Trade_Routes>
				Abregado_Rae_Jabiim
		</Trade_Routes>
		<Home_Location> Rebel, Abregado_Rae </Home_Location>
		<Home_Location> Empire, Jabiim </Home_Location>
		<Starting_Active_Player> Rebel </Starting_Active_Player>
		<AI_Player_Control> Empire, None </AI_Player_Control>
		<AI_Player_Control> Underworld, None </AI_Player_Control>
		<AI_Player_Control> Pirates, None </AI_Player_Control>
		<Human_Victory_Conditions>
				Galactic_All_Planets_Controlled
		</Human_Victory_Conditions>
		<AI_Victory_Conditions>
				Galactic_All_Planets_Controlled
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
		<Starting_Credits> Empire, 0 </Starting_Credits>
		<Starting_Tech_Level> Empire, 1 </Starting_Tech_Level>
		<Max_Tech_Level> Empire, 1 </Max_Tech_Level>
		<Starting_Forces> Rebel, Abregado_Rae, R_Ground_Barracks </Starting_Forces>
		<Starting_Forces> Rebel, Abregado_Rae, Rebel_Infantry_Squad </Starting_Forces>
		<Starting_Forces> Rebel, Abregado_Rae, Rebel_Infantry_Squad </Starting_Forces>
		<Starting_Forces> Rebel, Abregado_Rae, Rebel_Light_Tank_Brigade </Starting_Forces>
		<Starting_Forces> Empire, Jabiim, E_Ground_Barracks </Starting_Forces>
		<Starting_Forces> Empire, Jabiim, Imperial_Stormtrooper_Squad </Starting_Forces>
	</Campaign>
</Campaigns>
'''


def prepare(out, label):
    if os.path.exists(out):
        if not os.path.exists(os.path.join(out, MARKER)):
            raise SystemExit('refusing to overwrite %s: not a cu8h probe folder' % out)
        shutil.rmtree(out)
    os.makedirs(out)
    g.write(os.path.join(out, MARKER), ('cu8h probe mod (session %s, tools/probe_mod/cu8h_probes); removed by '
                                        'cleanup_cu8h.cmd\r\n' % label).encode('ascii'))


def kit_text(path):
    return open(path, encoding='ascii').read()


def lua_config(cfg):
    return '-- generated by build_cu8h_mod.py (session %s)\nC8_CFG = %s\n' % (cfg['session'], g.lua_value(cfg, ''))


def freestore(src, cfg):
    shipped, fs_src = src.need(g.FREESTORE)
    text = shipped.decode('latin-1').replace('\r\n', '\n').rstrip('\n')
    tail = (lua_config(cfg) + '\n' + kit_text(os.path.join(PROBE_MOD, 'cu8g_probes', 'cu8g_common.lua')) + '\n' +
            kit_text(os.path.join(HERE, 'cu8h_hum.lua')))
    return g.crlf(text + '\n\n' + tail), fs_src


def patch_planets(data):
    text = data.decode('latin-1')
    text = g.patch_entity(text, 'Planet', 'Jabiim', lambda b: g.sub_once(
        b, r'<Land_Tactical_Map\s*>[^<]*</Land_Tactical_Map\s*>', '<Land_Tactical_Map> %s </Land_Tactical_Map>' % ENDOR_MAP,
        'Jabiim Land_Tactical_Map'))
    return text.encode('latin-1')


def build(session, game, out, source_zip=None, no_text=False):
    if session not in SESSIONS:
        raise SystemExit('unknown session ' + session)
    src = g.Source(game, source_zip)
    prepare(out, session)
    cfg = {'session': session}
    files = []

    def put(rel, data):
        g.write(os.path.join(out, rel), data)
        files.append(rel)

    if session == 'galendor':
        cf = src.need(g.CAMPAIGNFILES)[0].decode('latin-1')
        i = cf.index('</Campaign_Files>')
        put('Data\\XML\\CampaignFiles.xml', (cf[:i] + '\t<File>CU8H_Probe_Campaigns.xml</File>\r\n' + cf[i:]).encode('latin-1'))
        put('Data\\XML\\CU8H_Probe_Campaigns.xml', g.crlf(CAMPAIGN))
        put('Data\\XML\\Planets.xml', patch_planets(src.need(g.PLANETS)[0]))
        cfg.update({'map': 'campaign CU8H_Probe_Endor (Jabiim land map %s)' % ENDOR_MAP})
    else:
        site, note = g.land_site(game, LAND_MAP) if not src.zip else (dict(g.LAND_FALLBACK_SITE), 'fallback (offline)')
        cfg.update({'ai_faction': 'REBEL', 'map': LAND_MAP, 'hum': dict(LUKE if session == 'luke' else HUM, site=site), 'site_note': note})
        fs, fs_src = freestore(src, cfg)
        put('Data\\Scripts\\FreeStore\\BusyTacticalFreeStore.lua', fs)
        cfg['freestore_source'] = fs_src
    if not no_text:
        put('Data\\Text\\MasterTextFile_English.dat', g.texts_dat(src, TEXTS[session]))
    cfg['files'] = files
    return cfg


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('session', choices=SESSIONS)
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--out')
    ap.add_argument('--source-zip')
    ap.add_argument('--no-text', action='store_true')
    ap.add_argument('--config-out')
    a = ap.parse_args()
    out = a.out or os.path.join(a.game, 'corruption', 'Mods', MOD_NAME)
    cfg = build(a.session, a.game, out, a.source_zip, a.no_text)
    if a.config_out:
        with open(a.config_out, 'w') as f:
            json.dump(cfg, f, indent=1)
    print('built %s (%s): %s' % (out, a.session, ', '.join(cfg['files'])))
    if 'site_note' in cfg:
        print('site:', cfg['site_note'])
    return 0


if __name__ == '__main__':
    sys.exit(main())
