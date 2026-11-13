#!/usr/bin/env python3
"""a2fb_spec.py - the one table the a2f t07 budget probe is generated from (build script, Lua header, report).

Everything the Lua probe evaluates is listed here, so the mod equations, the Lua sample keys and the report decoder
cannot drift apart:

  * budget-fraction equations A2FB_<FAC>_BFB_<k>_<CAT>: Variable_Self.BudgetFractionToBuild of one of two probe
    structure types (k = 1, 2; different build times) with and without Parameter_Category.  AI_GALACTIC_TOKENS: the
    value is R / D, R = remaining cost of the type, D = build time x category income rate + category available
    credits (all categories: budget +0x68 income per second, player credits).  Two types with different build times
    give D1, D2 and so rate and available credits per category (a2f_report.py solves it).
  * A2FB_CreditsUnnorm = Variable_Self.CreditsUnnormalized (AI player: the budget's total of pools, 0x6136b0 [M]).
  * shipped equations (no mod file needed): the budget-weight equations of each faction's galactic templates and the
    Tech_Upgrade trigger / untrigger.
  * a2h t02 (MJ record, Empire): the MajorItem flush / reclaim trigger equations (shipped) and their inputs (mod
    equations A2FB_HumanSpaceTotal, A2FB_ActiveGoalsMajor, ...), MAJOR_SHIPPED / MAJOR_MOD below.

None of these equations contains the '#' operator (a random draw inside an evaluation, AI_GALACTIC section 8), so
evaluating them draws nothing from the game's synchronised RNG (selftest_a2f.py checks the shipped ones).
"""
import re

PROBE_EQ_FILE = 'DATA\\XML\\AI\\PERCEPTUALEQUATIONS\\AI_EQUATIONS_EXPANSIONGENERIC_GALACTIC.XML'

# category key -> AIGoalCategoryType name (None = no Parameter_Category: all categories). Enum values:
# Data\XML\Enum\AIGoalCategoryType.xml (Offensive 0, Defensive 1, Information 2, Infrastructure 3, MajorItem 11).
CATS = [('ALL', None), ('OFF', 'Offensive'), ('DEF', 'Defensive'), ('INFO', 'Information'),
        ('INFRA', 'Infrastructure'), ('MAJOR', 'MajorItem')]

# faction key (Lua: first letters of Get_Faction_Name upper case) -> (Find_Player name, probe type 1, probe type 2).
# Costs / build times come from the install at build time (types.json); these are shipped SpecialStructure names.
FACTIONS = [
    ('EMPIRE', 'EMP', 'Empire', 'E_Ground_Barracks', 'E_Ground_Research_Facility'),
    ('REBEL', 'REB', 'Rebel', 'R_Ground_Barracks', 'R_Ground_Research_Facility'),
    ('UNDERWORLD', 'UWD', 'Underworld', 'U_Ground_Barracks', 'U_Ground_Vehicle_Factory'),
]

# Shipped equations evaluated per faction (name, short key). Weights: the template budget equations that feed
# AIBudgetClass init / reweight; triggers: the Tech_Upgrade template (BasicEmpireTemplates.xml, priority 4).
SHIPPED = {
    'EMPIRE': [
        ('BasicEmpireOffensiveBudgetAllocation', 'wOFF'), ('BasicEmpireDefensiveBudgetAllocation', 'wDEF'),
        ('BasicEmpireInformationBudgetAllocation', 'wINFO'), ('BasicEmpireInfrastructureBudgetAllocation', 'wINFRA'),
        ('BasicEmpireMajorItemBudgetAllocation', 'wMAJOR'),
        ('EmpireTechUpgradeOffensiveAllocation', 'tOFF'), ('EmpireTechUpgradeDefensiveAllocation', 'tDEF'),
        ('EmpireTechUpgradeInformationAllocation', 'tINFO'), ('EmpireTechUpgradeInfrastructureAllocation', 'tINFRA'),
        ('EmpireTechUpgradeMajorItemAllocation', 'tMAJOR'),
        ('TechUpgradeBudgetRequirement', 'tReq'), ('Empire_Tech_Trigger', 'TRIG'), ('Empire_Tech_Untrigger', 'UNTRIG'),
    ],
    'REBEL': [
        ('BasicRebelOffensiveBudgetAllocation', 'wOFF'), ('BasicRebelDefensiveBudgetAllocation', 'wDEF'),
        ('BasicRebelInformationBudgetAllocation', 'wINFO'), ('BasicRebelInfrastructureBudgetAllocation', 'wINFRA'),
    ],
    'UNDERWORLD': [
        ('Underworld_Offensive_Budget_Allocation', 'wOFF'), ('Underworld_Infrastructure_Budget_Allocation', 'wINFRA'),
    ],
}
# Needs a Human player (Variable_Human); the Lua evaluates it only when a human player exists.
SHIPPED_NEEDS_HUMAN = ['Underworld_Offensive_Budget_Allocation', 'Underworld_Infrastructure_Budget_Allocation']

COMMON_EQS = [('A2FB_CreditsUnnorm', 'CU'), ('A2FB_NetIncome', 'NET')]

# a2h t02 (docs/re/A2G_T02_T04_PORT_VS_RETAIL.md section 4): the MajorItem flush / reclaim triggers and their inputs, one
# MJ record per sample (small and full) of these factions. Shipped equations: (name, key, needs a human player).
MAJOR_FACTIONS = ['EMPIRE']
MAJOR_SHIPPED = [
    ('Needs_More_Space_Force_Globally', 'NMS', True),        # Variable_Human.SpaceTotalUnnormalized > Self x 1.0
    ('Need_To_Flush_MajorItem_Budget', 'NTF', True),         # NMS + HasUnit(Death_Star): goal Flush_MajorItem_Budget
    ('Can_Reclaim_Excess_MajorItem_Budget', 'CRM', False),   # ActiveGoals{MajorItem} > 0: goal Reclaim_Excess_...
    ('Has_Enough_Space_Force_Globally', 'HES', False),       # 0.9 x FRT > ERT
    ('Friendly_Global_Space_Unit_Raw_Total', 'FRT', False),
    ('Enemy_Global_Space_Unit_Raw_Total', 'ERT', False),
]
# Mod equations (name, key, body, needs a human player): added to the equation file with the A2FB_* ones above.
# a2i t02: A2FB_ActiveGoals (key AG, Variable_Self.ActiveGoals with no Parameter_Category) is gone. Retail cannot evaluate
# it (ActiveGoals needs exactly one Parameter_Category, A2H_GALACTIC_AI_RE.md section 1), and the failed evaluation sets
# the calling script's failed flag, which pcall does not see: in run 20261106-040725 it stopped the story script and the
# Empire's free store after their first sample, and every Empire plan thread at its next sample. Every equation left
# here answered in that run. selftest_a2f.py (retail_eval_fails) rejects such a body.
MAJOR_MOD = [
    ('A2FB_HumanSpaceTotal', 'HST', 'Variable_Human.SpaceTotalUnnormalized', True),
    ('A2FB_SelfSpaceTotal', 'SST', 'Variable_Self.SpaceTotalUnnormalized', False),
    ('A2FB_ActiveGoalsMajor', 'AGM', 'Variable_Self.ActiveGoals {Parameter_Category = AIGoalCategoryType[MajorItem]}',
     False),
    ('A2FB_HasDeathStar', 'HDS', 'Variable_Self.HasUnit{Parameter_Type = "Death_Star", Parameter_Count = 1}', False),
    ('A2FB_Maintenance', 'MNT', 'Variable_Self.Maintenance', False),
    ('A2FB_MaintenanceUnnorm', 'MNTU', 'Variable_Self.MaintenanceUnnormalized', False),
    ('A2FB_PlanetsControlled', 'PLC', 'Variable_Self.PlanetsControlled', False),
    ('A2FB_PlanetsControlledUnnorm', 'PLCU', 'Variable_Self.PlanetsControlledUnnormalized', False),
]
MAJOR_KEYS = [k for _n, k, _h in MAJOR_SHIPPED] + [k for _n, k, _b, _h in MAJOR_MOD]

# a2j t02 (D4, docs/re/A2I_T03_TRAJECTORY.md section 5, docs/re/A2J_T02_PROBE_PLAN.md): the Conquer_Pirate desire of one
# (AI faction, target planet) pair and its terms, from a plot script of that faction's own AI story plot (channel D, one
# script per pair, so an evaluation that fails stops only that pair's channel). Pairs: (faction key, Find_Player name,
# FindPlanet name, script suffix, AI plot file whose Lua_Script the build sets).
PERC_PAIRS = [
    ('UNDERWORLD', 'Underworld', 'Kashyyyk', 'U', 'Story_Plots_Underworld_AI_Heroes.xml'),
    ('EMPIRE', 'Empire', 'Felucia', 'E', 'Story_Plots_Empire_AI_Heroes_EXP.xml'),
]
# Shipped equations, evaluated with the planet as the target (name, key, needs a human player). Why each is safe in
# retail: the goal system evaluates Should_Conquer_Pirate_Planet (Conquer_Pirate's desire) on these very targets every
# proposal pass, and the others are its sub-functions, so every token in them is evaluated in retail with a planet
# target already; GenericPlanetValue and Is_Neglected_By_My_Opponent_Space are also called from shipped Lua with a
# planet target (PathBonusForDistantTargets.lua). Is_AI_And_Should_Be_Ignored (and so Is_Good_Invasion_Target and
# Should_Conquer_Pirate_Planet) reads Needs_More_Space_Force_Globally -> Variable_Human, which fails with no human: the
# kit skips them then (as MJ does). No '#' in any galactic equation file (selftest).
PERC_SHIPPED = [
    ('Should_Conquer_Pirate_Planet', 'SCP', True),        # Is_Good_Invasion_Target x (IsHumanControlled == 0)
    ('Is_Good_Invasion_Target', 'IGI', True),
    ('Opens_New_Front', 'ONF', False),
    ('Is_AI_And_Should_Be_Ignored', 'IGN', True),
    ('GenericPlanetValue', 'GPV', False),
    ('Is_Neglected_By_My_Opponent_Space', 'NEG', False),
    ('System_Recon_Relevance', 'SRR', False),
    ('Is_Connected_To_Me', 'ICM', False),
    ('Is_Connected_To_Enemy', 'ICE', False),
]
# Mod equations (name, key, body): each body is a token of Is_Good_Invasion_Target written exactly as it stands there
# (same root, same parameters, none), so it evaluates wherever that shipped equation does.
PERC_MOD = [
    ('A2FB_D4_Markup', 'MK', 'Variable_Target.Markup'),
    ('A2FB_D4_ConnIso', 'CIP', 'Variable_Target.ConnectsIsolatedPlanetsByForce'),
    ('A2FB_D4_TradeRoutes', 'TR', 'Variable_Target.TradeRoutes'),
    ('A2FB_D4_Chokepoint', 'CHK', 'Variable_Target.Hints.Chokepoint'),
    ('A2FB_D4_PriorityTarget', 'PRI', 'Variable_Target.Hints.PriorityTarget'),
    ('A2FB_D4_HasGroundForce', 'HGF', 'Variable_Target.EnemyForce.HasGroundForce'),
    ('A2FB_D4_HasSpaceForce', 'HSF', 'Variable_Target.EnemyForce.HasSpaceForce'),
    ('A2FB_D4_IsHuman', 'HUM', 'Variable_Target.IsHumanControlled'),
    ('A2FB_D4_ConnCorruption', 'ICC', 'Variable_Target.IsConnectedToCorruption'),
    ('A2FB_D4_Corrupted', 'ICOR', 'Variable_Target.IsCorrupted'),
    ('A2FB_D4_AnyThreats', 'ACT', 'Variable_Self.AnyCurrentThreats'),
]
PERC_KEYS = [k for _n, k, _h in PERC_SHIPPED] + [k for _n, k, _b in PERC_MOD]
PERC_SHIPPED_NAMES = [n for n, _k, _h in PERC_SHIPPED]
# Sample frames: every 30 frames in 0..600, then every 300 frames to the end of the run (A2I_T03 section 5 asks 0..600).
PERC_DENSE_END, PERC_DENSE_STEP, PERC_STEP = 600, 30, 300

# Template that is active when: base template always (Basic_*_Default), Tech_Upgrade when TRIG > 0 (Empire only).
# Raw weights of the candidates, by the shipped equation keys above (a2f_report.py compares normalised pools).
TEMPLATE_CANDIDATES = {
    'EMPIRE': {
        'base': {'OFF': 'wOFF', 'DEF': None, 'INFO': 'wINFO', 'INFRA': 'wINFRA', 'MAJOR': 'wMAJOR'},
        'tech': {'OFF': 'tOFF', 'DEF': None, 'INFO': 'tINFO', 'INFRA': 'tINFRA', 'MAJOR': 'tMAJOR'},
    },
    'REBEL': {'base': {'OFF': 'wOFF', 'DEF': None, 'INFO': 'wINFO', 'INFRA': 'wINFRA'}},
    'UNDERWORLD': {'base': {'OFF': 'wOFF', 'INFRA': 'wINFRA'}},
}
# Defensive in the base templates is the equation Zero (weight 0); in Basic_Empire_Default the Defensive entry is
# Zero although BasicEmpireDefensiveBudgetAllocation exists (it is only used by BasicDefensive templates).  The
# table above therefore gives DEF no weight (None = 0).  The Rebel / Underworld default templates likewise.


def eq_name(fac3, k, cat):
    return 'A2FB_%s_BFB_%d_%s' % (fac3, k, cat)


def bfb_equations():
    """[(equation name, faction key, k, cat key)] for the three factions."""
    out = []
    for fkey, fac3, _, t1, t2 in FACTIONS:
        for k in (1, 2):
            for ckey, _cname in CATS:
                out.append((eq_name(fac3, k, ckey), fkey, k, ckey))
    return out


def equations_xml():
    """The probe equations as the text appended to the shipped AI_Equations_ExpansionGeneric_Galactic.xml."""
    lines = ['\t<!-- a2f t07 budget probe (tools/probe_mod/a2f_budget): read-only equations evaluated from the probe Lua.',
             '\t     Nothing in the game references them. No # operator (no RNG draw). -->',
             '\t<A2FB_CreditsUnnorm>', '\t\tVariable_Self.CreditsUnnormalized', '\t</A2FB_CreditsUnnorm>',
             '\t<A2FB_NetIncome>', '\t\tVariable_Self.NetIncomeUnnormalized', '\t</A2FB_NetIncome>']
    cat_name = dict(CATS)
    for fkey, fac3, _, t1, t2 in FACTIONS:
        for k, t in ((1, t1), (2, t2)):
            for ckey, _cn in CATS:
                n = eq_name(fac3, k, ckey)
                if cat_name[ckey] is None:
                    body = 'Variable_Self.BudgetFractionToBuild{Parameter_Type = "%s"}' % t
                else:
                    body = ('Variable_Self.BudgetFractionToBuild{Parameter_Type = "%s", '
                            'Parameter_Category = AIGoalCategoryType[%s]}' % (t, cat_name[ckey]))
                lines += ['\t<%s>' % n, '\t\t' + body, '\t</%s>' % n]
    lines.append('\t<!-- a2h t02: MajorItem flush / reclaim inputs (A2G_T02_T04_PORT_VS_RETAIL.md section 4) -->')
    for n, _k, body, _h in MAJOR_MOD:
        lines += ['\t<%s>' % n, '\t\t' + body, '\t</%s>' % n]
    lines.append('\t<!-- a2j t02: D4 perception tokens of Is_Good_Invasion_Target (A2J_T02_PROBE_PLAN.md) -->')
    for n, _k, body in PERC_MOD:
        lines += ['\t<%s>' % n, '\t\t' + body, '\t</%s>' % n]
    return '\n'.join(lines) + '\n'


def probe_equation_count():
    """Number of A2FB_* equations equations_xml() adds."""
    return len(COMMON_EQS) + len(bfb_equations()) + len(MAJOR_MOD) + len(PERC_MOD)


# bkg3 t03 / t04 (run_a2f_empire.cmd, a2f_run.py --human-empire): with a human Empire the Underworld pair samples
# Tatooine (a human planet the port's Underworld attacks with Conquer_Opponent from frame 6895, retail never before
# 39000) and adds the three shipped equations of that goal's desire. Safe in retail for the reason given above: the
# Underworld's goal system evaluates Should_Conquer_Opponent_Planet on every enemy planet each proposal pass
# (BasicOffensiveGalacticSet), and the other two are its sub-functions.
HUMAN_EMPIRE_PLANET = 'Tatooine'
HUMAN_EMPIRE_SHIPPED = [
    ('Should_Conquer_Opponent_Planet', 'SCO', True),
    ('First_Attack_Allowed', 'FAA', False),
    ('Should_Perform_Unrestricted_Grab_Space', 'UGS', True),
]


def set_human_empire():
    """Switch this module's channel-D lists to the human-Empire run (idempotent). Call before building the mod."""
    global PERC_KEYS, PERC_SHIPPED_NAMES
    for i, p in enumerate(PERC_PAIRS):
        if p[0] == 'UNDERWORLD':
            PERC_PAIRS[i] = (p[0], p[1], HUMAN_EMPIRE_PLANET, p[3], p[4])
    for e in HUMAN_EMPIRE_SHIPPED:
        if e not in PERC_SHIPPED:
            PERC_SHIPPED.append(e)
    PERC_KEYS = [k for _n, k, _h in PERC_SHIPPED] + [k for _n, k, _b in PERC_MOD]
    PERC_SHIPPED_NAMES = [n for n, _k, _h in PERC_SHIPPED]


def perc_lua(fkey, use_mod_eq=True):
    """Lua text of the D channel's pair table for one faction key (read by a2fb_percept.lua)."""
    pair = [p for p in PERC_PAIRS if p[0] == fkey][0]
    ship = ', '.join('{ "%s", "%s", %s }' % (k, n, 'true' if h else 'false') for n, k, h in PERC_SHIPPED)
    mod = ', '.join('{ "%s", "%s", false }' % (k, n) for n, k, _b in PERC_MOD) if use_mod_eq else ''
    return ('A2FB_PERC = { fac = "%s", player = "%s", planet = "%s", shipped = { %s }, tokens = { %s } }\n'
            'A2FB_PERC_DENSE_END = %d\nA2FB_PERC_DENSE_STEP = %d\nA2FB_PERC_STEP = %d\n'
            % (pair[0], pair[1], pair[2], ship, mod, PERC_DENSE_END, PERC_DENSE_STEP, PERC_STEP))


def patch_equation_file(shipped_text):
    """Shipped file text (str) -> text with the probe equations inserted before </Equations>."""
    i = shipped_text.rindex('</Equations>')
    return shipped_text[:i] + equations_xml().replace('\n', '\r\n' if '\r\n' in shipped_text else '\n') + \
        shipped_text[i:]


def lua_header(use_mod_eq=True):
    """Lua text defining the evaluation tables A2FB_SPEC (read by a2fb_core.lua)."""
    L = ['-- generated by build_a2f_mod.py from a2fb_spec.py',
         'A2FB_MODEQ = %s' % ('true' if use_mod_eq else 'false'),
         'A2FB_SPEC = {}']
    for fkey, fac3, pname, t1, t2 in FACTIONS:
        L.append('A2FB_SPEC.%s = {' % fkey)
        L.append('\tplayer = "%s", types = { "%s", "%s" },' % (pname, t1, t2))
        small = []
        for k in (1, 2):
            for ckey in ('ALL', 'INFRA', 'OFF'):
                small.append('{ "B%d%s", "%s" }' % (k, ckey, eq_name(fac3, k, ckey)))
        L.append('\tsmall = { %s },' % ', '.join(small))
        full = []
        for k in (1, 2):
            for ckey, _cn in CATS:
                if ckey in ('ALL', 'INFRA', 'OFF'):
                    continue
                full.append('{ "B%d%s", "%s" }' % (k, ckey, eq_name(fac3, k, ckey)))
        L.append('\tfull = { %s },' % ', '.join(full))
        ship = ', '.join('{ "%s", "%s", %s }' % (key, n, 'true' if n in SHIPPED_NEEDS_HUMAN else 'false')
                         for n, key in SHIPPED[fkey])
        L.append('\tshipped = { %s },' % ship)
        if fkey in MAJOR_FACTIONS:
            # { key, equation, needs a human, mod equation }
            major = ['{ "%s", "%s", %s, false }' % (k, n, 'true' if h else 'false') for n, k, h in MAJOR_SHIPPED]
            if use_mod_eq:
                major += ['{ "%s", "%s", %s, true }' % (k, n, 'true' if h else 'false') for n, k, _b, h in MAJOR_MOD]
            L.append('\tmajor = { %s },' % ', '.join(major))
        L.append('}')
    L.append('A2FB_COMMON = { %s }' % ', '.join('{ "%s", "%s" }' % (k, n) for n, k in COMMON_EQS))
    return '\n'.join(L) + '\n'


def parse_type_data(xml_texts, names):
    """{type name: {'cost': float, 'time': float}} from SpecialStructure XML texts (Build_Cost_Credits,
    Build_Time_Seconds)."""
    out = {}
    for n in names:
        for t in xml_texts:
            m = re.search(r'<SpecialStructure\s+Name="%s"\s*>(.*?)</SpecialStructure>' % re.escape(n), t,
                          re.S | re.I)
            if not m:
                continue
            c = re.search(r'<Build_Cost_Credits>\s*([0-9.]+)', m.group(1))
            b = re.search(r'<Build_Time_Seconds>\s*([0-9.]+)', m.group(1))
            if c and b:
                out[n] = {'cost': float(c.group(1)), 'time': float(b.group(1))}
                break
    return out


def probe_type_names():
    return [t for _f, _f3, _p, t1, t2 in FACTIONS for t in (t1, t2)]
