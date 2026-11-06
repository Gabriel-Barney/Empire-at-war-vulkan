# A2h t03: Empire Infrastructure raw weight 10.7250 (port) vs 10.7351 (retail) at the budget Init (2026-11-06)

Scenario: `Sandbox_Heart_Of_The_Maw_Rebel`, human Rebel, Normal, Empire AI `Basic_Empire_Default` (A2G_T02_T04_PORT_VS_RETAIL.md §1-2).
Retail: the a2f t07 probe runs `20261105-191238`, `20261105-192142`, `20261105-195851` (`tools/probe_mod/a2f_budget/out/`), their
`samples.tsv` pools and the `SM` records in `records.txt` (`NET=` is the mod equation `Variable_Self.NetIncomeUnnormalized`, printed with
6 significant digits). Port: HEAD of `claude/a2h-followups-3` (`d0087c4`), exported and patched with `tools/analysis/a2g_trace_patch.py`
plus a scratch hook that adds extra equations from a file, so single tokens could be evaluated through `ALAMO_A2G_EQS` (method below).
Nothing under `engine/` was changed. Tags as in the other `docs/re` pages.

## t03 port decomposition

### Equation (BUDGETINGEQUATIONS.XML, `BasicEmpireInfrastructureBudgetAllocation`)

```
1.1 * (2 + 0.5 * TechLevel - MaxedStarbases - MaxedGroundbases) * (1.0 + Maintenance)
    * (1.0 + 2.0 * (0.5 > StructureCount{Empire_Ground_Mining_Facility} / PlanetsControlledUnnormalized))
+ 6  * (1 - Function_Can_Build_Offensive_Space.Evaluate)
+ 10 * Function_Need_Tech_Upgrade_Unblocked.Evaluate
+ 10 * ((HasStructure{E_Ground_Light_Vehicle_Factory} == 0) + (HasStructure{E_Ground_Barracks} == 0)
        + (HasStructure{Empire_Ground_Mining_Facility} == 0))
```

### Inputs at the Empire's Init (frame 1, player 1), port values

| Input | Port | Term |
|---|---|---|
| `Variable_Self.TechLevel` | 3 | 2 + 0.5 x 3 = 3.5 |
| `Variable_Self.MaxedStarbases` | 0.00 | |
| `Variable_Self.MaxedGroundbases` | 0.25 (1 of 4 planets) | 3.5 - 0 - 0.25 = **3.25** |
| `Variable_Self.Maintenance` | **0.0** (`galactic_ai_tokens.cpp`: "[port] no upkeep model") | (1 + M) = 1 |
| `StructureCount{Empire_Ground_Mining_Facility}` / `PlanetsControlledUnnormalized` | 1 / 4 = 0.25 < 0.5 | factor **3** |
| `Function_Can_Build_Offensive_Space` | 1 (Kuat has Empire_Star_Base_3) | 6 x 0 = 0 |
| `Function_Need_Tech_Upgrade_Unblocked` | 0 | 0 |
| HasStructure LVF / Barracks / Mining Facility | 1 / 1 / 1 (Kuat + Tatooine LVF, four barracks, Corellia mine) | 10 x 0 = 0 |
| `Variable_Self.GrossIncomeUnnormalized` | 667.5 | (Maintenance denominator) |
| **Total** | 1.1 x 3.25 x 1 x 3 = **10.7250** (the equation and the trace agree) | |

Correction to A2G_T02_T04 §1 cause 2: there is **no "+10 missing structure" term** in this scenario. All 10.725 is the first product
(3.3 x 3.25); the Empire starts with an LVF, barracks and a mining facility. So the 0.0101 gap is a 0.094 % gap in that product, not
1.4 % in one factor.

### Which input gives 10.7351

| Candidate | Value needed for 10.7351 | Plausible? |
|---|---|---|
| TechLevel | 3.0061 | no: an integer (player +0x84) |
| MaxedStarbases + MaxedGroundbases | 0.24694 | no: a fraction of 4 owned planets (multiples of 0.25) |
| mines factor | 3.0028 | no: 1 or 3 (a comparison) |
| the three additive terms | +0.0101 | no: steps of 6 / 10 (booleans; Need_Tech_Upgrade_Unblocked is a product of booleans) |
| **Maintenance** | **9.40e-4** (MaintenanceUnnormalized 0.628 of Gross 667.5) | **yes**: the only continuous input, and the only one that moves without a structure or planet change |

### Retail Maintenance, measured

The probe's `NET` (= `NetIncomeUnnormalized` = Gross - MaintenanceUnnormalized, AI_GALACTIC_TOKENS §3 [M]) gives retail's upkeep directly.
With Gross 667.5 (port and retail agree, A2G_T02_T04 §1 cause 3):

| Frames (all 3 runs) | Retail NET | MaintenanceUnnormalized | Retail Infrastructure weight (from the pools) |
|---|---|---|---|
| 0 .. ~430-460 | 666.872 | 0.628 | 10.73506 (8.33333 x 4000.3190 / 3105.3394) |
| ~430-460 .. ~647-677 | 666.837 | 0.663 (+0.035) | |
| ~647-677 .. 10600 | 666.787 | 0.713 (+0.050) | 10.7364 (A2G_T02_T04 §1 cause 2) |

Underworld (AI, same runs): NET 983.8 at frame 1 against the port's Gross 985.0, i.e. upkeep 1.200.

### Formula that reproduces every retail number [H by fit, 5 independent values; M as code]

```
MaintenanceUnnormalized(Self) = Faction.Maintenance_Cost
                                * sum over the player's SPACE units in fleets of
                                  Type.Maintenance_Cost * Build_Cost_Credits / Build_Time_Seconds / 30
Maintenance                   = MaintenanceUnnormalized / GrossIncomeUnnormalized      (Gross 0 -> 0)
NetIncomeUnnormalized         = GrossIncomeUnnormalized - MaintenanceUnnormalized
```

Base type values (variant inheritance applied: Victory_Destroyer from Victory_Destroyer_No_Fighters, Accuser_Star_Destroyer takes
Maintenance_Cost 0.3 and Build_Time 90 from Generic_Star_Destroyer and keeps its own cost 5000). No difficulty build-time multiplier
(Normal 0.8), no planet modifiers. Faction Maintenance_Cost: Empire 0.25, Underworld 0.25 (FACTIONS.XML / EXPANSION_FACTIONS.XML).

| Check | Units (Maintenance_Cost x cost / time) | Predicted | Retail |
|---|---|---|---|
| Empire Init | 4 Tartan (0.05 x 1250 / 15 = 4.1667 each), 2 Victory (0.1 x 3800 / 30 = 12.667), 2 Acclamator (0.1 x 2500 / 30 = 8.333), Accuser (0.3 x 5000 / 90 = 16.667; Piett, spawned by Story_Empire_AI_Heroes_EXP at tech 3, Coruscant -> Corellia, GALACTIC_RNG_DRAWS row 3): sum 75.333 | 75.333 x 0.25 / 30 = 0.62778, NET 666.8722 | 666.872 |
| Empire step 1 | + one Tartan_Patrol_Cruiser (4.1667) | +0.03472, NET 666.8375 | 666.837 |
| Empire step 2 | + one Broadside_Class_Cruiser (0.05 x 1800 / 15 = 6.0) | +0.05000, NET 666.7875 | 666.787 |
| Underworld Init | 4 Crusader_Gunship (0.05 x 1400 / 8 = 8.75), 3 Interceptor4_Frigate (0.2 x 2400 / 23 = 20.870), Kedalbe_Battleship (0.3 x 6000 / 75 = 24), 2 Vengeance_Frigate (0.1 x 2800 / 25 = 11.2); StarViper squadrons and heroes have no Maintenance_Cost: sum 144.009 | 1.20007, NET 983.7999 | 983.8 |
| Empire weight at Init | 1.1 x 3.25 x (1 + 0.62778 / 667.5) x 3 | 10.73509 | 10.73506 (10.7351) |
| Empire weight after step 2 | 1.1 x 3.25 x (1 + 0.7125 / 667.5) x 3 | 10.73645 | 10.7364 |
| Underworld Infrastructure weight at Init | port 11.75 = (4 - 3 x PlanetsCorrupted 0.3333) x (2.5 - 0 - MaxedGroundbases 0.25) x (1 + M) + 5 (one palace) + 0 (Should_Save_For_Corruption, all structures present), traced; with M = 1.20007 / 985 | 11.75822 | 11.75824 (6.0 x 5297.0310 / 2702.9724) |

Rejected variants (each misses at least one row): ground companies included (Empire NET 666.856, Underworld far below 983.8);
build time x 0.8 Normal multiplier (Empire 666.715, Tartan step 0.0434); Accuser not counted (Empire 667.011).

So the candidate cause is **the port's `Maintenance` = 0**. Retail's drift (+0.0013 over 5.5 min) is the Empire's first two space
builds (a Tartan, then a Broadside) entering the upkeep sum; no structure or planet changed.

### Open points [L]

- "Units in fleets": landed ground companies do not count (both factions' fits need them out). Whether a ground company inside a fleet
  (in transit or in orbit) counts is open; retail's NET 666.786 from about frame 10684 (+0.0017 = one Imperial_Stormtrooper_Squad,
  0.05 x 60 / 15 / 120) hints that it does, but nothing else in the window confirms it.
- The constant 30 (likely frames per second) and the base (not per-player) cost and time are fitted, not read. Types with
  Build_Time_Seconds 0 (hero teams) have no Maintenance_Cost here; a zero time must be skipped, not divided by.
- The port with `--story` evaluates the Init at frame 1 without the Accuser (Empire SpaceTotalUnnormalized 52600 with and without
  `--story`), so an upkeep model alone gives 4 Tartan + 2 Victory + 2 Acclamator = 0.48889, weight 10.73286, unless the Accuser
  exists in the fleet before the Empire's first AI service (retail's frame-0 sample already includes it). Which module spawns it, and
  when, is outside engine/ai.
- Float vs double: the retail values fit in double to the printed digits; float accumulation is not distinguishable here.

### Effect of a fix

Only `Variable_Self.Maintenance` (BasicEmpire / BasicRebel / Underworld Infrastructure allocations, all `(1 + Maintenance)`) and
`NetIncomeUnnormalized` (`Can_Afford_Steal_Tech`, > 500) read these tokens in the shipped data. Every galactic-AI golden moves
(`galactic_ai_hotm_golden.txt`, `galactic_ai_hotm_human_golden.txt`, and the HotM save / galactic-AI CRC tests): the budget split
changes by about 0.1 % at every reweight.

### Method (reproduce)

```
git archive HEAD CMakeLists.txt engine tools/lint tools/test_env.cmd tools/cache_env.cmd tools/meg tools/formats tools/shaders/cpp | tar -x -C <dir>
python tools/analysis/a2g_trace_patch.py <dir>
# scratch hook in <dir>/engine/apps/headless/ai_run.cpp after load_equation_files: read ALAMO_A2H_EXTRA_EQS ("name|expression" per
# line), eqs.add(name, expression), then eqs.link()
ALAMO_A2G_TRACE=1 ALAMO_A2H_EXTRA_EQS=eqs.txt ALAMO_A2G_EQS=A2H_TL,A2H_MS,A2H_MG,A2H_M,...,BasicEmpireInfrastructureBudgetAllocation
  alamo_headless --game <corruption> --economy --galactic-ai --campaign Sandbox_Heart_Of_The_Maw_Rebel --human Rebel --ticks 3000 --workers 0 --quiet --ai-log rb.log 2> rb.err
grep "A2G EQ f=1 p=1 " rb.err
```

with `eqs.txt` lines such as `A2H_M|Variable_Self.Maintenance`, `A2H_MG|Variable_Self.MaxedGroundbases`,
`A2H_MINES|Variable_Self.StructureCount {Parameter_Type = "Empire_Ground_Mining_Facility"}`,
`A2H_CBOS|Function_Can_Build_Offensive_Space.Evaluate`. Retail NET series:
`grep "|SM|" records.txt | grep fac=EMPIRE`, sorted by `f=`.

## Port after the a2h t03 fix (2026-11-06)

`MaintenanceUnnormalized` / `Maintenance` / `NetIncomeUnnormalized` now follow A2H_GALACTIC_AI_RE.md §3 (Self's fleets; Maintenance_Cost
parsed for types and factions). Rebel-entry trace (scratch export of the worktree with the a2g trace patch, `ALAMO_A2G_TRACE=1`):

| Run | Init (frame 1) MaintenanceUnn / weight | from frame 285 (leaf cache 7 s expired) |
|---|---|---|
| `--galactic-ai` (no story) | 0.48889 / 10.7329 (no Accuser: no story hero spawns) | unchanged |
| `--galactic-ai --story` | 0.48889 / 10.7329 | 0.62778 / **10.7351** (Accuser in the Empire fleet) |

The Accuser (Spawn_Piett, UNIQUE_UNIT at the missing Coruscant -> Corellia) is spawned by the story's first service at frame 1
(SimHost T9), after the AIs' first service (T7) in the same frame, so the Empire's budget Init does not see it. Retail's frame-0
sample already includes it (NET 666.872 at frames 0 and 1). The order is a host / story question (engine/apps), not an AI one.
