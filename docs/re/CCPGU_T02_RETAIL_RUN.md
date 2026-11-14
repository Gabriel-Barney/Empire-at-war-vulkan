# ccpgu t02: Capture Control Points retail run 20261108-214851

Kit `tools/probe_mod/ccp_probe/`, run `out/20261108-214851`, re-parsed in `report_v2.txt`. FoC Steam 1.121, game type
shown as "Land Control", map "(4) El Kamino", Empire human vs Easy AI Rebel, no orders; the Lua hook spawned
invulnerable Imperial troopers on the one neutral `Reinforcement_Point` (obj 82036).

## Result [H]: retail matches SKIRMISH_SETUP §4.2 and the port

| tick | what (memory reader, mode +0x2b0 / victory monitor) |
|---|---|
| 454 = C | point 82036 owner 3 -> 0 (Empire); armed byte 1, timer 899, player 0 (same tick) |
| 455..1352 | timer + tick = 1353 on every line |
| 1353 = C + 899 | timer 0, declared record {type 21 = 0x15, player 0}, countdown 210; then disarmed (byte 0, timer -1, player -1) |
| 1353..1563 | countdown + tick = 1563 on every line; no decrement in the declaring tick |
| 1563 = C + 1109 | countdown 0; the monitor is freed and the mode ends inside this tick (the frame counter stops at 1562) |

So capture -> end = 1109 = 900 + 210 - 1, as the port (`engine/apps/sim/src/tactical_battle_victory.cpp:115` arm 900,
`:331-335` decrement and test, `:328` countdown, `:358` end; capture service before `check_victory`,
`tactical_battle.cpp:4494` / `:4513`). The [M] ±1 of §4.2 on the tick order is resolved. Inside tick 1353 retail
writes the declaration before it clears +0x2b0 (the port disarms first, then tests; not observable).

## Memory facts

- Mode +0x2b0 is a byte; +0x2b1 read 1 throughout (raw block at frame 0 `00010000 ffffffff ffffffff`).
- The player list (RVA 0xA16FD0) at mode frame 0 is still the pre-remap one (faction order: Rebel 0, Empire 1, ...,
  no human flag); after the §2.1 remap the ids are Empire 0 (human), Rebel 1, Neutral 3.
- The mode frame counter (+0x10) is written after the tick's sim state: 22 polls saw a tick's change still stamped
  with the previous frame.

## Faction-fixed points

At frame 0 the game also had `Reinforcement_Point_Empire` (owner Empire, 2734.7, 3536.5) and
`Reinforcement_Point_Rebel` (owner Rebel, 2919.4, 2023.9), exactly on the map's `Team_00_` / `Team_01_Reinforcement_Point_Plus2_Cap`
team markers (MULTIPLAYER_STRUCTURE_MARKERS.XML: `Marker_For_Specific_Object_Type` Underworld / Rebel / Empire points).
The skirmish setup replaces a team marker by the point whose Affiliation is slot NN's faction, owned by slot NN; slot 0
was the human Empire. The port does the same: `tactical_battle_bt.cpp:740-766` (`team_pad`) and `:1267`
(`skirmish_slot_player`). No CAPTURE_POINT, not relevant for 0x15. All 24 `_MP_LAND_*` maps carry such markers (51).

## Open

- Multi-point arm test (the "every other relevant point allied" rule) not exercised: Kamino has one point.
  Bothawui (2 points) would cover it.

## ccpgu t04: multi-point run, Bothawui (run 20261109-045610)

Kit `tools/probe_mod/ccp_probe/` with `--map _MP_LAND_BOTHAWUI --scenario steal`; Empire human vs AI Rebel, no orders.
Two neutral `Reinforcement_Point`s (obj 69364 at 2332,1310; obj 69363 at 1642,2420). The hook put invulnerable Rebel
infantry on 69363 and Empire troopers + AT-AT on 69364 at frame 150, despawned the Rebel units at 1050 and put Empire
troopers on 69363 at 1110. Owner changes (mode ticks), retail armed / timer / player after each, against the port rule
(`vm_cp_capture`: Neutral new owner disarms; arm only if every other relevant point is the new owner's) replayed on them:

| tick | point | change | retail armed/timer/player | port | verdict |
|---|---|---|---|---|---|
| 452 | 69364 | Neutral -> Empire | 0/-1/-1 (69363 still Neutral) | 0/-1/-1 | MATCH |
| 453 | 69363 | Neutral -> Rebel | 0/-1/-1 | 0/-1/-1 | MATCH |
| 1413 | 69363 | Rebel -> Neutral | 0/-1/-1 | 0/-1/-1 | MATCH |
| 1921 | 69363 | Neutral -> Empire | 1/899/0 | 1/899/0 | MATCH |

The enemy takeover went through Neutral (1 via Neutral, 0 direct), as §4.2 says. Declaration {0x15, Empire} at tick 2820 =
last capture + 899, countdown 210, countdown 0 / battle end at 3030 = C + 1109 (frame counter stops at 3029).

**Verdict [H]:** the multi-point rule matches the port: a capture while another relevant point is not the capturer's
(Neutral or enemy) does not arm; Rebel -> Neutral -> Empire; the capture that completes the set arms 899 and the win comes
1109 frames later. Two further reader rows at the teardown (owners read as -17891602 after the mode ended) are garbage
and are ignored by `ccp_parse.py` (owner ids above 15 are not owners). Report: `out/20261109-045610/report.txt`
(re-parsed after the parser fix).
