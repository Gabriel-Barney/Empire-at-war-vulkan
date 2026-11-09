# q6 t15: retail runs of the Q6 [port] rules, 2026-11-09

Kit `tools/probe_mod/q6_t15_probes/` (README: questions, setup, port expectations). FoC Steam 1.121, driven by Claude
through computer use.

- Vanilla session `out/20261109-070547-vanilla/` (exit 0, 1508 records): `report.txt`, `records.log`, `screenshots/`
  (the PNGs show only the top-left ~64 % of the 4K screen, `screenshots/NOTE.txt`; the Select All readings come from
  the live screenshots taken at the same moments).
- TR session `out/20261109-073043-tr/` (exit 10, no records, expected): only the GCMenu part; Blood Gulch was not run
  (DECISIONS.md "Probe runs trimmed"). `screenshots/gcmenu_still_running_3min.png`.

## Results

| # | Question | Retail | Port | Verdict |
|---|---|---|---|---|
| 1A | ELIM A: human owns nothing all game | `RAN_TO_END`, no end in 170 s | goes on | MATCH [H] (for another reason, see below) |
| 1B | ELIM B: given Abregado Rae at t = 21.97 s, taken back at t = 51.93 s (f = 1558) | last heartbeat is the TAKE record; the campaign ended within 5 s: credits, then "Defeat! You have been defeated" (`elim_b_defeat.png`) | ends at the first check after TAKE | MATCH [H] (outcome; mechanism differs, see below) |
| 1C | ELIM C (control): AI side `Galactic_Opponent_Controls_No_Planets`, human owns nothing from the start | `RAN_TO_END`, the 170 s message appeared, no end | ends at once | **DIFFERS** [H] |
| 1 TR | GCMenu, no pick, 3 min 20 s | did not end | goes on | MATCH [M] (by eye) |
| 2a | Object-script first-pump draw bound | every draw in [0, ServiceRate] (max r/rate 0.999 at rate 1, 0.962 at 20, 0.964 at 6); next pump at the next service | hi = `ServiceRate` | MATCH [H] |
| 2b | Do vanilla objects run their `Lua_Script`? | yes: 49 of 54 vanilla-rate instances pumped at least twice, land and space | only with a mod layer (`[port interim rule]`) | **DIFFERS** [H]: the interim rule can go |
| 3 | Perpetual `STORY_ELAPSED T` period (trimmed, came for free) | Cycle2 61 frames (38 of 39; one 62), Cycle1 31, Cycle40 1201, Cycle07 22/23 (T×30 = 21); Once2 and Cycle2 both first fire at f = 60 | T×30 + 1 (61 for T = 2) | MATCH [H] (T = 0.7 splits 22/23, [M] float accumulation) |
| 4 | Load-time story resume | `MAIN` at f = 0 before the Begin0 fire (`TRIG` f = 1); `ENTER Q6S_Begin0` at f = 7 | threads resumed before the frame-1 dispatch | MATCH [H] |
| 5 | TR Blood Gulch end time | not run (trimmed) | — | not measured |
| 6 | Select All (Ctrl+A) | land: both build-pad turrets, the AT-ST, Darth Vader and every stormtrooper squad selected (one panel slot and one ring per squad / TEAM); barracks, turbolaser tower, command centre and build pads not selected; mine off screen. Space: Star Destroyer, Accuser, Eclipse and the TIE squadron selected; own space station not; defense satellite off screen. After the Star Destroyer launched fighters the panel looked the same | COMMANDS §2.4.1 | MATCH [M] (by eye; mine, satellite and launched fighters not judged) |

## ELIM C: why retail does not end it [H]

The galactic victory types 0 / 1 / 4 / 5 / 9 are **not polled per tick**. Their only caller is the planet's
derived-owner change 0x3fb040 (the "%s: now controlled by %s (owner player ID %d)" path, port
`GalacticSim::on_owner_change`). When a planet's owner changes, it tests, for every player in PlayerList order,
0x28f570(GMM, 0 / 1 / 4 / 5 / 9, player) and stops at the first pass (0x3fb040 tail, after the TEXT_PLANET_CHANGED_HANDS
message). The first call for a planet (+0x1c9 still 0) sets the owner and skips the tests, so the starting owners
test nothing. The other galactic triggers are type 10 at each week rollover and type 8 every 30 frames (galactic tick
0x4b2ef0), and type 2 in Add_Credits 0x27f370. In ELIM A and C no planet changes owner, so the Rebel's
`Opponent_Controls_No_Planets` is never evaluated and the campaign runs on.

Why B ends: campaign setup 0x289a90 (per player, from 0x28dbe0) gives no records to an `Is_Neutral` faction (+0x108).
A player whose faction is the campaign's `Starting_Active_Player` (+0xa8), or any player when param_4 ≠ 0 ([M]:
multiplayer), gets the `Human_Victory_Conditions` list (+0x268). Every other player gets the `AI_Victory_Conditions` list
(+0x2d8) **plus one extra type 1 record** (`Opponent_Controls_No_Planets`, countdown 30 frames, no faction filter),
added only when that list is non-empty. B's TAKE is an owner change (Lua `Change_Owner` reaches 0x3fb040). The AI's
implicit type 1 then passes because no enemy playable player owns a planet, and the AI wins after 30 frames: "Defeat"
for the human. At GIVE the same test failed, because the human owned Abregado Rae.

Type 1 in 0x344710 (case 1): P's faction must be `Is_Playable` (+0x20a8). A planet blocks only if its owner is an enemy
(relation 1, 0x2824f0) with a playable faction (name list, if any, case-insensitive). The test **skips planets whose
owner and P are both AI-controlled and not human** (player +0x360 set, +0x62 = 0; 0x2814d0). In multiplayer galactic
(0x35f6b0: game type 1 / 2 / 4), planets of non-human owners are skipped too, and an AI P answers "no" at once (gate at
the top). So in single player an AI's type 1 means "no human enemy owns a planet". That is the original's only
"elimination": planets only, no units (a human with fleets but no planet still loses), and tested only at owner
changes.

Port consequences (the latch is not the original's mechanism):
- A human who owns nothing is safe only until the first owner change anywhere. Then each AI's implicit type 1 passes
  and the campaign ends. A TR story that hands out AI planets before the human's, without an `ENABLE_VICTORY 0` hold,
  would end at once in retail [M, TR scripts not read].
- `player_eliminated` counts live units; the original does not.

Probe that would confirm the two inferred halves (not run): ELIM D, the human owns nothing and the script moves one
neutral planet to the Rebel at t + 20 s (expected: the AI wins about 1 s later). ELIM E, the human owns a fleet and one
planet, and the script takes the planet (expected: ends, so units do not count).

## Open

- Probe 5 (TR Blood Gulch) stays unmeasured by decision.
- Select All: mine, defense satellite and docked / launched fighters not judged (off screen or not visible in the
  panel).
