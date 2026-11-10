# s78n t08 retail run: tractor-beam footprint range and Combat_Bonus on an allied player (2026-11-10)

Kit `tools/probe_mod/s78n_t08/` (`run_s78n.cmd`), run `out/20261109-200249` (copy in the main checkout's
`tools/probe_mod/s78n_t08/out/`), driven by Claude with computer use while Gabriel was away. Retail FoC, windowed,
two skirmish battles in one game session. The kit's own verdicts were both INCONCLUSIVE; the reading below is by
hand from `report.txt` / `records.log`.

## Setup notes
- Space: Kuat, human Empire (team 1) vs Easy AI Rebel (team 2).
- Land: Dagobah Rush. The skirmish lobby refuses two factions on one team ("All players on each team must be of
  the same faction"), so the Underworld Easy AI was put on team 3 and the probe made it an ally with `Make_Ally`
  both ways (relations then Empire <-> Underworld 1). The Rebel Easy AI was team 2.

## (A) Tractor beam range includes the target footprint (TACTICAL_COMBAT §5.4.2)
Star_Destroyer `TRACTOR_BEAM` (R = 1000) against a Nebulon_B_Frigate, which the kit gives E_x 23.48 / E_y 111.86
(|model bbox max| x Scale_Factor, the port's way). The SD walked in when out of range, so thresholds are bracketed
by the poll interval:

| trial | facing | start d | beam started | retail threshold | port R + E |
|---|---|---|---|---|---|
| ctrl_toward | toward | 950.0 | in place | < 950 | 1023.5 |
| small_toward | toward | 976.9 | in place | > 976.9 | 1023.5 |
| mid_toward | toward | 1112.0 | after closing | **[1054.5, 1057.2]** | 1023.5 |
| ctrl_side | side | 1152.0 | after closing | **[1107.8, 1112.2]** | 1111.9 |
| mid_side | side | 1621.0 | never (min d 1574.7) | - | 1111.9 |
| out_side | side | 1151.9 | never (min d 1133.5) | - | 1111.9 |

- **Footprint confirmed [H]:** the beam starts beyond R (at 1054.5 and 1107.8), so retail does add a target extent.
- *Superseded by the follow-up below:* the first reading ("broadside matches the port, nose-on retail ~R + 55") took
  the kit's facing labels at face value; neither trial had the facing the kit intended.
- **Follow-up (2026-11-10), TACTICAL_COMBAT §5.4.5:** retail's E along the facing is the model's collision-bound Y half
  x Scale_Factor (109.95 here), across it the X half (23.48); ctrl_side was not broadside (|f.u| 0.747 > 0.7071, predicted
  1109.95 in [1107.76, 1110.04]) and mid_toward's 1054.5 is X still turning when the beam was ordered, not an extent [M].
- The two `never` side trials did not close in (the SD did not walk); not a contradiction.

## (B) Combat_Bonus aura reaches an allied player's units (TACTICAL_COMBAT §5.4.3 item 4)
Emperor_Palpatine (Empire, `Health_Bonus` 0.25) spawned at the test site; AT-STs (health 400) of all three players
near him (~156-170), far from him (~2600-2740) and pre-damaged before he arrived; each hit by Lua
`Take_Damage(120)`.

The kit assumed every player's units take Lua damage unscaled; in retail the **Easy AI players' units take 2.5x**
(the difficulty damage modifier), so its OTHER marks are the bonus or no-bonus figure times 2.5:

| unit | drop | reading |
|---|---|---|
| own near / far | 0.24 | bonus (120 / 500) |
| ally (Underworld) near / far | 0.60 = 2.5 x 0.24 | **bonus** |
| enemy (Rebel) near / far | 0.75 = 2.5 x 0.30 | no bonus |
| pre-damaged own 0.70 | -> 0.76 at arrival | current health raised by +100 with the max |
| pre-damaged ally 0.25 | -> 0.40 at arrival | (100 + 100) / 500: **bonus, current raised** |
| pre-damaged enemy 0.25 | unchanged | no bonus |
| own / ally after the Emperor was removed | 0.76 -> 0.95, 0.40 -> 0.50 | max back to 400, current kept |

- **An allied player's units get the aura [H]** (port rule since s78m t05: allied with the source owner, own player
  included). Matches the port.
- **No radius [H]:** units ~2600 away get it too. Matches the port.
- **Units already on the map get it when the source arrives, current health raised [H].** Matches the port.
- **Removing the source removes the bonus, current health kept [H].** Matches the port (removal at Detach).
- Side finding: Easy AI units take 2.5x Lua `Take_Damage`; probes that compare health drops across players must
  correct for the AI difficulty (or use Hard / human-owned controls).
