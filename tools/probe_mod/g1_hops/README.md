# G1 hop timing (STRATEGIC_GAPS probe G1): one galactic hop in the original game

Question: how long does a fleet take to jump from one planet to the next, in ticks and in seconds? The port needs the
fleet speed multiplier M (STRATEGIC_GAPS 2.1: FoC `Object_Max_Speed_Multiplier_Galactic` 5.0, or 1.0).

## Result so far (2026-10-29): ticks are settled from an existing capture, no new game launch needed

The Q2 reference trace `tools/probe_mod/q2_traces/out/20261027-101057-galactic-vanilla/trace.q2t` (original FoC 1.121,
`Sandbox_Heart_Of_The_Maw_Empire`, hands-off AI play, every tick sampled) contains about 100 hops of the AI fleets.
`analyze_hops.py` reads them (positions per tick, so the speed and the tick count are exact):

| hop | route HS | units | units/tick | ticks | at 30 ticks/s |
|---|---|---|---|---|---|
| **Corellia -> Kuat** | 5 | 72.11 | 2.625 | **26** | **0.87 s** |
| Tatooine -> Corellia | 6 | 87.32 | 3.150 | 26 | 0.87 s |
| Kuat -> Kashyyyk | 5 | 78.10 | 2.625 | 28 | 0.93 s |
| Naboo -> Tatooine | none | 60.21 | 0.525 | 113 | 3.77 s |
| NalHutta -> Shola | none | 84.85 | 0.525 | 160 | 5.33 s |

units/tick / 0.07 = 7.5 (no route), 37.5 (HS 5), 45 (HS 6), 11.25 (Underworld on an unrouted hop, corruption bonus 1.5):
that is Hyperspace_Speed 1.5 x M x R with **M = 5.0**. The 2026-10-28 port value M = 1.0 gave 136 ticks (4.5 s) for
Corellia -> Kuat, 5.2x too slow in ticks. The tests `test_retail_hops` and `test_foc_corellia_kuat_hop`
(engine/galactic/tests/test_economy.cpp) lock the formula and the retail numbers (26 ticks); the port still applies M = 1.0
until `kApplyModeSpeedMultiplier` in engine/galactic/src/strategic.cpp is set to true (that moves shared goldens, so it
waits for Gabriel's decision).

Run it yourself: `toolchain\venv\Scripts\python.exe -I tools\probe_mod\g1_hops\analyze_hops.py <trace.q2t> [--from CORELLIA --to KUAT] [--csv]`
(or double-click `analyze.cmd`). It only reads the trace; nothing is installed or changed, so there is no cleanup.
Hops that straddle a missed sample (the trace lost 577 of 15248 ticks) can show a tick count that is a few too low; the
table above uses hops with unbroken runs (the same hop appears several times and agrees).

## What is not settled: seconds on the clock

The trace has no wall clock. 26 ticks is 0.87 s only if the original galactic mode runs at 30 ticks/s (the default
game-speed index, MAIN_LOOP.md 1.2; the trace's own wall time of about 23.5 ticks/s average, including loading, is
consistent with that). The 2026-10-28 playtest note said the original's hops "take several seconds". If a real stopwatch
disagrees with 0.87 s, the difference is in the pacing or the presentation (not in M).

## For Gabriel (about 5 minutes, only to confirm the seconds)

1. Start Steam, then FoC (`StarWarsG.exe`, no mods). Single Player > Galactic Conquest > **Heart of the Maw**, play as the
   **Empire**. Leave the game speed at its default (Options: galactic game speed in the middle).
2. The Empire starts with a fleet (Tartan Patrol Cruiser + Victory Destroyer) in orbit at **Corellia**. Zoom out so
   Corellia and Kuat are both on screen.
3. Start a stopwatch (phone, or screen recording at 60 fps and count frames) at the moment you click **Kuat** with the
   Corellia fleet selected (the fleet leaves the orbit), stop it when the fleet arrives in Kuat's orbit.
4. Do it twice more (move it back to Corellia, then to Kuat again) and write down the three times.
5. **Send back:** the three times in seconds, and the galactic game-speed setting you used. Expected: about 0.9 s
   (26 ticks at 30 ticks/s, plus the departure animation). A value near 4.5 s would mean M = 1 after all; a value near
   0.9 s confirms the port's tick count and leaves the "several seconds" impression to presentation.

Nothing is installed and nothing needs cleaning up. If you want tick-exact numbers as well, run the Q2 galactic kit
(`tools\probe_mod\q2_traces\run_galactic.cmd`, README there) and move the Corellia fleet to Kuat once during the run, then
`analyze.cmd` its `trace.q2t` (a human order shows up as one more fleet hop in the trace).
