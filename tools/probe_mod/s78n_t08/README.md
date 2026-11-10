# s78n t08 probe kit: tractor-beam range footprint (A) and Combat_Bonus aura on an ally (B)

Retail check of two port guesses in `docs/re/TACTICAL_COMBAT.md`:

- **(A) sec. 5.4.2**: a Star_Destroyer's `TRACTOR_BEAM` may start when the XY centre distance
  `d <= R + E`, with `R` = `Activation_Max_Range` (1000) and `E` the target's range-geometry half-extent: its X
  half-extent when the target's facing is along the line of sight (|f.u| > 0.7071), else its Y half-extent.
  Target Nebulon_B_Frigate (scale 0.7): `E_x` 23.5, `E_y` 111.9 (computed by `s78n_extent.py` the port's way from
  the install's model), so the port's threshold is **1023.5 nose-on ("toward") and 1111.9 broadside ("side")**.
- **(B) sec. 5.4.3 item 4**: a CombatBonus aura (FoC `Combat_Bonus_Ability`) is accepted by every unit whose owner
  is allied to the source's owner (own units included); the port has no aura radius and applies the aura to units
  already on the map when the source arrives (current health raised with the maximum).

## Run it (one game session, about 10 minutes, hands-off once a battle starts)

From the project root, in a normal PowerShell or cmd window (not a Claude terminal tab, so the game is not a child
of Claude's shell), with the game closed:

```
tools\probe_mod\s78n_t08\run_s78n.cmd --no-launch --non-interactive
```

It refuses to start while `StarWarsG.exe` runs, installs `corruption\Mods\S78NProbe` (two files, marker
`S78N_PROBE_MARKER.txt`), snapshots the profile, prints the menu steps and waits up to 15 minutes for the game. Then,
in a second window:

```
toolchain\venv\Scripts\python.exe -I tools\probe_mod\common\launch_foc.py MODPATH=Mods\S78NProbe
```

Launch the game WINDOWED (as for the other kits). The runner attaches, scans the game's memory for the records
(read only) until the game exits, writes the report, removes the mod and restores the profile. Without
`--no-launch` the runner starts the game itself.

### Menu steps (both battles in the same game session)

Both battles: give NO orders, click nothing on the map (moving the camera is fine), never pause, never change the
game speed, never save, keep the game window in front (FoC pauses on focus loss). Probe messages start with
"S78N PROBE:".

**A) space, about 4 minutes**
1. Main menu > Single Player > Skirmish > **Space**, map **Kuat**.
2. Slot 1 (you): **Empire**, team 1. Slot 2: **AI** (any difficulty), **Rebel**, team 2. Every other slot
   **Closed**. Default credits and settings. Start.
3. "S78N PROBE: tractor beam trials armed" appears in the first seconds. At 15 s a Star Destroyer (yours) and a
   Nebulon-B (theirs) appear far from both bases, six times in turn, each pair for 10 to 30 s.
4. Wait for "S78N PROBE: tractor beam trials recorded" (about 4 minutes in; the probe ends on its own), then
   Esc > Quit to the **main menu** (do not quit the game).

**B) land, about 2 minutes**
1. Single Player > Skirmish > **Land**, map **Dagobah** (3 start positions).
2. Slot 1 (you): **Empire**, team 1. Slot 2: **AI Underworld, team 1** (your ally). Slot 3: **AI Rebel, team 2**.
   Every other slot Closed. Default credits and settings. Start.
3. "S78N PROBE: aura trials armed" appears; at 15 s three AT-ST walkers (yours, the Underworld's, the Rebels'), then
   the Emperor (yours), then six more AT-STs appear away from the bases.
4. Wait for "S78N PROBE: aura trials recorded" (about 1 minute in), Esc > Quit, quit the game.

The runner ends with `S78N_RUN_DONE exit=0` and prints the report (also in
`tools\probe_mod\s78n_t08\out\<run>\report.txt`, with `records.log`, `report.json`, `config.json`).

If the skirmish lobby does not let you put the Underworld AI on your team, start B anyway with the three players on
any teams except Empire and Rebel together: the probe calls `Make_Ally` both ways when Empire and Underworld are not
allied and the report says so (B0 then reads "allied by Make_Ally, not the menu").

## How it works

`build_s78n_mod.py` appends `SN_CFG` (generated) and `s78n_probe.lua` to the shipped
`BusyTacticalFreeStore.lua` and adds the cue strings to `MasterTextFile_English.dat`. Only the Rebel AI's free
store arms the probe, and only when `Find_Player("Empire").Is_Human()` (so the main-menu background battle is
skipped), after `Base_Definitions`, from its service calls (ServiceRate 0.05 s while it runs). Records are Lua
strings `S8N|KIND|...|END`, found by a read-only heap scan (`..\a1_tactical\a1mem.py`).

(A) per trial: a fresh invulnerable Star_Destroyer U (human, ability autofire off) and an invulnerable, unarmed,
AI-locked Nebulon-B X at centre distance d along +x. "Toward": X turns to face U on the line. "Side": X spawns
north of U, turns to face U there, then is teleported onto the line (facing kept, so it shows U its side). Then
`U.Activate_Ability("TRACTOR_BEAM", X)`; every poll (3 frames) records both positions, U's
`Is_Ability_Active` / `Is_Ability_Ready` and X's `Is_Under_Effects_Of_Ability`. The first poll with the beam is the
start (ENG). If U has not moved 90 frames after the order, X is stepped 3 units closer per poll and the order is
repeated, so a threshold is still crossed slowly. Trials (R 1000): `ctrl_toward` 950, `small_toward` 1011.7
(R + E_x/2), `mid_toward` 1067.7, `ctrl_side` 950, `mid_side` 1067.7 (R + (E_x + E_y)/2), `out_side` 1151.9
(R + E_y + 40).

(B) observable: `Get_Hull()` (health fraction) after a fixed Lua `Take_Damage(120)` on an AT_ST_Walker (health
400, no shield): the hull drops 0.300 without the aura and 0.240 with it (Emperor_Land_Combat_Bonus Health_Bonus
0.25: maximum 500). Three AT-STs (own, ally, enemy) are damaged first (hull 0.70), then the Emperor spawns at the
site: under the port they rise to 0.76 (current health raised with the maximum). Then three AT-STs near the Emperor
(about 156 / 170 away) and three about 2600 away spawn, are damaged once, and the drop is read. All test units are
AI-locked, unarmed and invulnerable (Lua damage passes invulnerability, sec. 3.6; if the first damage shows no drop
the probe makes them vulnerable and damages again, record INVULN_BLOCKS).

## What the report prints

Per battle a header (`script <sid>: ARMED tb|cb`, DONE, ERR / APIERR count), then:

(A) one line per trial, for example

```
trial 3 mid_toward toward d0 1067.67 ...: started after closing in: T in [1022.67, 1028.67] (r 132, user moved 0.00, target stepped) -> port R+E 1023.48: PASS
```

IN_PLACE = the beam started within 15 frames of the order at the starting distance (so retail's threshold is at
least d0); CLOSED = it started after U walked or X was stepped, with the interval that holds retail's threshold
(one poll of latency allowed); NONE = never (threshold below the smallest d). PASS when the port's R + E for that
facing fits. Then:

- `A0` both controls (R - 50) started in place (else INCONCLUSIVE: the setup failed);
- `A1` footprint: the beam started beyond R (PASS), or never and the threshold is under R + E (FAIL: centre
  distance only), and never beyond R + max(E) + 1;
- `A2` every trial agrees with the port;
- `A3` fitted thresholds `T toward in [lo, hi)` and `T side in [lo, hi)` vs R + E_x / R + E_y (FAIL "swapped" if
  retail uses the other half-extent per facing).
- `VERDICT (A) ...: PASS | FAIL | INCONCLUSIVE`.

(B) the alliance state (`Is_Ally` both ways, and whether Make_Ally was needed), the Emperor spawn, then one line per
unit (`near_/far_/pre_` x `own/ally/enemy`): hull before and after the damage, the drop, BONUS / NONE / OTHER
(tolerance 0.015); for the `pre_` units the hull before / after the Emperor arrived: BONUS (0.70 -> 0.76),
BONUS_MAX_ONLY (0.70 -> 0.56), NONE. Then:

- `B0` setup: allied, the Emperor spawned, own near BONUS (positive control), enemy near NONE (negative control);
- `B1` the ally's near unit gets the bonus: **the question**; PASS = the port is right;
- `B2` radius: far own / far ally BONUS = no radius (port); NONE = retail has a radius;
- `B3` arrival: pre own / pre ally rise to 0.76 (port); BONUS_MAX_ONLY or NONE differ.
- `VERDICT (B) ...: PASS | FAIL | INCONCLUSIVE` (INCONCLUSIVE when B0 fails).

`run_s78n.cmd --report-only <records.log>` rebuilds the report; `s78n_analyse.py <records.log> [--config
config.json]` prints it.

## Offline checks (no game)

```
toolchain\venv\Scripts\python.exe -I tools\probe_mod\s78n_t08\selftest_s78n.py
tools\probe_mod\s78n_t08\run_s78n.cmd --dry-run
```

The selftest builds the mod (install read only), compiles the Lua with stock Lua 5.0.2, runs the probe under a
stubbed engine (port model: (A) and (B) PASS; centre-only beam with a walking user: (A) FAIL; owner-only aura with a
300 radius, no alliance and invulnerability blocking damage: (B) FAIL with both fallbacks reported; main menu: SKIP
only) and ends with `S78N SELFTEST OK`.

## Cleanup

The runner cleans up at the end (also after an error). By hand: `tools\probe_mod\s78n_t08\cleanup_s78n.cmd`
(removes `corruption\Mods\S78NProbe` only if it holds the marker, restores the profile snapshot of the last run).

## Known limits

- (A) whether retail walks a Star Destroyer toward an out-of-range tractor-beam target is unknown; both cases are
  handled (walk: interval from U's approach; no walk: X stepped closer). Facing comes from `Turn_To_Face` and a
  teleport that keeps the facing; if retail's Teleport resets the facing, the side trials measure the toward rule
  and A3 reports "swapped" or INCONCLUSIVE (check the `TURN` / `TELE` lines and the X positions in records.log).
- (B) the observable is indirect: health bonus seen through `Get_Hull`. It assumes `Take_Damage` from Lua is not
  scaled by armour (AT-ST vs damage kind 0) and that Health_Bonus multiplies the maximum. Both controls catch a
  wrong assumption (own near must show 0.24, enemy near 0.30); if they fail the verdict is INCONCLUSIVE rather than
  wrong. A Rebel or Underworld AI hero with its own aura near the site would show as OTHER on the controls.
- Spawned objects with `Prevent_AI_Usage` stay where they are; the AI never targets them as they are far from its
  units, but a stray AI attack during the 4 minutes would show as extra HULL changes (B) or a moved X (A).
