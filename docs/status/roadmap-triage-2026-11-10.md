# Roadmap triage proposal (2026-11-10)

**Applied 2026-11-10; parity-tier tags and tier-3 drops added the same day (`docs/ROADMAP_PROTOCOL.md`, "Parity tier")** (Gabriel approved it as drafted; 15 backlog entries were later re-filed into better categories, and a2m t06 was promoted to skai as b0004 -> skai t13). It gave each of the 25 open groups an exit test and one outcome (keep, close or park), and sorts every open task into stay, backlog or drop. Rules: `docs/ROADMAP_PROTOCOL.md`, "Backlog and group exit tests".

## Result

- Open groups: **25 -> 14** (4 closed, 7 parked).
- Open tasks: **127 -> 51** stay in groups; 33 go to the backlog, 2 are dropped, 41 stay in parked groups.
- Started groups (the limit counts these): 8 (a2m, ccpgu, modc, q6, r1b, s78n, skai, storyb); proposed limit 8, so `lint` shows no wip warning. Waiting groups (q4, qafull), Gabriel-only groups (q4d, wf1) and unstarted ones (opt3f, bm4) are not counted.
- Every kept group has an exit test, none is over the cap of 8 open tasks.

## Keep (with an exit test)

### a2m: A2 follow-ups 8

**Exit test:** Over 12 seeds, the Underworld and Rebel galactic scenarios (hotm) land every galactic-AI metric inside the retail range: credits at 4500 / 9000 / 13500, build count and Shola capture time (parity harness, distribution mode).

t01 (Underworld credits regression), t02 (Move_To port) and t03 (Rebel build count) are the tasks that move those metrics. t06 is the land skirmish that never ends, which is skai's exit test, so it moves there. The saboteur and slicer detail work (t08-t12, t14, t15) is draw-order and precision chasing, which the parity-tiers decision (2026-11-09) says to do only when a tier-2 metric fails.

| Task | Outcome | Text |
|---|---|---|
| t01 | Stays | Underworld credits_at_9000 = 0 after the a2l t03 ports (t02 fix alone: 2277 PASS; retail 2462..2849), credits_at_4500 374 vs 1106..2756, credits_at... |
| t02 | Stays | Port A2L_T01_RE_ANSWERS Q8: Move_To moves only the elected task-force fleet (TF +0x120; elected in Add_Force 0x6cd1a0, re-elected on signal 0x10 /... |
| t03 | Stays | Rebel after a2l: build_count 19 vs retail 32..50 (from the t03 lane H ports: builds 40 -> 22), credits_at_13500 1890 vs 23..899 (Shola taken at 128... |
| t04 | Backlog S2 | RE pass (separate agent, docs/re only): Setup A exclusions for galactic units (obj +0x210 (+8 / +0x34), +0x3A7, +0x3B4, lock 0x39ad40, attach +0x35... |
| t05 | Backlog S2 | Port the t04 answers plus the Setup A hardpoint minimum in the health test (the galactic store keeps no hardpoint health) |
| t06 | Backlog S2 then promote to `skai` | [port check] foc-land skirmish: seed 2 does not end in 30000 ticks on dev (Rebel idles at 40000 credits in LANDSCOUT / SECUREBUILDPAD / CLEARVEHICL... |
| t08 | Backlog S3 | [RE] (a2k t05) Precision of Game.Age 0x6e1f20 vs TimeSinceCorruptionChange 0x65f110 (docs/re/A2K_T05_SABOTEUR_RUN.md) |
| t09 | Backlog S3 | [port] (a2k t05) Match that precision in the port's Underworld saboteur desire inputs |
| t10 | Backlog S3 | [RE] (a2k t05) Why the Corellia saboteur plan exits at 248 (retail) and the saboteur stops at Naboo |
| t11 | Backlog S2 | [port check] (a2k t05) Underworld proposal cadence: retail 103/250/397 vs port 119/195/271 |
| t12 | Backlog S3 | [kit+docs] (a2k t05) a2f kit: stage naming, longer desire samples, a 30-min run option; mark the pre-fix recorder artefacts in a2f README s8 and A2... |
| t14 | Backlog S3 | [RE] (a2m t07) FindTarget("Enemy",0.1) candidates for the slicer; run 11 with no slice |
| t15 | Backlog S3 | [port check] (a2m t07) Can_Afford_Steal_Tech after tick 8326; the respawn timer draw when nothing is affordable (align apply_slicer) |

### s78n: S7/S8 follow-ups 9: ability range and accept leftovers

**Exit test:** Ground-unit collision and path-around follow the MOVEMENT spec (tier 1): the t20-t25 unit tests pass, the land_battle liveness measurement shows no unit stuck, regenerated goldens are named in the commit, MSVC and clang are green.

Claimed by another session (movement-combat-workflow-842cd2): only the exit field is added, no task is touched. 7 open tasks, within the cap.

| Task | Outcome | Text |
|---|---|---|
| t20 | Stays | [port, on branch, not landed] Collision batch A: formation destination check 0x439360 (MOVEMENT 4.3.1), Path Around as a visibility-graph search wi... |
| t21 | Stays | [RE done ac2750e3, f17c4fb4 / port] Collision batch 1: Path Around edge and crossing rules (MOVEMENT 1.5.1 item 4a: inward 45-degree bisector, trac... |
| t22 | Stays | [RE done 5e72f4ac / port] Collision batch 2: zone validation 0x50f480 of an ordered point and the destination check from the validated goal (MOVEME... |
| t23 | Stays | [port] Collision batch 3: MovingVsMoving chooser for all pairs with the synced retry draw (MOVEMENT 5.4.6), its state-1 speed and impassable-cell v... |
| t24 | Stays | [RE done 3e08fd61 / port] Collision batch 4: TerrainCollision 0x509090 / 0x687ff0 (MOVEMENT 5.4.7) replaces the port's blocked-frames re-plan; own... |
| t25 | Stays | [RE done 0f8573a9 / port] Collision batch 5: attack approach on land (MOVEMENT 4.3.2): polar arc slot 0x438c10, slot leg, fire trim 0x410e90; attac... |
| t26 | Stays | [decide after t25] Three stand-ins found while planning that go against high-confidence spec text: Destroyer Droid walking while deployed (Max_Dist... |

### skai: Land skirmish AI plays a full game (from playtest QA t50)

**Exit test:** A land skirmish between two AIs plays to a victory condition: foc-land seeds 2 and 4 end within 30000 ticks, and the idle-human Utapau run does not end within 36000 frames (as retail); land goldens regenerated.

Takes a2m t06 (the foc-land seed that does not end). t13 is an RE question that no task needs yet.

| Task | Outcome | Text |
|---|---|---|
| t07 | Stays | [port] Land skirmish start forces on the slot's spawn marker (tactical_battle_bt.cpp:1082 overwrites it with the Defending Forces marker) |
| t08 | Stays | [port] Pre-build all 9 base structures per slot (bt_skirmish_base, tactical_battle_bt.cpp:1286) |
| t09 | Stays | [port] Charge the AI for pad builds (skipped by if (!ai) at tactical_battle_bt.cpp:302); AI production is paid in full at enqueue |
| t10 | Stays | [port] Enforce Build_Limit_Current_Per_Player (tactical_battle_hangar.cpp:531; a second Han lands at 13895) |
| t11 | Stays | [port] Destruction_Survivors spawn the company's units, not the dummy company object (tactical_battle_oi_abilities.cpp:616) |
| t12 | Stays | [port] Imperial CC self-repair and magnetically sealed hull in land skirmish: the idle-human Utapau run must not end within 36000 frames; regenerat... |
| t13 | Backlog S2 | [RE] What the land unit cap 0x2ac5c0 counts (13-14 companies vs a curve of 9) and what stops pool drops after 15232; correct the pay-as-you-go read... |

### q4: Q4 / Milestone 1 parity check

**Exit test:** Parity harness in distribution mode, at least 12 seeds per scenario (hotm_galactic, geonosis_skirmish): every tier-2 metric inside the retail range and the tier-1 lane speed equal to retail; baseline re-recorded.

Waits on skai (its `after`); q2f and q4hf closing removes two of its blockers. t02 is a tier-1 constant (hyperspace lane speed 2.62 vs 3.15).

| Task | Outcome | Text |
|---|---|---|
| t01 | Stays | Run the comparison harness (group Q4 harness) after OPT2 and compare against its pre-OPT2 baseline: travel and build timings, damage, income, AI ch... |
| t02 | Stays | [from q4hf t04, 2026-11-08] Underworld Nal Hutta -> Kashyyyk leg 25 ticks vs retail 21 in every retail run: the port's fleet flies that lane at 2.6... |
| t03 | Stays | [from q4hf t02, 2026-11-08] geonosis_skirmish combat.first_shot_frame 2150 vs retail 3953 (dev a6a7c09c): the port's Rebel AI attacks ~60 s early,... |

### q4d: Q4 harness: distribution mode (parity tier 2)

**Exit test:** At least 6 retail runs per scenario recorded and added; the harness grades tier 2 across seeds; baseline re-recorded.

Gabriel's capture run (about 1 h 50 min, hands-off) is the only thing blocking it.

| Task | Outcome | Text |
|---|---|---|
| t03 | Stays | Gabriel: double-click tools/probe_mod/parity_refs/run_capture.cmd with Steam running (6 Heart of the Maw + 6 Geonosis runs, about 1 h 50 min, hands... |
| t04 | Stays | After t03: run the batch's add_refs.cmd in the main checkout, check the new runs with parity_harness.py retail (leave-one-out), re-record tools/par... |

### q6: Q6 Thrawn's Revenge mod compatibility

**Exit test:** Thrawn's Revenge: the campaign (GCMenu) starts and a story session reaches the planet handouts with correct victory handling; the t01 / t02 re-check on the final sim passes after Q4.

| Task | Outcome | Text |
|---|---|---|
| t04 | Stays | After Q4 lands: re-run t01 and t02 on the final sim and fix anything the OPT2 / Q4 changes moved |
| t21 | Stays | [port] (t15) Victory: drop EliminationLatch / player_eliminated (engine/galactic/src/victory.cpp:174-231, victory.hpp:26-34 and 81-100, engine/apps... |
| t22 | Stays | [port] (t15) load_victory_rules (victory.cpp:164-172): Human_Victory_Conditions for the Starting_Active_Player faction, AI_Victory_Conditions for t... |
| t23 | Stays | [port] (t15) Vanilla objects run their Lua_Script: remove the 'object scripts only with a mod layer' interim rule (engine/apps/game/src/game_sessio... |
| t24 | Stays | [RE] (t15) TR GCMenu: in which order its story hands out planets (Change_Owner) and whether it holds victory with ENABLE_VICTORY 0; with the owner-... |
| t26 | Backlog S3 | [port, low] (t15) Tests: STORY_ELAPSED T = 0.7 alternates 22 / 23 frames; Select All (sim_host.cpp:787-809) leaves build pads, buildings and the ow... |
| t27 | Backlog S3 | [RE, low] (t25) End timing: ELIM D ends at most 1.0 s after its owner change (planet to an AI), ELIM E 4.3-5.3 s after its take-back (planet to Neu... |

### modc: MODC Mod-agnostic compatibility system

**Exit test:** The second installed mod (workshop 1770851727) reaches its main menu and a skirmish through alamo_game, and no engine code depends on a mod's names or content.

34 tasks of which 24 are done: the open ones go from 10 to 5.

| Task | Outcome | Text |
|---|---|---|
| t09 | Stays | Gabriel: run the second mod in game and report what breaks |
| t26 | Backlog S3 | Shader model 3 leftovers in engine/shaderxlat (modc t22 covers what the installed mods use; docs/MODS.md 4.1): vs_3_0 / ps_3_0 varyings with other... |
| t27 | Backlog S2 | Run the .fx prebuild without a manual step (modc t23 follow-up; docs/MODS.md 4.1 'Effect sources'). Today the loader takes a compiled .fx from the... |
| t28 | Backlog S3 | Unread-tag coverage per owner (modc t25 follow-up; docs/MODS.md 5): objects::find_field_schema falls back to any owner's row, so the load report co... |
| t29 | Stays | engine/objects field schema: kind_of_code maps descriptor code 0x50 to Raw, but docs/re/xml_field_tables.md (modc t25) shows 0x50 is a bit-flag boo... |
| t30 | Stays | RadarMap parity (modc t25; docs/re/xml_field_tables.md, table 0xa39c50): in the original the Radar_Map_Event fields carry over from one Radar_Map_E... |
| t31 | Backlog S3 | Mod picker (t08) follow-ups: (a) a list item wider than its listbox is drawn squeezed horizontally (long mod names in IDD_PORT_MODS_DIALOG); find t... |
| t32 | Stays | Object scripts (modc t18 follow-up, OBJECT_SCRIPTS.md §1.3): a battle's player world is now bound to the store before the map objects spawn, but th... |
| t33 | Backlog S3 | Space-skirmish movement cost with very large obstacles (modc t06, docs/MODS.md §9): workshop mod 1770851727's space skirmish on its Geonosis map wi... |
| t34 | Stays | alamo_game front end with non-TR mods (modc t06, docs/MODS.md §9): the hidden skirmish smoke (alamo_game --smoke skirmish --hidden --audio null STE... |

### r1b: R1b hardpoint follow-ups: Lua named damage, attached model, engine glow

**Exit test:** Hardpoints match retail: Lua Take_Damage(n, name) damages the named hardpoint (an unknown name stops the script like retail), the provisional attach-child rule is gone, and the Model_To_Attach slot hides when its hardpoint dies.

| Task | Outcome | Text |
|---|---|---|
| t01 | Stays | [port] Lua Take_Damage(n, name): match the name case-insensitively against the type's HardPoints and damage that CombatSystem hardpoint; no match:... |
| t02 | Stays | [port] Delete the provisional attach-child rule: sim_snapshot.cpp:473-487, sim_snapshot.hpp:17-21, SnapshotObject::attach_parent (hpp:213) if unuse... |
| t03 | Stays | [port] V1: hide the Model_To_Attach slot when its hardpoint dies (hardpoints.hpp:17, hardpoints.cpp:158-164); for now: when the hardpoint has a bre... |
| t04 | Backlog S3 | [port] V3: engines-off visual turning off pe_stardestroyerengines; the sim has engines_offline (tactical_battle.cpp:3074); snapshot flag + renderer... |
| t05 | Stays | [RE] V1: the path that removes Model_To_Attach (SPACE_AND_OPEN s B.10 answer 4): breakoff spawn, 0x3a5700, the attachment list at 0x12d770 |
| t06 | Backlog S3 | [RE] V3: what turns the glow off (ENGINE offline -> Locomotor_Set_Engines 0x5545a0, TACTICAL_COMBAT.md:2359); does an ion stun do it too |
| t09 | Backlog S3 | [port, check] (t08) Retail: Lua Point_Camera_At(position) from a free-store script moves the tactical camera to that point at the current zoom. The... |

### storyb: Story follow-ups: map hints, Kessel story parity

**Exit test:** The Kessel UM01 mission runs in the port as in the retail trace: map hint names load, the mission script does not stop, and a story_kessel parity scenario passes.

| Task | Outcome | Text |
|---|---|---|
| t01 | Stays | [port] Load map hints: parse .ted chunk 0x44C/0x482/0 mini 01 into ObjectPlacement (engine/terrain/src/ted_read.cpp:588, ted.hpp:256; keep it in th... |
| t02 | Stays | [port] --human accepts a faction whose campaign AI type is ScriptableHuman (engine/apps/headless/ai_run.cpp:196-213) |
| t03 | Stays | [port] Write the tactical battle's objects to --trace-out in --galactic-ai runs while a battle is on top (ai_run.cpp:459-470, as skirmish_run.cpp:8... |
| t04 | Backlog S2 | [port] Call Story_Handle_Esc as the engine does (after t05) |
| t05 | Backlog S2 | [RE] How and when the engine calls Story_Handle_Esc (string 0x85dde0, xref 0x328440), synced command path or not |
| t06 | Backlog S3 | [RE] Retail has 8 players in UM01 (human index 6), the port 7 (index 5): which faction is the extra one |

### ccpgu: Capture Control Points follow-ups

**Exit test:** Capture Control Points: the announcer lines play at the 60 / 30 / 15 / 5 s marks for the local player, and the headless type-4 skirmish AI is shown to go for control points (or retail's AI is shown not to).

| Task | Outcome | Text |
|---|---|---|
| t01 | Stays | alamo_game: play the SFXEvent_HUD_Reinforcement_Point_{Owned,Ally_Owned,Enemy_Owned}_* lines at the 60 / 30 / 15 / 5 s marks of TacticalBattle::con... |
| t03 | Stays | Check whether the tactical AI goes for control points in a type-4 skirmish (alamo_headless --skirmish-ai foc-land --map _MP_LAND_ENDOR --control-po... |

### wf1: WF1 Parallel-agent workflow

**Exit test:** The release PR dev -> main is merged (build_gate.cmd green on the tip being merged).

Only Gabriel's merge is left (t15). Set to status gab so it shows in Needs Gabriel and is not counted as started work. PR #105 is behind dev now; re-gate before merging.

Status set to `gab`.

| Task | Outcome | Text |
|---|---|---|
| t15 | Stays | Gabriel: merge `dev` into `main`: release PR #105 (head dev 44a7c384, 281 commits, mergeable, CLEAN). build_gate.cmd on 44a7c384 2026-11-09 (after... |

### opt3f: OPT3f First battle tick under 33.3 ms

**Exit test:** bench_msvc.cmd on a quiet PC: the first battle tick is under 33.3 ms for land, space and skirmish, recorded in docs/PERFORMANCE.md.

Not started; stays in the queue.

| Task | Outcome | Text |
|---|---|---|
| t01 | Stays | Plan VM load (~20 ms wall land / space: Lua parse of the PG libraries in every plan VM): build the VMs before the battle tick (needs the AI world w... |
| t02 | Stays | Land skirmish model loads the session-load warm (TacticalBattle::prepare_models) misses: team-marker pads of the slot factions, base structures, pr... |
| t03 | Backlog S3 | Remaining serial skirmish setup on the sim thread (loaded-PC thread cycles): pad-type table build (bt_add_pad_types / extend_battle build_units), T... |
| t04 | Backlog S3 | Space foc-station seed 3 frame 9818 (one long path request, still 40-50 ms on a loaded PC after the grid index): measure on a quiet PC; if still ov... |
| t05 | Stays | Re-run bench_msvc.cmd on a quiet PC and record the first battle ticks (land, space, skirmish) in docs/PERFORMANCE.md |

### bm4: BM4 In-game benchmark screen

**Exit test:** The benchmark runs from the menu (full or one scene), shows median, p99 and worst frame per scene and the report path, and Escape cancels cleanly; Gabriel runs it on this PC.

Not started; stays in the queue.

| Task | Outcome | Text |
|---|---|---|
| t01 | Stays | Benchmark entry in the options or main menu, choosing the full run or one scene, with the expected duration shown |
| t02 | Stays | Results screen with median, p99 and worst frame per scene, system details, and the path of the saved report |
| t03 | Stays | Escape cancels cleanly and returns to the menu, with the player's settings restored and no effect on their saves |
| t04 | Stays | Show a clear message when required game data or a scenario is missing or out of date |
| t05 | Stays | Gabriel: run the full benchmark from the menu on this PC and report any problems |

### qafull: Full QA pass (after the core systems)

**Exit test:** Gabriel's playtest and a fresh 4K QA pass are done and every defect is filed (S1 / S2 backlog entries, not new groups).

Waits on the core systems (its `after`).

| Task | Outcome | Text |
|---|---|---|
| t01 | Stays | Gabriel's playtest: confirm what only tests or smoke shots have covered: hit sparks under fire, one squadron marker, station break-up and fade at b... |
| t05 | Stays | Fresh deeper pass: record a new 4K capture over galactic, land and space battles, menus and dialogs, review it frame by frame like docs/qa/QA_2026-... |

## Close now

### q2f: Q2 follow-ups: fix the parity gaps the traces found

**Passed:** t01-t08 done; the remaining Rebel galactic production gap is the same metric as a2m t03.

| Task | Outcome | Text |
|---|---|---|
| t09 | Drop: Same metric as a2m t03 (Rebel build_count and credits vs retail); tracked there | Rebel galactic production vs retail. Done 2026-11-08: sync free-store gather ported (dev f88d6c1d, RE docs/re/Q2F_T09_RE_ANSWERS.md, AI save v7); t... |

### q4hf: Q4 harness follow-ups

**Passed:** t01-t04 done: baseline re-recorded, skirmish trace hook, two more retail references, hop segmentation.

| Task | Outcome | Text |
|---|---|---|
| t05 | Backlog S2 | Auto-resolve parity ctest against tools/parity/autoresolve_refs.json (engine/galactic/src/auto_resolve.cpp:165) |
| t06 | Backlog S2 | Port skirmish: the idle-human Empire builds 19 and loses 8 (retail reference 20261108-204213): find why the port's human side acts |

### cu8m: CU8m cu8l leftovers: indigenous spawner, unmodelled adopt sites, retail traces for the port guesses

**Passed:** cu8l ports landed and tier-2 metrics hold; draw order is tier 3 (parity-tiers decision, 2026-11-09), chased only when a tier-2 metric fails.

| Task | Outcome | Text |
|---|---|---|
| t01 | Backlog S2 | [port] Indigenous spawner (SpawnIndigenousUnits): its two land containers per spawn compose at the same tick's step 10, so Endor composes 12 at gf... |
| t02 | Backlog S3 | [port] Adopt / lock sites the port has no model for yet, each once modelled: cable wraps and lock, LANDING touchdown, lone fighter hyperspace-in fi... |
| t03 | Backlog S3 | [RE / retail trace] Settle the cu8l port guesses (docs/PHASE4.md cu8l t01-t03 lines): space skirmish rule 3 (CU8K_T02_T06_RE s1.5, arm before Start... |
| t04 | Backlog S3 | [test] Golden coverage for the cu8l t02 sites: a scenario where a non-squad hero or vehicle uses a stop-and-hold ability, garrisons and gets a scri... |
| t05 | Drop: Low priority and needs a retail trace first (task text) | [port, low] Activate_Ability leftovers: a task-force argument uses the task force's own position (vf+0x170, trace first) instead of its first live... |

### fowf: Fog of war follow-ups

**Passed:** t02-t05 done (RE in docs/re/FOG_OF_WAR.md); the remaining land fog box change is a coupled fog / AI change that needs a seed sweep.

| Task | Outcome | Text |
|---|---|---|
| t01 | Backlog S2 | Map extents: traced (FOG_OF_WAR §1.2 [H]: land = the terrain box [0, 20 W] x [0, 20 H], space = the playable box); switch the land fog box together... |

## Park

### cb1: CB1 Bridge view: bridge camera, holo-table, ship status

Park: Side goal (captain's view, Gabriel 2026-11-03), not on the path to playable. Unpark when the core systems are done. (7 tasks kept in the group, status `parked`).

### cb2: CB2 Subsystem targeting from the captain's chair

Park: Side goal (captain's view); follows cb1. (5 tasks kept in the group, status `parked`).

### cb3: CB3 Fleet orders from the captain's chair

Park: Side goal (captain's view); follows cb1. (5 tasks kept in the group, status `parked`).

### cb4: CB4 Cinematic moments

Park: Side goal (captain's view); follows cb1. (5 tasks kept in the group, status `parked`).

### fp1: FP1 Possess a unit: direct control, eye camera, hitscan

Park: Side goal (first-person ground unit, Gabriel 2026-11-03), not on the path to playable. Unpark when the core systems are done. (8 tasks kept in the group, status `parked`).

### fp2: FP2 Aimed fire: projectiles, cooldowns, camera collision

Park: Side goal (first-person); follows fp1. (5 tasks kept in the group, status `parked`).

### fp3: FP3 First-person polish: HUD, animation, audio, leave and respawn

Park: Side goal (first-person); follows fp2. (6 tasks kept in the group, status `parked`).

## Settings

- `meta/backlog` `wip` 6 -> 8 (started groups at once).
- Tools already added for this: `trim-group`, `park` / `unpark`, a cap that counts open tasks only, a limit that counts started groups only.

## How it would be applied

One script runs the plan with the new commands (`trim-group`, `close-group`, `park`, `write` for the `exit` fields and `wf1` status, `backlog promote` for a2m t06), then `lint` and `sync`. Each step is its own commit in `roadmap-store`, so any of it can be reverted. Groups claimed by another session (s78n) only get their `exit` field.
