# skai t06: retail land skirmish runs (Utapau), read for skai t02 and SKIRMISH_SETUP §10

Runs of 2026-11-08 (Gabriel, by hand: Skirmish, Land, "(6) Utapau Heights", you = Empire idle, slot 2 Rebel Medium AI,
Advanced... Default). Read with `tools/probe_mod/q2_traces/analysis/land_attack.py` plus throw-away queries over the
`O` / `X` / `C` lines; no binary read. Frames are mode frames (30 per second).

| Run (`tools/probe_mod/q2_traces/out/…`) | Ticks | Seed | Notes |
|---|---|---|---|
| `20261108-220755-skirmish-vanilla-lua` | 36000 | 0x7860fa8a | Lua probe: 1152 samples, 100 kills (the probe's cap), 75 built; validate 0 |
| `20261108-223403-skirmish-vanilla` | 9000 | 0x09d7255f | no Lua; same start state (identical object ids at frame 0) |

Map check: the Lua `TMODE` record names `.\Data\Art\Maps\_MP_LAND_UTAPAU.TED` and both runs changed the profile's
`LastInternetMapLand` to `_MP_LAND_UTAPAU.TED`, so the menu's "(6) Utapau Heights" is `_MP_LAND_UTAPAU`. Players: p0 Empire
(is_human 1, no AI), p1 Rebel (AI), p2 Pirates, p3 the map's Neutral owner (pads, markers, props).

## 1. skai t02: does the Rebel AI take the idle human's base?

**Army size (36000 run).** Companies on the field (Population_Value 1 each; `Droids_Team` has no Population_Value):
2 Rebel_Infantry_Squad at frame 0; droids 1054; Kyle Katarn 3361 (dies 10031); a light tank brigade + an infantry squad
4135-4375; a light tank brigade 4930-5170; 9 companies at 12277; Han Solo 13345; an artillery brigade + 2 infiltrator teams
14227-14407; a speeder wing 15112-15232: **13 Population_Value companies + droids from 15232 to the end**. Every company
arrives by a landing (`GALLOFREE_TRANSPORT_LANDING`, `MOLDY_CROW_LANDING`, `MILLENNIUM_FALCON_LANDING`). After 15232 nothing
lands for 20768 frames although 26 more companies are bought (8 heavy tank brigades, 5 infiltrator teams, 5 speeder wings,
2 artillery brigades, a tank buster squad, Garm, Yoda, Luke, Obi-Wan, a second Katarn); the AI's credits pile up to 20859.
So **9 is not the field cap**: retail fields 13 (14 with the droids), then stops dropping. What it counts is open (§4 [RE]).

**Arrival.** First AI unit within 1500 of the human base centre (5121, 1261): frame 5964 (a T2B; 9000 run: 6928, the droids
team). Three T2Bs die at the base shield 6319-6496. AI objects within 1500: 8 at 9000, 19 at 12600, 39 at 18000, 54-64
from 25200 to the end.

**Base losses (all to p1).** E_Ground_Turbolaser_Tower 13107 and 15233, Communications_Array_E 16285, the four start
stormtrooper teams 16451-17132, E_Ground_Heavy_Vehicle_Factory 18175, E_Ground_Base_Shield 19131, E_Ground_Barracks 19347,
E_Ground_Research_Facility 21349, E_Ground_Light_Vehicle_Factory 23120, Power_Generator_E_Multiplayer 24533. Each destroyed
factory / barracks / research facility spawns one STORMTROOPER_TEAM with its squads at its position in the same frame
(`Destruction_Survivors`, Empire_Survivors_*); these die too.

**Result: no.** The Imperial_Command_Center (2800 HP; `E_Command_Center_Self_Repair` Force_Healing,
`E_Magnetically_Sealed_Structure` Redirect_Blaster on Take_Damage) never drops below 2668 HP and heals back to 2759 at
36000. With the CC, the reinforcement point and the empty pads left, the battle does not end in 36000 frames (20 min); from
24533 the AI army stands around the base without progress.

## 2. SKIRMISH_SETUP §10 land [M] items

- **Pre-built pads [H, trace].** With Advanced Default each slot starts with 9 built structures on their pads: Command
  Center, Barracks, Light and Heavy Vehicle Factory, Research Facility, Base Shield, two Turbolaser Towers, Communications
  Array, Power Generator (`*_MULTIPLAYER`). The five `<FACTION>_BUILD_PAD` of each slot start empty; every Neutral pad
  (4 BUNKER_PAD, 4 SKIRMISH_BUILD_PAD, 5 SKIRMISH_MINERAL_PROCESSOR_PAD, SKIRMISH_OUTPOST_PAD, REINFORCEMENT_POINT) starts
  empty and owned by p3.
- **Land_Skirmish_AI_Default_Forces [H, trace].** The idle human gets them too. Empire: 2 Imperial_Stormtrooper_Squad =
  4 STORMTROOPER_TEAM / 36 squads; Rebel: 2 Rebel_Infantry_Squad = 6 REBEL_TROOPER_TEAM / 30 squads. Both companies of a
  slot stand at the same TEAM_tt_SPAWN_POINT_MARKER: the last of its three in id order (Empire 93302 at (5115, 1255), Rebel
  93300 at (1247, 4728)) [M: why the last, and why not one marker per company as §2.2 reads]. The map's
  DEFENDING_FORCES_POSITION (1671, 4620, at the Rebel base) is not used.
- **Credits [H, trace].** The AI pays the full Tactical_Build_Cost_Multiplayer in one step:
  - production at enqueue, several orders in one frame: 382 Droids_Team -850 (built 745, 12.1 s: Medium land build-time
    factor 1.0); 2295 -4520 = Katarn 1300 + 2 light tank brigades 2 x 1100 + infantry squad 320 + RL_Combat_Armor_L1 700,
    while the second brigade only starts building at 3735 (done 4635);
  - pad builds when the UC object appears: Mineral Processor -800 (1909, 5854, 6277, 6700), anti-infantry turret -700,
    Ground Mining Facility -1300 (4641), AV turret + Mineral Processor -1650 (9247).
  §3 reads 0x289050 as "an AI player always counts as PAYG, no charge at enqueue"; the trace shows the opposite in land
  skirmish (§4 [RE]).
- **Neutral pad capture [H, trace].** A Neutral pad changes owner 150 frames (149-151 on six events) after the first AI unit
  comes within its radius: about 50-100 for mineral / build pads, 150-200 for bunker pads. First SKIRMISH_MINERAL_PROCESSOR_PAD
  81773 at 1474 (9000 run 1468); the UC Mineral Processor follows at 1909 (both runs). Others: outpost 3342 / 3347,
  REINFORCEMENT_POINT 4366 / 3821, mineral pads 5248, 5965, 6065, 8736. A pad with a structure keeps its owner; an empty owned
  pad (bunker, build pad) goes back to Neutral 150 frames after the owner's last unit leaves its radius (four bunker events,
  148-151 frames), with no enemy near.

## 3. Port run and differences

`build-wt-msvc\bin\alamo_headless.exe --skirmish-ai foc-land --map _MP_LAND_UTAPAU --human Empire --seed 2019621514
--ticks 36000 --workers 4 --trace-out port.q2t --ai-log ai.txt` (exe of 2026-11-08 19:12, worktree at d3237074). The battle
ends at frame 14733, **winner Rebel** (victory 0x13, reported 0x0e): Barracks dies 12532, the CC 14523.

| # | Item | Retail | Port |
|---|---|---|---|
| 1 | Human start forces | TEAM_00 spawn marker at its own base | at DEFENDING_FORCES_POSITION (1671, 4620) inside the Rebel base: `tactical_battle_bt.cpp:1082` overwrites the skirmish anchor set by `bt_skirmish_anchors()` at :1079. They fight from frame ~200; all four teams die by 11374 |
| 2 | Spawn marker | last TEAM_tt marker (93300 / 93302), both companies on it | first in map order (`tactical_battle_bt.cpp:1369`), marker 763 (1335, 4819), a formation |
| 3 | Pre-built base | 9 structures per slot | CC + Barracks only, "4 pre-built of 30 pads" (`bt_skirmish_base`, `tactical_battle_bt.cpp:1286`); the AI rebuilds the rest |
| 4 | Imperial CC | min 2668 / 2800, heals; no end in 36000 | 2666 at 11400 to 0 at 14523, never heals; Rebel wins at 14733 |
| 5 | AI pad builds | charged at UC creation | free: the charge sits under `if (!ai)` (`tactical_battle_bt.cpp:302`) |
| 6 | Hero limit | one Han_Solo_Team_Land_MP | a second Han lands at 13895 while the first lives: only Build_Limit_Current_For_All_Allies is checked (`tactical_battle_hangar.cpp:531`), not Build_Limit_Current_Per_Player = 1 |
| 7 | Destruction_Survivors | the company's members (team + squads) | the DUMMY_GROUND_COMPANY `EMPIRE_SURVIVORS_LARGE` object itself, no members (`tactical_battle_oi_abilities.cpp:616`) |
| 8 | Field cap | 13 companies + droids | min(capacity, 9) (`battle_ai.cpp:1283`, `tactical_battle_bt.cpp:1279`) |
| 9 | Pad capture | first mineral pad 1474, outpost 3342, 5 mineral pads by 8736 | 2113, outpost 14665, 3 mineral pads by 14621 |
| - | Same as retail | production charged at enqueue (154: Droids_Team -850); first AI unit at the human base 6337 (retail 5964 / 6928); landings by transport; empty bunker pads go back to Neutral | |

`land_attack.py` got `--centre X,Y`: its default centre (the human's frame-0 objects) moves into the Rebel base on a port
run because of item 1.

## 4. Follow-up tasks

- [port] Land skirmish start forces at the slot's TEAM_tt / Player spawn marker; the Defending Forces marker must not
  replace the skirmish anchor (`tactical_battle_bt.cpp:1079-1082`); both companies on the last TEAM_tt marker [M].
- [port] Pre-build all 9 faction base structures per slot (Advanced Default), not only CC + Barracks (`bt_skirmish_base`).
- [port] Charge the AI for pad builds (Tactical_Build_Cost_Multiplayer when the UC object appears).
- [port] Build_Limit_Current_Per_Player in the tactical production check.
- [port] Destruction_Survivors: spawn the survivor company's Company_Units, not the company object.
- [port] Imperial Command Center self repair / magnetically sealed (Redirect_Blaster) in land skirmish; after items 1-6 the
  idle-human Utapau run must not end within 36000 frames. Regenerate the land skirmish goldens.
- [RE] Land skirmish unit cap: what 0x2ac5c0 counts (N with an idle human, start forces, heroes, capacity +0x404), given
  13 companies + droids on the field with curve 9, and what stops the pool drops after frame 15232.
- [RE] SKIRMISH_SETUP §3: correct the PAYG reading of 0x289050 (the AI pays at enqueue, in one step).
- [RE] L1_S4 §4.2: an empty owned pad returns to Neutral 150 frames after the owner's last unit leaves; check the port's timing.

## 5. Port result after the follow-ups (skai t07-t11, 2026-11-12)

Items 1-5 of §4 are in the port (`SKIRMISH_SETUP.md` §10, "skai t07-t11"); item 6 needed no change (the Command Center self
repair, 2 % every 2.5 s, and the sealed structure, off until its upgrade, were already as retail). Port Utapau run with an
idle human Empire: the first base structure falls at 13475 (retail 13107), but the Command Center then dies at 19321 and the
battle ends at 22920; retail never loses the Command Center in 36000 frames. The port's Rebel army damages the CC at about
2.6 HP per frame (retail about 0.7, under the 56 HP / 75 frames repair) through BurnUnitsLand (`Find_Nearest(MainForce,
"InBase")`, then Attack_Move loops on the nearest structure). The retail Lua probe has no land plan records, so where retail's
attack stops (TACTICAL_COMBAT 2.8 [L], backlog b0004) needs a retail land-plan probe; open in skai t14. AI-vs-AI foc-land at
30000 ticks: seeds 1-6 all end (21439 / 29833 / 18993 / 21933 / 24310 / 28372).

## 6. Retail land-plan probe (skai t14, 2026-11-12)

Run `20261112-095520-skirmish-vanilla-lua` (`run_skirmish_land_attack.cmd`, Claude driving with Gabriel's go-ahead; same
lobby as §1: Utapau Heights, idle human Empire, Rebel Medium AI): 36000 frames, the trace's `ticks_reached` end. The kit's
plan hooks covered the four land attack plans (`make_plan_hooks.py`: BurnUnitsLand, DestroyStructure, DestroyTurbolaser,
DestroyLandUnit, with `tgt=` / `near=` fields); 91 plan scripts loaded, 0 probe errors. Read with throw-away scripts over
`trace.q2t` and `lua_records.txt`; no binary read.

**Human base losses.** Turbolaser towers 10987 and 16475, Heavy Vehicle Factory 19782, Communications Array 20220, Barracks
21081, Base Shield 21931, Power Generator 23626, Light Vehicle Factory 27096, **Command Center 32213**. The Research
Facility (750 HP all run) and the reinforcement point survive, so 0x13 never holds and the battle does not end. In the §1
run the CC lived and the Research Facility died (21349): which structure survives varies, the no-end result does not.

**Destroy_Structure** (191 records): single-unit task forces (one REBEL_TROOPER_TEAM or T2B_TANK, `count=1`) against each
base structure, the Research Facility most (17 starts), then the CC (11), Base Shield (9), Power Generator (8). Each run:
PSTART, PBLOCK k=1 (Produce_Force, ret true), then PEXIT about 12-14 s later with `MainForce:0`: the unit dies on the way,
no Attack_Move block ever returns.

**Burn_Units_Land** (28 records, 13 runs). Run 1 at 459.8 s (frame ~13794): the shipped `Find_Nearest(MainForce, "InBase",
PlayerObject, false)` gave nothing; the fallback `Find_Nearest(MainForce, PlayerObject, false)` gave the human's
STORMTROOPER_TEAM 161351 (444 from the CC); three 20 s Attack_Move blocks (482 / 502 / 522 s, 20 / 24 / 26 units, ret nil);
the thread was killed when the goal released the plan (run 2 starts at 578 s with `prev=none`). Runs 2-13 (578-1180 s):
PSTART, then PDONE 37 s later (Sleep 1 + 1 + 5 + 30) with no PBLOCK and no `near=`: both Find_Nearest calls found nothing,
although the CC (InBase, UNIT_AI) lived until 32213. `count=0` at PDONE is not evidence of an empty task force (the plan
calls Release_Forces before its last Sleep).

**Rebel army.** From ~13800 the army stands 200-600 from the human structures; the T2Bs nearest a structure sit at 209-221
from it (their Land_FOW_Reveal_Range is 220), troopers at 237+. Composition at 13800: 15 REBEL_TROOPER_TEAM (71 squads),
10 T2B_TANK, 4 MPTL, 3 spotters, 2 infiltrators.

**Port at the same point** (§5 run, `r2_utapau`): BURNUNITSLAND from 11494 every ~550-600 frames; plan 79 (11555) sends 22
units at the Base Shield, plan 117 (14000) 18 units at the Research Facility (dies 14574, the port's first structure
loss after the towers). The port's army stands 60-110 from the structures. Its composition differs from frame ~2300:

| | Retail | Port |
|---|---|---|
| First Mineral Processor (UC appears) | 1909 (both retail runs) | 1492 |
| Credits at 1400 | 5570 | 5570 |
| ~2300 purchase | 2295: -4520 (Kyle Katarn, 2 light tank brigades, an infantry squad, RL_Combat_Armor_L1; §2) | 2326: -2499 Tech_Upgrade_Emergency (RC_Level_Two_Tech_Upgrade, done 4127), 2494: -699 Purchase_Early_Land_Upgrades |
| Tech 2 done | 9295 | 4127 |
| Heroes | Kyle Katarn 3355 (Moldy Crow) | none (Land_Heroes from 12814, each run "ended failure") |
| Field at 13800 | 15 trooper teams, 10 T2B, 4 MPTL | 6 trooper teams, 8 infiltrators, 3 snowspeeders, 2 MPTL, 2 Plex teams (from 5492), a bacta tank |

Skirmish_Upgrade_Tech (AI_EQUATIONS_EXPANSIONGENERIC_LANDSKIRMISH) needs a built Mineral Processor, so the port's earlier
processor (1492 + 750 build frames = 2242) lets Tech_Upgrade_Emergency win at 2327, the moment retail spends on units. Why
retail's Build_Econ_Structure starts 435 frames after the pad capture (1474 -> 1909; the port 103) is not in these records:
the run had no hooks on the build plans.

**Open (run 2 of the kit, prepared 2026-11-12):** `make_plan_hooks.py` now also hooks Build_Econ_Structure,
Tactical_Multiplayer_Build_Land_Units_Generic, Tactical_Multiplayer_Build_Infantry_Emergency, Tech_Upgrade_Emergency,
Purchase_Early_Land_Upgrades, Purchase_Land_Upgrades_Generic, Land_Heroes, Droids, Secure_Contestable and Land_Scout_Area,
and records every BurnUnitsLand Find_Nearest call (FNEAR: the task force's count at the call, the result, and when it is nil
the same call without a player and with the Empire's player): it tells an empty task force (no position) from the Rebel's
fog. Same `run_skirmish_land_attack.cmd`; selftest `selftest.cmd` (SELFTEST_LUA covers FNEAR on both paths).

## 7. Why the port diverged before the attack (skai t12, 2026-11-12)

Read from the run 1 trace of §6 (`trace.q2t`, `lua_records.txt`; throw-away scripts, no binary read) against port runs
`alamo_headless --skirmish-ai foc-land --map _MP_LAND_UTAPAU --human Empire --seed 3379407547 --ticks 36000` (the retail
run's seed 0xc96daabb; draw order is the port's own, so only the timeline is compared).

**Goal pass length [H, trace].** Retail starts plans in batches 14-14.5 s apart (`lua_records.txt`, PSTART times of
Destroy_Structure / Burn_Units_Land), its first purchase is at frame 382, and the four later Mineral Processors start
363-616 frames after their pad changes owner (1470 -> 1909, 5240 -> 5854, 8128 -> 8713, 8755 -> 9118): a build waits for
the next goal commit, and a proposal pass takes about 390-430 frames. That is the quota of AI_GALACTIC §1.3 with the
whole PlayerList: the trace header gives `players=7`, one of them human, so the cap is (int)(20 / 6 + 0.5) = 3
evaluations per frame. The port divided by the battle's own players (4, cap 7): first commit at 168, pad capture to
build 103 frames.

**Goals per category [H, trace].** At the first commit retail's six trooper teams leave in three groups: 161385 and
161368 north (two Land_Scout_Area plans, category Tactical_Targeted), 161369 east to the outpost pad and 161387 to the
nearest mineral pad at 401-410 (two Secure_Contestable plans, category Map_Control), and their escorts 161367 and
161386 at 521-522. Two new goals per category is the Commit limit of AI_GALACTIC §1.6 step 5 with the AIPlayerType's
GoalSetExtensionSize (default 2); the port's tactical goal system kept its config default 1 and started one of each.

**TacticalBuiltStructureCount counts a structure under construction [H, trace].** The 2295 purchase is
Tactical_Multiplayer_Build_Land_Units_Generic (Katarn_Team, 2 Rebel_Light_Tank_Brigade, Rebel_Infantry_Squad) plus
Purchase_Early_Land_Upgrades (RL_Combat_Armor_L1): both need `Is_Initial_Skirmish_State_Established`, so
`Num_Refineries > 0`, 386 frames after the first UC_Rebel_Mineral_Processor appears (1909) and 364 before the finished
Rebel_Mineral_Processor replaces it (2659). Each goal has one function source (BasicLandGoalSet), so nothing else can
have made them fire. This settles the under-construction question of AI_TACTICAL §12 item 11.

Port after these three changes (dev, 2026-11-12):

| | Retail (run 1) | Port before | Port after |
|---|---|---|---|
| First commit / Droids_Team bought | 382 | 168 | 389 |
| Teams leaving at the first commit | 4 at 401-410, 2 escorts at 521 | 2, 1 escort | 4 at 401-405, escorts 527 |
| Nearest mineral pad changes owner | 1470 | 1389 | 1469 |
| First Mineral Processor starts / done | 1909 / 2659 | 1492 / 2242 | 1936 / 2686 |
| Outpost pad changes owner | 3338 | 14665 (§3) | 3389 |
| Early upgrade RL_Combat_Armor_L1 | 2295 | 2494 | 2333 |
| Purchase beside it | Katarn, 2 light tank brigades, infantry | tech 2 (2326) | tech 2 (2333) |
| Tech 2 bought | 7494 (2498 credits; done 9270) | 2326 | 2333 |
| Battle | no end in 36000 | Rebel wins 20207 | Rebel wins 18727 |

AI against AI, foc-land, 30000 ticks, seeds 1-6 (frame of the end; Rebel wins each): before 24866 / 26186 / 23038 /
25631 / 22518 / 19855; with the quota and the extension 26334 / 21659 / 23802 / 22630 / 22490 / 24824; with all three
21942 / 24479 / 21427 / 22930 / 21313 / 21549. The space outcome seeds (`--skirmish-ai foc`, 16000 ticks): 15 of 16
end, seed 3 runs into the 14400-frame limit and is left out of the tests.

**Still different: what the force sums and Find_Nearest see [M, trace; waits for run 2's FNEAR records].** Three
observations point at one rule, "an object with DUMMY_GROUND_STRUCTURE (the base structures: Command Center, Barracks,
factories, Research Facility, turbolaser towers...) is not in the perception grid's per-player object list", although
those types list UNIT_AI:
- `Skirmish_Upgrade_Tech` is 0 at 2295: tech 2 (2500) was affordable with 5300 credits and the unit plan ran instead,
  which needs `1 - Skirmish_Upgrade_Tech`. Its refinery count is the same token as `Num_Refineries` (true), the force
  ratio is near 1 either way, so `Variable_Self.FriendlyForceUnnormalized > 400` was false: the Rebels had 290 of units
  (6 trooper teams 180, droids 110) and 2100 of base structures (Barracks 300, two factories 600 + 600, two towers 300).
  Tech 2 is bought at 7494, the first commit with 2500 credits after the units passed 400 (540 at 3600); no unit is
  bought in between (the unit goal is off while the tech goal waits for credits).
- Units are bought again at 11620 (3697 credits) with tech 2 done (9270): the tech 3 term needs
  `FriendlyForceUnnormalized > 3000`; units + all structures were 3791 at 11400, units + the structures built on pads
  during the battle 1691. Tech 3 (3996) follows at 13352, when units + pad-built structures reach 3187.
- §6: BurnUnitsLand's `Find_Nearest(MainForce, "InBase", PlayerObject, false)` finds nothing while the human's Command
  Center stands, and its fallback finds a stormtrooper team. Find_Nearest walks the same per-player UNIT_AI list.

Port experiment with that rule (not on dev): at 2333 it buys RL_Combat_Armor_L1, Katarn_Team and a
Rebel_Light_Tank_Brigade (tech 2 "cannot afford"), tech 2 at 5924, tech 3 at 14018, and the idle-human Utapau run does
not end in 36000 ticks (the Command Center falls at 20074; Light Vehicle Factory, Base Shield and Power Generator
stand). But AI against AI stalls: foc-land seeds 1-6 do not end in 30000 ticks, because the enemy force that normalises
`EnemyUnitConcentrationNBTD` drops by the base structures too, so Clear_Infantry (100 x concentration) wins the first
commit for the Empire AI and takes all four start teams into the Rebel base; it never captures a mineral pad. Whether
retail does the same is not known (no retail AI-against-AI land run exists). The rule is held back until run 2 confirms
the Find_Nearest part (FNEAR with `fn_ret=nil` and `fn_any=nil` while the Command Center lives and `fn_count > 0`).

**Kit for a run 3:** `make_plan_hooks.py` adds to every land plan's PSTART record the values of six shipped equations
read with `EvaluatePerception(name, PlayerObject)` (the land goal system evaluates all of them for this player every
pass; none has a target or a `#` draw): `nsf` Non_Structure_Force_Unnormalized, `ut` Skirmish_Upgrade_Tech, `init`
Is_Initial_Skirmish_State_Established, `nr` Num_Refineries, `sav` Is_Saving_For_Refineries_Land, `burn`
Should_Burn_Units_Land. `ut` flipping while `nsf` passes 400 settles the first observation.
