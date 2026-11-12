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
