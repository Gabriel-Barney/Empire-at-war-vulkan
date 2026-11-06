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
    return '\n'.join(lines) + '\n'


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
