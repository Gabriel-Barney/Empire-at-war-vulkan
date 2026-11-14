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
  luke      t02 rerun of the Saber_Throw half: as hum without Vader (HUM below, LUKE overrides). cu8k t05 (CU8F sec. 2.7
            item 6) reruns it with cu8h_run.py's watcher also logging the thrown saber (position, travelled, max travel).
  kashyyyk  cu8k t05 (MOVEMENT sec. 4.7.6 items 1 and 3): campaign "CU8K PROBE: KASHYYYK SPACE" with Corellia (human
            Rebel fleet: Calamari_Cruiser, Nebulon_B_Frigate, two Rebel_X-Wing_Squadron) and Kashyyyk (Empire:
            Empire_Star_Base_2; no AI, no fleet), route Corellia_Kashyyyk. Moving the fleet to Kashyyyk starts a
            galactic space battle on _Space_Planet_Kashyyyk_01.ted (the campaign spawn 0x2a1eb0 path). Files:
            CampaignFiles.xml, CU8K_Probe_Campaigns.xml, texts. The draws are traced by cu8h_dbg.py.
  jetpack   cu8k t05 (MOVEMENT sec. 4.7.6 item 4, sec. 4.7.4): land skirmish _MP_LAND_DAGOBAH, human EMPIRE vs AI REBEL.
            The AI Rebel free store spawns three Dark_Trooper_PhaseII (JET_PACK, secondary JETPACK_LOCOMOTOR) for the
            human, records C8G|DBGARM| (cu8h_run.py then arms the debugger), and 8 s later runs three trials: A moves
            and activates JET_PACK while moving (the swap re-issues the move), B activates it standing (the hold), C
            moves and activates it with a position (as the shipped scripts do); then C8G|DBGSTOP|. Files:
            BusyTacticalFreeStore.lua (shipped + C8_CFG + ../cu8g_probes/cu8g_common.lua + cu8k_jetpack.lua), texts.
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
SESSIONS = ('galendor', 'hum', 'luke', 'kashyyyk', 'jetpack')
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
# session jetpack (cu8k t05, MOVEMENT sec. 4.7.6 item 4): three Dark_Trooper_PhaseII 120 units apart at the site, spawned
# at 10 s; DBGARM 1 s later; trial A 8 s after DBGARM (the arm file reaches the debugger about 100 frames late; move
# 350 units east, JET_PACK to that goal 15 frames later), trial B 3 s later (JET_PACK standing, to a point 300 units
# east), trial C 3 s later (move, then JET_PACK at a point 300 units east); DBGSTOP 7 s after C. JET_PACK targets are
# marker objects ('target', fallback 'target_fallback') spawned with the units, as in the shipped scripts: a bool
# (run 1) and a Create_Position target (rerun, 2026-11-09 06:29) both ended the probe script at the activation.
JET = {
    'human_faction': 'Empire', 'unit': 'Dark_Trooper_PhaseII', 'ability': 'JET_PACK',
    'start_frame': 300, 'arm_after': 30, 'arm_lead': 240, 'spacing_y': 120.0, 'move_dx': 350.0, 'jet_delay': 15,
    'jump_dx': 300.0, 'trial_gap': 90, 'after': 210, 'end_after': 60, 'poll_every': 15,
    'target': 'Story_Trigger_Zone', 'target_fallback': 'Generic_Marker_Land',
}

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
    'kashyyyk': [
        ('TEXT_CU8K_CAMP_NAME', 'CU8K PROBE: KASHYYYK SPACE'),
        ('TEXT_CU8K_CAMP_DESC', 'cu8k t05 probe. Play the REBEL. Move the fleet at Corellia to Kashyyyk and choose '
                                'Begin. The battle loads _Space_Planet_Kashyyyk_01. Never save.'),
    ],
    'jetpack': [
        ('TEXT_CU8K_JET_WAIT', 'CU8K PROBE: jetpack probe armed. Give no orders; three Dark Troopers appear at 10 s.'),
        ('TEXT_CU8K_JET_ARM', 'CU8K PROBE: debugger arming; the jetpack trials start in 8 s. Give no orders.'),
        ('TEXT_CU8K_JET_DONE', 'CU8K PROBE: jetpack probe done. Quit the battle and the game.'),
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


CAMPAIGN_KASHYYYK = r'''<?xml version="1.0"?>
<Campaigns>
	<!-- cu8k t05 probe (tools/probe_mod/cu8h_probes, session kashyyyk) -->
	<Campaign Name="CU8K_Probe_Kashyyyk">
		<Campaign_Set> CU8K_Probe_Kashyyyk_Set </Campaign_Set>
		<Sort_Order>0</Sort_Order>
		<Text_ID> TEXT_CU8K_CAMP_NAME </Text_ID>
		<Description_Text> TEXT_CU8K_CAMP_DESC </Description_Text>
		<Camera_Shift_X> 0.0 </Camera_Shift_X>
		<Camera_Shift_Y> 0.0 </Camera_Shift_Y>
		<Camera_Distance> 1200.0 </Camera_Distance>
		<Locations>
				Galaxy_Core_Art_Model,
				Corellia,
				Kashyyyk
		</Locations>
		<Trade_Routes>
				Corellia_Kashyyyk
		</Trade_Routes>
		<Home_Location> Rebel, Corellia </Home_Location>
		<Home_Location> Empire, Kashyyyk </Home_Location>
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
		<Starting_Forces> Rebel, Corellia, Calamari_Cruiser </Starting_Forces>
		<Starting_Forces> Rebel, Corellia, Nebulon_B_Frigate </Starting_Forces>
		<Starting_Forces> Rebel, Corellia, Rebel_X-Wing_Squadron </Starting_Forces>
		<Starting_Forces> Rebel, Corellia, Rebel_X-Wing_Squadron </Starting_Forces>
		<Starting_Forces> Empire, Kashyyyk, Empire_Star_Base_2 </Starting_Forces>
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


def freestore(src, cfg, hook='cu8h_hum.lua'):
    shipped, fs_src = src.need(g.FREESTORE)
    text = shipped.decode('latin-1').replace('\r\n', '\n').rstrip('\n')
    tail = (lua_config(cfg) + '\n' + kit_text(os.path.join(PROBE_MOD, 'cu8g_probes', 'cu8g_common.lua')) + '\n' +
            kit_text(os.path.join(HERE, hook)))
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
    elif session == 'kashyyyk':
        cf = src.need(g.CAMPAIGNFILES)[0].decode('latin-1')
        i = cf.index('</Campaign_Files>')
        put('Data\\XML\\CampaignFiles.xml', (cf[:i] + '\t<File>CU8K_Probe_Campaigns.xml</File>\r\n' + cf[i:]).encode('latin-1'))
        put('Data\\XML\\CU8K_Probe_Campaigns.xml', g.crlf(CAMPAIGN_KASHYYYK))
        cfg.update({'map': 'campaign CU8K_Probe_Kashyyyk (Kashyyyk space map _Space_Planet_Kashyyyk_01.ted)'})
    elif session == 'jetpack':
        site, note = g.land_site(game, LAND_MAP) if not src.zip else (dict(g.LAND_FALLBACK_SITE), 'fallback (offline)')
        cfg.update({'ai_faction': 'REBEL', 'map': LAND_MAP, 'jet': dict(JET, site=site), 'site_note': note})
        fs, fs_src = freestore(src, cfg, 'cu8k_jetpack.lua')
        put('Data\\Scripts\\FreeStore\\BusyTacticalFreeStore.lua', fs)
        cfg['freestore_source'] = fs_src
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
