# s78n t08 probe kit: tractor-beam range footprint (A) and Combat_Bonus aura on an ally (B)

Retail check of two port guesses in `docs/re/TACTICAL_COMBAT.md`:

- **(A) sec. 5.4.5** (second version of the space trials, 2026-11-10): a Star_Destroyer's `TRACTOR_BEAM` may start
  when the XY centre distance `d <= R + E`, `R` = `Activation_Max_Range` (1000), `E` the target's collision-bound
  half size x `Scale_Factor`: the model **Y** half when the target's facing is along the line to the user
  (|f.u| > 0.7071), else the model **X** half. Nebulon_B_Frigate (scale 0.7, collision half 33.54 / 157.07):
  **E along 109.95, across 23.48, thresholds 1109.95 along and 1023.48 across** (computed by `s78n_extent.py
  range_extents` from the install's `RV_NEBULONB.ALO`). The old port values (|max.x| along, |max.y| across:
  1023.48 / 1111.86) are printed beside them.
  The first run (`out/20261109-200249`, `docs/re/S78N_T08_RUN.md`) did not set the target's facing: `Turn_To_Face` was
  still turning at the order and the "side" trial was really |f.u| 0.747. The space trials now spawn, teleport both
  ships to exact spots, turn the target at once with `Face_Immediate`, suspend both locomotors and read the
  target's facing from two model bones until it is settled.
- **(B) sec. 5.4.3 item 4**: a CombatBonus aura (FoC `Combat_Bonus_Ability`) is accepted by every unit whose owner
  is allied to the source's owner (own units included); the port has no aura radius and applies the aura to units
  already on the map when the source arrives (current health raised with the maximum). Answered by the first run
  (all PASS by hand, `S78N_T08_RUN.md`); still available in the full mode.

## Space-only re-run (A), about 5 minutes with the menus, 2 to 3 minutes in battle

From the project root, in a normal PowerShell or cmd window (not a Claude terminal tab, so the game is not a child
of Claude's shell), with the game closed and Steam running:

```
tools\probe_mod\s78n_t08\run_s78n.cmd --space-only --no-launch --non-interactive
```

Then, in a second window (or from a Claude shell):

```
toolchain\venv\Scripts\python.exe -I tools\probe_mod\common\launch_foc.py MODPATH=Mods\S78NProbe
```

Launch the game WINDOWED. Driving steps:

1. Main menu > Single Player > Skirmish > **Space**, map **Kuat**.
2. Slot 1 (you): **Empire**, team 1. Slot 2: **AI Easy**, **Rebel**, team 2. Every other slot **Closed**. Default
   credits and settings. Start.
3. Give NO orders, click nothing on the map (moving the camera is fine), never pause, never change the game speed,
   never save, keep the game window in front (FoC pauses on focus loss).
4. "S78N PROBE: tractor beam trials armed" appears in the first seconds. At 15 s a Star Destroyer (yours) and a
   Nebulon-B (theirs) appear away from both bases, ten times in turn (each pair 6 to 15 s; the camera follows).
5. Wait for "S78N PROBE: tractor beam trials recorded" (about 1.5 to 3 minutes in battle; up to 6 minutes when the
   facing read is off or the old rule holds and the target is stepped a long way), then Esc > Quit and quit the
   game.

The runner then writes `out\<run>\report.txt`, removes the mod, restores the profile and ends with
`S78N_RUN_DONE exit=0`. The land probe is not built in (a land battle with this mod only records SKIP).

## Full run (A + B; one game session, about 10 minutes, hands-off once a battle starts)

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

**A) space, about 3 minutes**
1. Main menu > Single Player > Skirmish > **Space**, map **Kuat**.
2. Slot 1 (you): **Empire**, team 1. Slot 2: **AI** (Easy), **Rebel**, team 2. Every other slot
   **Closed**. Default credits and settings. Start.
3. "S78N PROBE: tractor beam trials armed" appears in the first seconds. At 15 s a Star Destroyer (yours) and a
   Nebulon-B (theirs) appear far from both bases, ten times in turn.
4. Wait for "S78N PROBE: tractor beam trials recorded" (the probe ends on its own), then
   Esc > Quit to the **main menu** (do not quit the game).

**B) land, about 2 minutes**
1. Single Player > Skirmish > **Land**, map **Dagobah** (3 start positions).
2. Slot 1 (you): **Empire**, team 1. Slot 2: **AI Underworld, team 1** (your ally; the lobby refuses two factions
   on one team, then use team 3: the probe calls `Make_Ally`). Slot 3: **AI Rebel, team 2**.
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

(A) per trial (probe `tv`, records `p=tv`): a fresh invulnerable Star_Destroyer U (human, ability autofire off)
and an invulnerable, unarmed, AI-locked Nebulon-B X are spawned (retail's `Spawn_Unit` may put them hundreds of
units off the asked point, as in the first run). 10 frames later both are teleported to exact spots, U at
`user_at`, X at `user_at + (d, 0)`; `U.Face_Immediate(X's spot)`; `X.Face_Immediate(U's spot)` ("along", |f.u| = 1)
or `X.Face_Immediate(X's spot + (0, 600))` ("across", exactly 90 deg, f.u = 0); `Suspend_Locomotor(true)` on X
and on U (no walking: a clean in / out at a fixed d). Every 15 frames X's facing is read from two model bones
(`Get_Bone_Position("missile_target00")` front, `"missile_target01"` rear, both on the long axis of
`RV_NEBULONB.ALO`; the names are taken from the install's model at build time, since a failed engine read can stop
a retail script) and recorded (`FACE`); the order is given when two consecutive reads of both bones and of X agree
within 0.5 (at least 60 frames after placing; `SETTLE why=agree`), else after 600 frames (`timeout` /
`no_facing_read`). The order is the form that worked in the first run, `U.Activate_Ability("TRACTOR_BEAM", X)`
(never a bool, a position or no second argument: those stop a retail script). The `PRE` record holds both
positions, d and the facing (`fu` = |f.u|, `fx`, `fy`, raw bone positions); every poll (`P`) records positions,
d, U's `Is_Ability_Active` / `Is_Ability_Ready`, X's `Is_Under_Effects_Of_Ability`, every 10th poll the facing
again; the first poll with the beam is `ENG` (with the facing). No beam after 150 frames with U still: X is
teleported 1 unit closer per poll (facing kept) and the order repeated (`STEP`), so the threshold is also
bracketed. If the locked user does not start the beam on the first control (950) within 90 frames, the lock is
taken off for good (`ULOCK`) and later trials let U walk in (bracket from its approach, as in the first run).
Trials (R 1000): `ctrl_along` 950, `along_1095` / `1105` / `1115` / `1125` (around 1109.95), `ctrl_across` 950,
`across_1010` / `1018` / `1029` / `1037` (around 1023.48).

(B) observable: `Get_Hull()` (health fraction) after a fixed Lua `Take_Damage(120)` on an AT_ST_Walker (health
400, no shield): the hull drops 0.300 without the aura and 0.240 with it (Emperor_Land_Combat_Bonus Health_Bonus
0.25: maximum 500). Three AT-STs (own, ally, enemy) are damaged first (hull 0.70), then the Emperor spawns at the
site: under the port they rise to 0.76 (current health raised with the maximum). Then three AT-STs near the Emperor
(about 156 / 170 away) and three about 2600 away spawn, are damaged once, and the drop is read. All test units are
AI-locked, unarmed and invulnerable (Lua damage passes invulnerability, sec. 3.6; if the first damage shows no drop
the probe makes them vulnerable and damages again, record INVULN_BLOCKS).

## What the report prints

Per battle a header (`script <sid>: ARMED tb|cb`, DONE, ERR / APIERR count), then:

(A) a header with R, E along / across, the predicted thresholds 1109.95 / 1023.48, the old port's 1023.48 /
1111.86 and the facing bones, then one line per trial, for example

```
trial  4 along_1115   along  d0  1115.00 |f.u| 1.0000 (bones; 0.0 deg off the plan; first read 0.0 deg off; turned 0.00 deg from the settled facing by the order, 0.00 by the beam; settle agree) E 109.95 -> pred OUT; observed OUT, then the beam at d 1109.00 after target stepped: T in [1109.00, 1111.00] at |f.u| 1.0000 -> pred 1109.95 fits (old 1023.48 no): MATCH  [old port E 23.48 -> OUT: differs]
```

d0 and |f.u| are those at the order (f from the bones; "commanded" when there is no facing read); E is the sec.
5.4.5 extent for that |f.u|, pred IN when d0 <= R + E. Observed IN = the beam started with nothing moved (`late`
when after 30 frames); OUT = not at d0, then either the interval [d at the beam, d up to two polls earlier] after
X was stepped (or U walked), checked against R + E at |f.u| of the beam start, or "never" with the smallest d.
MATCH when observation and prediction agree; the old port rule is judged in brackets. A `ULOCK` line says the user
lock was taken off. Then:

- `A0` both controls (950) started in place (else INCONCLUSIVE: the setup failed);
- `A1` target facing: along |f.u| > 0.98, across < 0.17, no turn > 1 deg from the settled facing to the order and
  to the beam start (FAIL lists the trials; ASSUMED without bone reads);
- `A2` every trial MATCHes sec. 5.4.5 (`n / 10 trials`);
- `A3` the intervals from all trials: `along T in [lo, hi)` must hold 1109.95, `across T in [lo, hi)` 1023.48 (each
  also says whether the old port value is in or out);
- `old port rule (along 1023.48 / across 1111.86, for reference): n / 10 trials agree`;
- `VERDICT (A) tractor beam range, TACTICAL_COMBAT 5.4.5 (R + E: along 1109.95 / across 1023.48): PASS | FAIL |
  INCONCLUSIVE (old port 1023.48 / 1111.86: n / 10 trials agree)`: INCONCLUSIVE when A0 fails, FAIL when A2
  fails, PASS when A3 passes and A1 is PASS / ASSUMED, else INCONCLUSIVE. In a space-only run (B) reads
  `NO DATA (space-only run)`.

The first run's records (`p=tb`) are still read with the first version's analysis (`--report-only`).

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
tools\probe_mod\s78n_t08\run_s78n.cmd --dry-run --space-only
tools\probe_mod\s78n_t08\run_s78n.cmd --dry-run
```

The selftest builds the mod twice (full and `--space-only`; install read only), checks E 109.95 / 23.48, the
facing bones and the trial distances, compiles the Lua with stock Lua 5.0.2 and runs the probe under a stubbed
engine (spawns 300 / 200 off the asked point, `Face_Immediate` taking effect 40 frames late, bones from the
model): sec. 5.4.5 with a locked user: (A) PASS, every trial MATCH; the old port rule: (A) FAIL; a lock that blocks
the beam and a walking user: ULOCK reported, (A) PASS; no facing read: A1 ASSUMED, (A) PASS; a land battle with the
space-only mod: SKIP only; port aura model: (B) PASS; owner-only aura with a 300 radius, no alliance and
invulnerability blocking damage: (B) FAIL with both fallbacks reported; main menu: SKIP only. It also rebuilds
the first run's report when `out\20261109-200249\records.log` is there, and ends with `S78N SELFTEST OK`.

## Cleanup

The runner cleans up at the end (also after an error). By hand: `tools\probe_mod\s78n_t08\cleanup_s78n.cmd`
(removes `corruption\Mods\S78NProbe` only if it holds the marker, restores the profile snapshot of the last run).

## Known limits

- (A) retail walks a Star Destroyer toward an out-of-range tractor-beam target (first run); `Suspend_Locomotor`
  should stop that. Untested in retail for this kit: whether a suspended locomotor still lets the beam start (the
  ULOCK fallback covers "no"), whether `Face_Immediate` is instant on a capital ship (the settle wait and A1 cover
  "no"), and `Get_Bone_Position` on these two bones (if it fails, `APIERR api=Get_Bone_Position` appears, every
  trial waits 600 frames and A1 is ASSUMED; if the script stops there, re-run with `--no-bone-read`).
- (A) the bones are read where the game last posed the model; the camera is pointed at each trial so the ships
  are on screen.
- (B) the observable is indirect: health bonus seen through `Get_Hull`. It assumes `Take_Damage` from Lua is not
  scaled by armour (AT-ST vs damage kind 0) and that Health_Bonus multiplies the maximum. Both controls catch a
  wrong assumption (own near must show 0.24, enemy near 0.30); if they fail the verdict is INCONCLUSIVE rather than
  wrong. A Rebel or Underworld AI hero with its own aura near the site would show as OTHER on the controls.
- Spawned objects with `Prevent_AI_Usage` stay where they are; the AI never targets them as they are far from its
  units, but a stray AI attack during the 4 minutes would show as extra HULL changes (B) or a moved X (A).
