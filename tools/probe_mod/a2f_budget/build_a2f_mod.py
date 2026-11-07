#!/usr/bin/env python3
"""build_a2f_mod.py - builds the a2f t07 budget probe mod (Mods\\A2fBudgetProbe) from the installed game's own files.

  build_a2f_mod.py [--game DIR] [--out MODDIR] [--source-zip extracted\\foc_data_text.zip] [--no-mod-equations]
                   [--no-percept] [--saboteur [--no-sab-desire]]

Nothing from the game is stored in the repository: every shipped file is read from the install (loose files / MEGs, FoC
order, through tools/probe_mod/common/ted_owners.py; the extracted text zip is the fallback) and patched:
  Data\\XML\\AI\\PerceptualEquations\\AI_Equations_ExpansionGeneric_Galactic.xml   shipped + the A2FB_* probe equations
  Data\\XML\\Story_Plots_Sandbox_12U_{Rebel,Underworld,Empire}.xml   Lua_Script = A2fb_Story (human faction's plot)
  Data\\Scripts\\Story\\A2fb_Story.lua                    header + core + a2fb_story.lua (channel S)
  Data\\Scripts\\FreeStore\\GalacticFreeStore.lua        shipped + header + core + a2fb_freestore_hook.lua (channel F)
  Data\\Scripts\\AI\\<plan>.lua x 12                      shipped + header + core + a2fb_plan_hook.lua (channel P)
                                                        (+ a2fb_saboteur_plan.lua for AI_Plan_Underworld_DeploySaboteur)
  Data\\XML\\Story_Plots_Underworld_AI_Heroes.xml        a2j t02 (D4): shipped + Lua_Script = A2fb_PerceptU (Underworld AI plot)
  Data\\XML\\Story_Plots_Empire_AI_Heroes_EXP.xml        a2j t02 (D4): shipped + Lua_Script = A2fb_PerceptE (Empire AI plot)
  Data\\Scripts\\Story\\A2fb_Percept{U,E}.lua            header + pair table + core + a2fb_percept.lua (channel D)
  Data\\Scripts\\Miscellaneous\\GameScoring.lua          a2j t02 (D3): shipped + header + core + a2fb_scoring_hook.lua (channel Q)
  Data\\Text\\MasterTextFile_English.dat                 shipped + TEXT_A2FB_LOADED / _DONE / _LONG_DONE / _MIN30 (when readable)
  A2FB_Canary_{Story,FreeStore,Plan,PerceptU,PerceptE,Scoring}.txt   _CustomScriptMessage deletes each on its first call
  A2FB_TYPES.json                                       cost / build time of the probe structure types
  A2FB_QUEUE_TYPES.json                                 a2j: object type -> ground / space production queue (report D3)
  A2FB_PROBE_MARKER.txt                                 lets cleanup recognise the folder as ours
--saboteur (a2k t05, run_a2f_sab.cmd): the short saboteur run. Channel D is left out; instead the Underworld AI plot
(Story_Plots_Underworld_AI_Heroes.xml) gets Lua_Script = A2fb_SabU (Data\\Scripts\\Story\\A2fb_SabU.lua, channel U:
a2fb_saboteur.lua), the free store gets its saboteur records (FSAB / FADD / FSVC) and the story script shows
TEXT_A2FB_SAB_DONE after frame SAB_STOP. --no-sab-desire leaves out channel U's desire evaluations (UD records).
--no-mod-equations builds the Lua without the A2FB_* equation calls (and leaves the equation file alone): use it
if a run showed that the game does not load the mod's equation file (the report says so).
"""
import argparse
import json
import os
import re
import shutil
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PROBE_MOD = os.path.dirname(HERE)
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(PROBE_MOD, 'common'))
sys.path.insert(0, PROBE_MOD)
import a2fb_spec  # noqa: E402

MOD_NAME = 'A2fBudgetProbe'
MARKER = 'A2FB_PROBE_MARKER.txt'
DEFAULT_GAME = r'F:\SteamLibrary\steamapps\common\Star Wars Empire at War'
MASTERTEXT = 'DATA\\TEXT\\MASTERTEXTFILE_ENGLISH.DAT'
FREESTORE = 'DATA\\SCRIPTS\\FREESTORE\\GALACTICFREESTORE.LUA'
SCORING = 'DATA\\SCRIPTS\\MISCELLANEOUS\\GAMESCORING.LUA'
GAMEOBJECTFILES = 'DATA\\XML\\GAMEOBJECTFILES.XML'
# a2j D3: which planet production queue an object type is built in, by its XML element (the report's queue-head table)
QUEUE_TAGS = {'space': {'SpaceUnit', 'Squadron', 'StarBase'},
              'ground': {'GroundCompany', 'SpecialStructure', 'GroundStructure', 'GroundInfantry', 'GroundVehicle',
                         'GroundBuildable', 'HeroCompany'}}
PLOTS = ['Rebel', 'Underworld', 'Empire']
# (shipped plan script name, task-force thread function the hook wraps). Infrastructure plans + the Empire tech plan
# (category MajorItem). Verified against the shipped scripts at build time.
PLANS = [
    ('BuildBaseComponentBarracks', 'BaseForce_Thread'),
    ('BuildBaseComponentLightVehicleFactory', 'BaseForce_Thread'),
    ('BuildBaseComponentResearchFacility', 'BaseForce_Thread'),
    ('UpgradeStarbasePlan', 'BaseForce_Thread'),
    ('BuildEconomicStructurePlan', 'StructureForce_Thread'),
    ('EmpireAdvanceTechPlan', 'TechForce_Thread'),
    # a2h t02 (A2G_T02_T04_PORT_VS_RETAIL.md section 4): the plans the port releases / runs that retail had no count for
    ('BuildBaseComponentHeavyVehicleFactory', 'BaseForce_Thread'),
    ('BuildBaseComponentAdvancedVehicleFactory', 'BaseForce_Thread'),
    ('LiftBlockadePlan', 'MainForce_Thread'),
    ('FlushMajorItemBudget', 'MainForce_Thread'),
    ('ConquerPiratePlan', 'MainForce_Thread'),
    # a2k t05 (A2K_T08_RE_ANSWERS.md section 5): the Underworld saboteur plan (Corrupt_Planet / Sabotage_Planet)
    ('AI_Plan_Underworld_DeploySaboteur', 'MainForce_Thread'),
]
# a2k t05: plan-specific extensions appended after a2fb_plan_hook.lua (extra PSTART / end keys, progress records)
PLAN_EXTRA = {'AI_Plan_Underworld_DeploySaboteur': 'a2fb_saboteur_plan.lua'}
# a2k t05 saboteur run (--saboteur): what channel U and the free-store hook look for, and when
SAB_TYPE = 'Underworld_Saboteur_Team'
SAB_STEP = 30          # frames between channel U samples
SAB_STOP = 1400        # last frame channel U samples; the story script shows TEXT_A2FB_SAB_DONE from SAB_STOP + 10
SAB_DES_STOP = 600     # last frame of the UD desire samples (the pass near frame 116 and the next ones, ~150 frames each)
SAB_PLANETS = ['Corellia', 'Felucia', 'Kamino', 'Kashyyyk', 'Kuat', 'MonCalimari', 'Naboo', 'NalHutta', 'Shola',
               'Tatooine', 'The_Maw', 'Yavin']       # the Locations of Sandbox_Heart_Of_The_Maw_Rebel
SAB_PLOT = 'Story_Plots_Underworld_AI_Heroes.xml'
SAB_SCRIPT = 'A2fb_SabU'

# Plans whose start and end samples are full ones (all BFB categories: the MajorItem pool before / after the flush).
FULL_SAMPLE_PLANS = {'FlushMajorItemBudget'}
SPECIAL_STRUCTURES = ['DATA\\XML\\SPECIALSTRUCTURES.XML', 'DATA\\XML\\SPECIALSTRUCTURES_UNDERWORLD.XML']
TEXTS = [
    ('TEXT_A2FB_LOADED', 'A2FB PROBE: story script running. Do not touch the game; keep it running for 30 minutes.'),
    ('TEXT_A2FB_DONE', 'A2FB PROBE: 5-minute dense phase done. Keep the game running (30 s samples continue).'),
    ('TEXT_A2FB_LONG_DONE', 'A2FB PROBE: 25 minutes recorded. Keep the game running until the 30-minute message.'),
    ('TEXT_A2FB_MIN30', 'A2FB PROBE: 30 minutes recorded. You can quit now (Esc > Quit to the main menu > Quit).'),
    ('TEXT_A2FB_SAB_DONE', 'A2FB PROBE: saboteur run recorded (frame 1400). You can quit now (Esc > Quit to the main '
                           'menu > Quit).'),
]
CANARIES = {'Story': 'A2FB_Canary_Story.txt', 'FreeStore': 'A2FB_Canary_FreeStore.txt', 'Plan': 'A2FB_Canary_Plan.txt',
            'PerceptU': 'A2FB_Canary_PerceptU.txt', 'PerceptE': 'A2FB_Canary_PerceptE.txt',
            'Scoring': 'A2FB_Canary_Scoring.txt', 'Saboteur': 'A2FB_Canary_Saboteur.txt'}


def canary_built(ch, percept=True, saboteur=False):
    """True when the build writes channel ch's canary (Percept* without --no-percept / --saboteur, Saboteur with it)."""
    if ch.startswith('Percept'):
        return percept and not saboteur
    if ch == 'Saboteur':
        return saboteur
    return True


def crlf(text):
    return text.replace('\r\n', '\n').replace('\n', '\r\n').encode('latin-1')


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


class Source:
    """Shipped Data\\ files: the install first, the extracted text zip as the fallback."""

    def __init__(self, game, zip_path):
        self.gf = None
        self.zip = None
        self.zip_names = {}
        if game and os.path.isdir(os.path.join(game, 'corruption')):
            import ted_owners
            self.gf = ted_owners.GameFiles(game)
        if zip_path and os.path.isfile(zip_path):
            self.zip = zipfile.ZipFile(zip_path)
            self.zip_names = {i.filename.replace('/', '\\').upper(): i for i in self.zip.infolist()}

    def read(self, name):
        """(bytes, where) or (None, None). name: upper case, backslashes."""
        if self.gf is not None:
            data, where = self.gf.read(name)
            if data is not None:
                return data, where
        if self.zip is not None and name in self.zip_names:
            return self.zip.read(self.zip_names[name]), 'zip'
        return None, None

    def need(self, name):
        data, where = self.read(name)
        if data is None:
            raise SystemExit('shipped file %s not found (install or extracted zip)' % name)
        return data, where


def read_text(name):
    return open(os.path.join(HERE, name), encoding='ascii').read()


def sab_head(desire=True):
    """a2k t05: Lua text of the saboteur-run switch (A2FB_SAB) and its configuration table A2FB_SABCFG."""
    return ('A2FB_SAB = true\nA2FB_SABCFG = { type = "%s", step = %d, stop = %d, des_stop = %d, planets = { %s } }\n'
            % (SAB_TYPE, SAB_STEP, SAB_STOP, SAB_DES_STOP if desire else -1,
               ', '.join('"%s"' % p for p in SAB_PLANETS)))


def assemble(orig, channel, hook, use_mod_eq, subs=None, extra=None, head_extra=''):
    """orig (str or None) + generated header (+ head_extra) + core + hook (+ the extra hook files), LF text (crlf()
    applied on write)."""
    hook = read_text(hook)
    for name in (extra or []):
        hook = hook + read_text(name)
    for k, v in (subs or {}).items():
        hook = hook.replace(k, v)
    head = a2fb_spec.lua_header(use_mod_eq) + head_extra + 'A2FB_CH = "%s"\n' % channel
    body = head + read_text('a2fb_core.lua') + hook
    if orig is None:
        return body
    orig = orig.replace('\r\n', '\n')
    if not orig.endswith('\n'):
        orig += '\n'
    return orig + '\n' + body


def plan_subs(name, thread):
    """Substitutions of a2fb_plan_hook.lua for one plan: name, thread function, task force, full-sample flag."""
    return {'@@PLAN@@': name, '@@THREAD@@': thread, '@@FORCE@@': thread[:-len('_Thread')],
            '@@FULL@@': 'true' if name in FULL_SAMPLE_PLANS else 'false'}


def story_lua(use_mod_eq=True, saboteur=False):
    return assemble(None, 'S', 'a2fb_story.lua', use_mod_eq, head_extra=sab_head() if saboteur else '')


def sab_lua(use_mod_eq=True, desire=True):
    """a2k t05: the channel-U plot script (Underworld AI plot) of the saboteur run."""
    return assemble(None, 'U', 'a2fb_saboteur.lua', use_mod_eq, head_extra=sab_head(desire))


def plan_lua(orig, name, thread, use_mod_eq=True):
    """A shipped plan script + header + core + a2fb_plan_hook.lua (+ its PLAN_EXTRA file)."""
    extra = [PLAN_EXTRA[name]] if name in PLAN_EXTRA else []
    return assemble(orig, 'P', 'a2fb_plan_hook.lua', use_mod_eq, plan_subs(name, thread), extra)


def percept_lua(fkey, use_mod_eq=True):
    """a2j t02 (D4): the channel-D plot script of one (faction, planet) pair of a2fb_spec.PERC_PAIRS."""
    pair = [p for p in a2fb_spec.PERC_PAIRS if p[0] == fkey][0]
    hook = read_text('a2fb_percept.lua').replace('@@SUF@@', pair[3])
    head = a2fb_spec.lua_header(use_mod_eq) + a2fb_spec.perc_lua(fkey, use_mod_eq) + 'A2FB_CH = "D"\n'
    return head + read_text('a2fb_core.lua') + hook


def ai_plot(shipped_text, script):
    """The shipped AI story plot file (it has no Lua_Script) with <Lua_Script>script</Lua_Script> as its first entry."""
    if re.search(r'<Lua_Script>\s*\S', shipped_text, re.I):
        raise SystemExit('the shipped AI plot file already has a Lua_Script: the probe would replace it')
    t = re.sub(r'<Lua_Script>\s*</Lua_Script>\s*', '', shipped_text, flags=re.I)
    m = re.search(r'<Story_Mode_Plots>', t, re.I)
    if not m:
        raise SystemExit('AI plot file without <Story_Mode_Plots>')
    nl = '\r\n' if '\r\n' in t else '\n'
    return t[:m.end()] + nl + '\t<Lua_Script>%s</Lua_Script>' % script + t[m.end():]


def queue_types(src):
    """{TYPE NAME: 'ground' | 'space'} from the game object files (GameObjectFiles.xml), by XML element."""
    data, _ = src.read(GAMEOBJECTFILES)
    out = {}
    if data is None:
        return out
    text = re.sub(r'<!--.*?-->', '', data.decode('latin-1'), flags=re.S)
    for fn in re.findall(r'<File>\s*([^<\s]+)\s*</File>', text):
        d, _ = src.read('DATA\\XML\\' + fn.upper())
        if d is None:
            continue
        for tag, name in re.findall(r'<(\w+)\s+Name\s*=\s*"([^"]+)"', d.decode('latin-1')):
            for q, tags in QUEUE_TAGS.items():
                if tag in tags:
                    out.setdefault(name.upper(), q)
    return out


def build(game, out, zip_path=None, use_mod_eq=True, percept=True, saboteur=False, sab_desire=True):
    """percept=False leaves channel D out (no AI plot scripts, no EvaluatePerception from them). Its evaluations share
    the AI's per-token leaf cache (AI_TACTICAL_PERCEPTION section 1.2), so they can shift when the AI's cached values are
    refreshed: in the port that changed the run from frame 1460 (A2J_T02_PROBE_PLAN.md section 3). Build without it
    for a budget run (t04) that the D4 reads cannot perturb.
    saboteur=True (a2k t05) implies percept=False and adds channel U and the saboteur records (module docstring);
    sab_desire=False leaves channel U's desire evaluations out."""
    if saboteur:
        percept = False
    src = Source(game, zip_path)
    if os.path.exists(out):
        if not os.path.exists(os.path.join(out, MARKER)):
            raise SystemExit('refusing to overwrite %s: not an a2f probe folder' % out)
        shutil.rmtree(out)
    notes = []

    # equations
    if use_mod_eq:
        data, where = src.need(a2fb_spec.PROBE_EQ_FILE)
        text = a2fb_spec.patch_equation_file(data.decode('latin-1'))
        write(os.path.join(out, 'Data', 'XML', 'AI', 'PerceptualEquations', 'AI_Equations_ExpansionGeneric_Galactic.xml'),
              text.encode('latin-1'))
        notes.append('equations from ' + where)

    # story plots (one per human faction; the shipped ones have an empty Lua_Script)
    for f in PLOTS:
        plot = ('<?xml version="1.0" ?>\r\n\r\n<Story_Mode_Plots>\r\n\t<Lua_Script>A2fb_Story</Lua_Script>\r\n'
                '\t<Active_Plot>Story_Sandbox_12U_%s.XML</Active_Plot>\r\n</Story_Mode_Plots>\r\n' % f)
        write(os.path.join(out, 'Data', 'XML', 'Story_Plots_Sandbox_12U_%s.xml' % f), plot.encode('latin-1'))
    write(os.path.join(out, 'Data', 'Scripts', 'Story', 'A2fb_Story.lua'), crlf(story_lua(use_mod_eq, saboteur)))

    # a2j t02 (D4): one perception plot script per (AI faction, planet) pair, in that faction's own AI story plot
    for fkey, _pn, _planet, suf, plot_file in (a2fb_spec.PERC_PAIRS if percept else []):
        data, where = src.need('DATA\\XML\\' + plot_file.upper())
        write(os.path.join(out, 'Data', 'XML', plot_file),
              ai_plot(data.decode('latin-1'), 'A2fb_Percept' + suf).encode('latin-1'))
        write(os.path.join(out, 'Data', 'Scripts', 'Story', 'A2fb_Percept%s.lua' % suf), crlf(percept_lua(fkey, use_mod_eq)))
    notes.append('%d perception plot scripts' % len(a2fb_spec.PERC_PAIRS) if percept else
                 'no perception channel (%s)' % ('--saboteur' if saboteur else '--no-percept'))

    # a2k t05: the saboteur run's channel U, in the Underworld's own AI story plot (channel D's slot)
    if saboteur:
        data, where = src.need('DATA\\XML\\' + SAB_PLOT.upper())
        write(os.path.join(out, 'Data', 'XML', SAB_PLOT), ai_plot(data.decode('latin-1'), SAB_SCRIPT).encode('latin-1'))
        write(os.path.join(out, 'Data', 'Scripts', 'Story', '%s.lua' % SAB_SCRIPT), crlf(sab_lua(use_mod_eq, sab_desire)))
        notes.append('saboteur run: channel U%s' % ('' if sab_desire else ' without desire reads (--no-sab-desire)'))

    # a2j t02 (D3): GameScoring production events (channel Q)
    data, where = src.need(SCORING)
    write(os.path.join(out, 'Data', 'Scripts', 'Miscellaneous', 'GameScoring.lua'),
          crlf(assemble(data.decode('latin-1'), 'Q', 'a2fb_scoring_hook.lua', use_mod_eq)))
    notes.append('GameScoring from ' + where)

    # free store
    data, where = src.need(FREESTORE)
    write(os.path.join(out, 'Data', 'Scripts', 'FreeStore', 'GalacticFreeStore.lua'),
          crlf(assemble(data.decode('latin-1'), 'F', 'a2fb_freestore_hook.lua', use_mod_eq,
                        head_extra=sab_head(sab_desire) if saboteur else '')))
    notes.append('free store from ' + where)

    # plans
    for name, thread in PLANS:
        data, where = src.need('DATA\\SCRIPTS\\AI\\%s.LUA' % name.upper())
        orig = data.decode('latin-1')
        if not re.search(r'function\s+%s\s*\(' % re.escape(thread), orig):
            raise SystemExit('%s: thread function %s not found: the hook cannot wrap it' % (name, thread))
        write(os.path.join(out, 'Data', 'Scripts', 'AI', '%s.lua' % name), crlf(plan_lua(orig, name, thread, use_mod_eq)))
    notes.append('%d plan scripts' % len(PLANS))

    # probe type data
    texts = []
    for n in SPECIAL_STRUCTURES:
        d, _ = src.read(n)
        if d is not None:
            texts.append(d.decode('latin-1'))
    types = a2fb_spec.parse_type_data(texts, a2fb_spec.probe_type_names())
    write(os.path.join(out, 'A2FB_TYPES.json'), json.dumps(types, indent=1).encode('ascii'))
    qt = queue_types(src)
    write(os.path.join(out, 'A2FB_QUEUE_TYPES.json'), json.dumps(qt, indent=0, sort_keys=True).encode('ascii'))
    notes.append('%d queue types' % len(qt))
    missing = [t for t in a2fb_spec.probe_type_names() if t not in types]
    if missing:
        notes.append('WARNING: no cost / build time for ' + ', '.join(missing))

    # text (on-screen proof lines); only when the install's text file is readable
    mt, where = src.read(MASTERTEXT)
    if mt is not None:
        from build_probe import dat_read, dat_bytes
        tmp = os.path.join(out, 'mt.tmp')
        write(tmp, mt)
        keys = {k for k, _ in TEXTS}
        pairs = [kv for kv in dat_read(tmp) if kv[0] not in keys]
        os.remove(tmp)
        write(os.path.join(out, 'Data', 'Text', 'MasterTextFile_English.dat'), dat_bytes(pairs + TEXTS))
        notes.append('text from ' + where)
    else:
        notes.append('no MasterTextFile_English.dat: the on-screen lines will be blank')

    for ch, fn in CANARIES.items():
        if not canary_built(ch, percept, saboteur):
            continue
        write(os.path.join(out, fn), b'a2f t07 canary, deleted by _CustomScriptMessage when the %s script runs\r\n' % ch.encode())
    write(os.path.join(out, MARKER), b'a2f t07 budget probe mod; removed by cleanup_a2f.cmd\r\n')
    print('built %s (%s)' % (out, '; '.join(notes)))
    return types


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--out', help='mod folder (default <game>\\corruption\\Mods\\%s)' % MOD_NAME)
    ap.add_argument('--source-zip', default=os.path.join(ROOT, 'extracted', 'foc_data_text.zip'))
    ap.add_argument('--no-mod-equations', action='store_true')
    ap.add_argument('--no-percept', action='store_true', help='leave channel D (the D4 perception scripts) out')
    ap.add_argument('--saboteur', action='store_true', help='a2k t05 saboteur run: channel U instead of D, FSAB records')
    ap.add_argument('--no-sab-desire', action='store_true', help='with --saboteur: no desire evaluations in channel U')
    a = ap.parse_args()
    build(a.game, a.out or os.path.join(a.game, 'corruption', 'Mods', MOD_NAME), a.source_zip,
          not a.no_mod_equations, not a.no_percept, a.saboteur, not a.no_sab_desire)


if __name__ == '__main__':
    main()
